#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include "Engine/Assets/MeshCache.h"
#include "Engine/Core/ProjectRoot.h"
#include "Engine/Scene/LevelAssetLoader.h"
#include "Engine/Scene/LevelDependencyPlan.h"
#include "Engine/Scene/SceneLoader.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

void PrintUsage()
{
    std::printf(
        "Usage: FoxCook [--force] [scene.json | mesh-path]...\n"
        "\n"
        "Cooks imported meshes into the derived-data cache (%s/*.fxmesh).\n"
        "Paths are relative to the project root. With no inputs, every scene in\n"
        "Assets/Scenes is cooked.\n"
        "\n"
        "  --force     re-cook entries that are already up to date\n"
        "  -h, --help  show this help\n",
        SE::MeshCache::k_defaultDirectory);
}

bool IsSceneFile(const std::string& path)
{
    std::string extension = fs::path(path).extension().string();
    for (char& c : extension)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return extension == ".json";
}

double SecondsSince(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

struct CookJob
{
    std::string path;
    SE::MeshCookResult result = SE::MeshCookResult::Failed;
    std::string error;
    double seconds = 0.0;
};

} // namespace

int main(int argc, char** argv)
{
    bool force = false;
    std::vector<std::string> inputs;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h")
        {
            PrintUsage();
            return 0;
        }
        if (arg == "--force")
            force = true;
        else if (!arg.empty() && arg[0] == '-')
        {
            std::fprintf(stderr, "FoxCook: unknown option '%s'\n\n", arg.c_str());
            PrintUsage();
            return 1;
        }
        else
            inputs.push_back(arg);
    }

    if (!SE::SetProjectRootCwd())
    {
        std::fprintf(stderr, "FoxCook: project root (Engine/ + Assets/) not found\n");
        return 1;
    }
    if (inputs.empty())
        inputs = SE::SceneLoader::ScanSceneDirectory("Assets/Scenes");

    const auto start = Clock::now();
    size_t failed = 0;
    std::vector<CookJob> jobs;
    std::unordered_set<std::string> seen;
    const auto addMesh = [&](const std::string& path) {
        if (!seen.insert(path).second) return;
        CookJob job;
        job.path = path;
        jobs.push_back(std::move(job));
    };
    for (const std::string& input : inputs)
    {
        if (!IsSceneFile(input))
        {
            addMesh(SE::LevelDependencyPlan::NormalizePath(input));
            continue;
        }
        const auto result = SE::LevelAssetLoader::LoadFromFile(input);
        if (!result.Succeeded())
        {
            std::printf("[failed]     %s: level parse failed\n", input.c_str());
            ++failed;
            continue;
        }
        for (const auto& dependency : SE::LevelDependencyPlan::Build(result.asset).dependencies)
            if (dependency.kind == SE::LevelDependencyKind::Mesh)
                addMesh(dependency.path);
    }

    const SE::MeshCache cache;
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
                const auto jobStart = Clock::now();
                try
                {
                    job.result = cache.Cook(job.path, force, &job.error);
                }
                catch (const std::exception& exception)
                {
                    job.result = SE::MeshCookResult::Failed;
                    job.error = exception.what();
                }
                job.seconds = SecondsSince(jobStart);
            }
        });
    }
    for (auto& worker : workers) worker.join();

    size_t cooked = 0;
    size_t upToDate = 0;
    for (const CookJob& job : jobs)
    {
        const std::string entry = cache.EntryPath(job.path);
        switch (job.result)
        {
        case SE::MeshCookResult::Cooked:
        {
            std::error_code ec;
            const auto bytes = fs::file_size(entry, ec);
            const double megabytes = ec ? 0.0 : static_cast<double>(bytes) / (1024.0 * 1024.0);
            std::printf("[cooked]     %s  (%.1f s) -> %s (%.1f MB)\n",
                        job.path.c_str(), job.seconds, entry.c_str(), megabytes);
            ++cooked;
            break;
        }
        case SE::MeshCookResult::UpToDate:
            std::printf("[up-to-date] %s -> %s\n", job.path.c_str(), entry.c_str());
            ++upToDate;
            break;
        case SE::MeshCookResult::Failed:
            std::printf("[failed]     %s: %s\n", job.path.c_str(), job.error.c_str());
            ++failed;
            break;
        }
    }
    std::printf("FoxCook: %zu cooked, %zu up to date, %zu failed in %.1f s\n",
                cooked, upToDate, failed, SecondsSince(start));
    return failed > 0 ? 1 : 0;
}
