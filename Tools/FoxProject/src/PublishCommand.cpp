#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <exception>
#include <map>
#include <set>
#include <thread>
#include <nlohmann/json.hpp>
#include "Common.h"
#include "Engine/Assets/MeshCache.h"
#include "Engine/Core/Project.h"
#include "Engine/Core/ProjectRoot.h"
#include "Engine/Scene/LevelAssetLoader.h"
#include "Engine/Scene/LevelDependencyPlan.h"
#include "Engine/Scene/MeshMaterialUtil.h"
#include "Engine/Scene/SceneLoader.h"
#include "Engine/Scripting/ScriptSystem.h"

#ifndef FOX_PLAYER_EXE
#error "FOX_PLAYER_EXE must name the Player executable (see Tools/FoxProject/CMakeLists.txt)"
#endif

namespace fs = std::filesystem;

namespace FoxProject {

namespace {

constexpr const char* k_presetDirectory = "Assets/Particles/Presets";

bool IsMeshSource(const std::string& path)
{
    static const char* const k_extensions[] = {
        ".fbx", ".gltf", ".glb", ".obj", ".dae", ".3ds", ".blend", ".ply", ".stl", ".x",
    };
    const std::string extension = ToLower(fs::path(path).extension().string());
    return std::any_of(std::begin(k_extensions), std::end(k_extensions),
                       [&](const char* candidate) { return extension == candidate; });
}

// A Windows file name: invalid characters become '_', no trailing dots or
// spaces, no reserved device names.
std::string SanitizeFileName(const std::string& name)
{
    std::string result;
    for (const char c : name)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        result += (u < 32 || std::string("<>:\"/\\|?*").find(c) != std::string::npos) ? '_' : c;
    }
    while (!result.empty() && (result.back() == '.' || result.back() == ' '))
        result.pop_back();
    while (!result.empty() && result.front() == ' ')
        result.erase(result.begin());
    if (result.empty())
        return "Game";
    std::string upper = result;
    for (char& c : upper)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    static const char* const k_reserved[] = {
        "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8",
        "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };
    if (std::any_of(std::begin(k_reserved), std::end(k_reserved),
                    [&](const char* reserved) { return upper == reserved; }))
        result += '_';
    return result;
}

// True when path is base or lies below it.
bool IsSameOrInside(const fs::path& path, const fs::path& base)
{
    const fs::path relative = path.lexically_relative(base);
    return !relative.empty() && *relative.begin() != fs::path("..");
}

struct MeshRequest
{
    std::string owner;   // file that references the mesh
    std::string where;   // JSON pointer inside owner, may be empty
    bool required = false;
};

// Everything the published build contains, keyed by output-relative path.
struct PublishSet
{
    std::map<std::string, std::string> files;     // destination -> source
    std::map<std::string, std::string> generated; // destination -> contents
    size_t errors = 0;
    size_t warnings = 0;

    void Add(const std::string& source, const std::string& destination)
    {
        files.emplace(destination, source);
    }

    // A project file shipped at the same relative path. Missing optional
    // files are warnings: the engine falls back to defaults for them.
    void AddProjectFile(const std::string& path, const std::string& owner, const std::string& where, bool required)
    {
        const std::string normalized = NormalizePath(path);
        if (normalized.empty())
            return;
        if (IsRegularFile(normalized))
        {
            Add(normalized, normalized);
            return;
        }
        Report(required ? Severity::Error : Severity::Warning, owner, where,
               "'" + normalized + "' does not exist" + (required ? "" : "; the published build will use a fallback"));
    }

    void Report(Severity severity, const std::string& owner, const std::string& where, const std::string& message)
    {
        PrintDiagnostic(severity, owner, where.empty() ? message : where + ": " + message);
        ++(severity == Severity::Error ? errors : warnings);
    }
};

class DependencyCollector
{
public:
    explicit DependencyCollector(PublishSet& set) : m_set(set) {}

    void AddScene(const std::string& rawPath, const std::string& owner)
    {
        const std::string path = NormalizePath(rawPath);
        if (path.empty() || !m_scenes.insert(path).second)
            return;
        if (!IsRegularFile(path))
        {
            m_set.Report(Severity::Error, owner, {}, "scene '" + path + "' does not exist");
            return;
        }
        m_set.Add(path, path);

        const SE::LevelAssetLoadResult result = SE::LevelAssetLoader::LoadFromFile(path);
        if (!result.Succeeded())
        {
            m_set.Report(Severity::Error, path, {}, "scene failed to load (run 'FoxProject check' for details)");
            return;
        }
        for (const SE::LevelDependency& dependency : SE::LevelDependencyPlan::Build(result.asset).dependencies)
        {
            if (dependency.kind == SE::LevelDependencyKind::Mesh)
                AddMesh(dependency.path, { path, dependency.owner, dependency.required });
            else
                m_set.AddProjectFile(dependency.path, path, dependency.owner, dependency.required);
        }
        const auto& entities = result.asset.entities;
        for (size_t e = 0; e < entities.size(); ++e)
            for (size_t s = 0; s < entities[e].scripts.size(); ++s)
                m_set.AddProjectFile(entities[e].scripts[s].script, path,
                                     "/entities/" + std::to_string(e) + "/scripts/" + std::to_string(s) + "/script",
                                     true);
    }

    void AddMesh(const std::string& rawPath, MeshRequest request)
    {
        const std::string path = NormalizePath(rawPath);
        if (!path.empty())
            m_meshes.emplace(path, std::move(request));
    }

    // A vehicle definition ({"layout": ..., "visual": {...}}) references its
    // layout, tires, aero and visual meshes relative to the definition; ship
    // all of them. Returns false when the file is not a vehicle definition.
    bool AddVehicleDefinition(const std::string& path)
    {
        std::string text;
        if (!ReadTextFile(path, text))
            return false;
        nlohmann::json root;
        try
        {
            root = nlohmann::json::parse(text, nullptr, true, true);
        }
        catch (const nlohmann::json::exception&)
        {
            return false;
        }
        if (!root.is_object() || !root.contains("layout") || !root.contains("visual"))
            return false;

        m_set.Add(path, path);
        const fs::path directory = fs::path(path).parent_path();
        auto resolve = [&directory](const std::string& relative) {
            return NormalizePath((directory / relative).generic_string());
        };
        try
        {
            for (const char* file : { "layout", "tires", "aero" })
                m_set.AddProjectFile(resolve(root.at(file).get<std::string>()), path, std::string("/") + file, true);
            const nlohmann::json& visual = root.at("visual");
            AddMesh(resolve(visual.at("body").get<std::string>()), { path, "/visual/body", true });
            static const char* k_wheelKeys[4] = { "fl", "fr", "rl", "rr" };
            for (const char* part : { "wheels", "spindles" })
            {
                if (!visual.contains(part))
                    continue;
                for (int w = 0; w < 4; ++w)
                    AddMesh(resolve(visual.at(part).at(k_wheelKeys[w]).get<std::string>()),
                            { path, "/visual/" + std::string(part) + "/" + k_wheelKeys[w], true });
            }
        }
        catch (const nlohmann::json::exception& exception)
        {
            m_set.Report(Severity::Error, path, {}, std::string("invalid vehicle definition: ") + exception.what());
        }
        return true;
    }

    void AddParticlePresets()
    {
        for (const std::string& path : ListFilesRecursive(k_presetDirectory))
        {
            m_set.Add(path, path);
            if (ToLower(fs::path(path).extension().string()) != ".json")
                continue;
            SE::SceneDescriptor::ParticleEmitterDesc preset;
            if (!SE::SceneLoader::LoadParticlePreset(path, preset))
            {
                m_set.Report(Severity::Warning, path, {}, "particle preset failed to load; shipped without its texture");
                continue;
            }
            m_set.AddProjectFile(preset.texture, path, "/emitter/texture", false);
        }
    }

    // Copies the quoted string starting at text[quote]; returns the index of
    // the closing quote (or text.size() when unterminated at a line end).
    static size_t ReadQuotedLiteral(const std::string& text, size_t quote, std::string& literal)
    {
        literal.clear();
        size_t j = quote + 1;
        for (; j < text.size() && text[j] != text[quote] && text[j] != '\n'; ++j)
        {
            if (text[j] == '\\' && j + 1 < text.size())
                ++j;
            literal += text[j];
        }
        return j;
    }

    void AddScriptLiteral(const std::string& literal, const std::string& owner)
    {
        if (!StartsWith(literal, "Assets/"))
            return;
        const std::string path = NormalizePath(literal);
        if (!IsRegularFile(path))
            return;
        const bool isScene = StartsWith(path, std::string(k_scenesDirectory) + "/") &&
                             ToLower(fs::path(path).extension().string()) == ".json";
        if (isScene)
            AddScene(path, owner);
        else if (IsMeshSource(path))
            AddMesh(path, { owner, {}, true });
        else if (!AddVehicleDefinition(path))
            m_set.Add(path, path);
    }

    // Asset paths written as string literals in scripts ("Assets/Meshes/Crate.glb",
    // a scene passed to fox.scene.load) ship with their dependencies.
    void ScanScriptLiterals(const std::string& scriptPath)
    {
        std::string text;
        if (!ReadTextFile(scriptPath, text))
            return;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] != '"' && text[i] != '\'')
                continue;
            std::string literal;
            i = ReadQuotedLiteral(text, i, literal);
            AddScriptLiteral(literal, scriptPath);
        }
    }

    // Ships each mesh as its cooked cache entry (MeshCache loads entries whose
    // source is absent) plus the material textures the level loader resolves.
    void ResolveMeshes()
    {
        const SE::MeshCache cache;
        struct CookJob
        {
            std::string path;
            SE::MeshCookResult result = SE::MeshCookResult::Failed;
            std::string error;
        };
        std::vector<CookJob> jobs;
        for (const auto& mesh : m_meshes)
            if (IsRegularFile(mesh.first))
                jobs.push_back({ mesh.first, SE::MeshCookResult::Failed, {} });

        std::atomic<size_t> cursor { 0 };
        const size_t hardware = (std::max)(1u, std::thread::hardware_concurrency());
        const size_t workerCount = (std::min)(hardware, jobs.size());
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (size_t w = 0; w < workerCount; ++w)
        {
            workers.emplace_back([&] {
                for (;;)
                {
                    const size_t index = cursor.fetch_add(1);
                    if (index >= jobs.size()) return;
                    CookJob& job = jobs[index];
                    try
                    {
                        job.result = cache.Cook(job.path, false, &job.error);
                    }
                    catch (const std::exception& exception)
                    {
                        job.result = SE::MeshCookResult::Failed;
                        job.error = exception.what();
                    }
                }
            });
        }
        for (std::thread& worker : workers)
            worker.join();

        std::set<std::string> failedCooks;
        for (const CookJob& job : jobs)
        {
            if (job.result == SE::MeshCookResult::Cooked)
                std::printf("publish: cooked %s -> %s\n", job.path.c_str(), cache.EntryPath(job.path).c_str());
            else if (job.result == SE::MeshCookResult::Failed)
            {
                const MeshRequest& request = m_meshes.at(job.path);
                m_set.Report(request.required ? Severity::Error : Severity::Warning, request.owner, request.where,
                             "cannot cook mesh '" + job.path + "': " + job.error);
                failedCooks.insert(job.path);
            }
        }

        for (const auto& mesh : m_meshes)
        {
            const std::string& path = mesh.first;
            const MeshRequest& request = mesh.second;
            if (failedCooks.count(path) != 0)
                continue;
            SE::MeshSourceData data;
            std::string error;
            if (!cache.Load(path, data, {}, &error))
            {
                m_set.Report(request.required ? Severity::Error : Severity::Warning, request.owner, request.where,
                             IsRegularFile(path) ? "cannot load mesh '" + path + "': " + error
                                                 : "mesh '" + path + "' does not exist and has no cooked cache entry");
                continue;
            }
            const std::string entry = cache.EntryPath(path);
            m_set.Add(entry, entry);

            size_t missing = 0;
            std::string firstMissing;
            std::set<std::string> textures;
            for (const SE::MeshSubMeshSource& subMesh : data.subMeshes)
                SE::ForEachTexturePath(subMesh.material, [&](const std::string& raw) {
                    textures.insert(SE::MaterialTexturePath(data.directory, raw));
                });
            for (const std::string& texture : textures)
            {
                if (IsRegularFile(texture))
                    m_set.Add(texture, texture);
                else if (missing++ == 0)
                    firstMissing = texture;
            }
            if (missing > 0)
                m_set.Report(Severity::Warning, path, {},
                             std::to_string(missing) + " material texture(s) not found (first: '" + firstMissing +
                             "'); the engine uses default textures for them");
        }
    }

private:
    PublishSet& m_set;
    std::set<std::string> m_scenes;
    std::map<std::string, MeshRequest> m_meshes;
};

bool PublishedProjectJson(std::string& out, std::string& error)
{
    std::string text;
    if (!ReadTextFile(SE::Project::k_fileName, text))
    {
        error = "cannot read";
        return false;
    }
    nlohmann::json root;
    try
    {
        root = nlohmann::json::parse(text, nullptr, /*allow_exceptions*/ true, /*ignore_comments*/ true);
    }
    catch (const nlohmann::json::parse_error& exception)
    {
        error = std::string("invalid JSON: ") + exception.what();
        return false;
    }
    if (!root.is_object())
    {
        error = "expected an object";
        return false;
    }
    root["published"] = true;
    out = root.dump(2) + "\n";
    return true;
}

struct CopyStats
{
    size_t copied = 0;
    size_t skipped = 0;
    size_t failed = 0;
    uintmax_t totalBytes = 0;
    uintmax_t copiedBytes = 0;
};

void CopyFiles(const PublishSet& set, const fs::path& output, bool force, CopyStats& stats)
{
    for (const auto& file : set.files)
    {
        const fs::path source(file.second);
        const fs::path target = output / fs::path(file.first);
        std::error_code ec;
        const uintmax_t size = fs::file_size(source, ec);
        const fs::file_time_type writeTime = ec ? fs::file_time_type() : fs::last_write_time(source, ec);
        if (ec)
        {
            PrintDiagnostic(Severity::Error, file.second, "cannot read: " + ec.message());
            ++stats.failed;
            continue;
        }
        stats.totalBytes += size;

        if (!force)
        {
            std::error_code targetError;
            const bool sameSize = fs::file_size(target, targetError) == size && !targetError;
            const bool sameTime = sameSize && fs::last_write_time(target, targetError) == writeTime && !targetError;
            if (sameTime)
            {
                ++stats.skipped;
                continue;
            }
        }

        fs::create_directories(target.parent_path(), ec);
        if (!ec)
            fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            PrintDiagnostic(Severity::Error, target.generic_string(), "cannot copy from '" + file.second + "': " + ec.message());
            ++stats.failed;
            continue;
        }
        // The size + mtime pair is the incremental key of the next publish.
        fs::last_write_time(target, writeTime, ec);
        ++stats.copied;
        stats.copiedBytes += size;
    }

    for (const auto& file : set.generated)
    {
        const std::string target = (output / fs::path(file.first)).string();
        stats.totalBytes += file.second.size();
        std::string existing;
        if (!force && ReadTextFile(target, existing) && existing == file.second)
        {
            ++stats.skipped;
            continue;
        }
        std::string error;
        if (!WriteTextFile(target, file.second, &error))
        {
            PrintDiagnostic(Severity::Error, target, error);
            ++stats.failed;
            continue;
        }
        ++stats.copied;
        stats.copiedBytes += file.second.size();
    }
}

} // namespace

int CommandPublish(ToolContext& context, const std::vector<std::string>& args)
{
    std::string outArgument;
    bool clean = false;
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == "--out")
        {
            if (i + 1 >= args.size())
                return UsageError("publish", "--out expects a directory");
            outArgument = args[++i];
        }
        else if (args[i] == "--clean")
            clean = true;
        else
            return UsageError("publish", "unexpected argument '" + args[i] + "'");
    }

    const CheckReport report = RunCheck(context);
    if (report.errors > 0)
    {
        std::printf("publish: aborted: check reported %zu error(s)\n", report.errors);
        return k_exitFailure;
    }

    const SE::ProjectSettings project = SE::Project::Load().project;
    const std::string name = SanitizeFileName(project.name);
    std::error_code rootError;
    const fs::path root = fs::weakly_canonical(fs::current_path(), rootError);
    std::error_code ec;
    fs::path output = outArgument.empty() ? root / "out" / "Publish" / name
                    : fs::path(outArgument).is_absolute() ? fs::path(outArgument)
                                                          : context.invocationDirectory / outArgument;
    output = fs::weakly_canonical(output, ec);
    if (ec)
        return UsageError("publish", "invalid output directory '" + outArgument + "': " + ec.message());
    // The output is deleted by --clean and must never contain the sources it copies.
    if (IsSameOrInside(root, output))
        return UsageError("publish", "output '" + output.generic_string() + "' contains the project");
    const fs::path engineRoot = fs::weakly_canonical(SE::GetEngineRoot(), ec);
    if (ec)
        return UsageError("publish", "cannot resolve engine root: " + ec.message());
    if (IsSameOrInside(engineRoot, output))
        return UsageError("publish", "output '" + output.generic_string() + "' contains the engine root");
    for (const char* source : { "Assets", "Cache" })
        if (IsSameOrInside(output, root / source))
            return UsageError("publish", "output '" + output.generic_string() + "' is inside the project's " + source + "/");
    if (IsSameOrInside(output, engineRoot / "Engine"))
        return UsageError("publish", "output '" + output.generic_string() + "' is inside the engine sources");

    PublishSet set;

    const fs::path player(FOX_PLAYER_EXE);
    if (!fs::is_regular_file(player, ec))
        set.Report(Severity::Error, player.generic_string(), {},
                   "player executable not found; build the Player target for this configuration");
    else
    {
        set.Add(player.generic_string(), name + ".exe");
        for (fs::directory_iterator it(player.parent_path(), ec), end; !ec && it != end; it.increment(ec))
        {
            std::error_code entryError;
            if (it->is_regular_file(entryError) && ToLower(it->path().extension().string()) == ".dll")
                set.Add(it->path().generic_string(), it->path().filename().string());
        }
#ifdef SE_DEBUG
        // FOX_PLAYER_EXE comes from this tool's own configuration.
        set.Report(Severity::Warning, player.generic_string(), {},
                   "Debug player: it opens a log console and needs the Visual Studio debug runtimes, "
                   "which other machines lack; publish with a Release build of FoxProject to distribute");
#endif
    }

    for (const std::string& path : ListFilesRecursive(SE::EnginePath("Engine/Shaders").generic_string()))
        set.Add(path, fs::path(path).lexically_relative(engineRoot).generic_string());
    const std::string stubDirectory = SE::EnginePath(SE::ScriptSystem::k_apiStubPath).parent_path().generic_string() + "/";
    std::vector<std::string> scripts;
    for (const std::string& path : ListFilesRecursive(SE::EnginePath("Engine/Scripts").generic_string()))
        if (!StartsWith(path, stubDirectory))
        {
            scripts.push_back(path);
            set.Add(path, fs::path(path).lexically_relative(engineRoot).generic_string());
        }
    for (const std::string& path : ListFilesRecursive(k_scriptsDirectory))
        if (!IsTestScript(path))
        {
            scripts.push_back(path);
            set.Add(path, path);
        }

    DependencyCollector collector(set);
    for (const std::string& scene : SE::Project::SceneList(project))
        collector.AddScene(scene, SE::Project::k_fileName);
    for (const std::string& scene : report.referencedScenes)
        collector.AddScene(scene, SE::Project::k_fileName);
    for (const std::string& path : scripts)
        if (EndsWith(path, ".lua"))
            collector.ScanScriptLiterals(path);
    collector.AddParticlePresets();
    collector.ResolveMeshes();

    std::string projectJson;
    std::string projectError;
    if (PublishedProjectJson(projectJson, projectError))
        set.generated.emplace(SE::Project::k_fileName, std::move(projectJson));
    else
        set.Report(Severity::Error, SE::Project::k_fileName, {}, projectError);

    if (set.errors > 0)
    {
        std::printf("publish: aborted: %zu error(s), %zu warning(s) while collecting files\n", set.errors, set.warnings);
        return k_exitFailure;
    }

    if (clean)
    {
        fs::remove_all(output, ec);
        if (ec)
        {
            PrintDiagnostic(Severity::Error, output.generic_string(), "cannot delete for --clean: " + ec.message());
            return k_exitFailure;
        }
    }
    // Standalone builds keep engine resources beside the project manifest.
    fs::create_directories(output / "Engine", ec);
    fs::create_directories(output / "Assets", ec);

    CopyStats stats;
    CopyFiles(set, output, clean, stats);
    std::printf("publish: %zu file(s) copied, %zu up to date, %zu failed, %zu warning(s); %llu bytes (%.1f MB) total, %llu bytes copied -> %s\n",
                stats.copied, stats.skipped, stats.failed, set.warnings,
                static_cast<unsigned long long>(stats.totalBytes),
                static_cast<double>(stats.totalBytes) / (1024.0 * 1024.0),
                static_cast<unsigned long long>(stats.copiedBytes), output.generic_string().c_str());
    return stats.failed > 0 ? k_exitFailure : k_exitSuccess;
}

} // namespace FoxProject
