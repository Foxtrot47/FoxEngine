#include "Engine/Assets/MeshCache.h"
#include "Engine/Core/Logger.h"

#include <windows.h>
#include <zstd.h>
#include <assimp/version.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace SE {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr uint32_t k_meshCacheVersion = 10;
constexpr uint32_t k_blockSize = 256 * 1024;
constexpr int k_compressionLevel = 3;
constexpr uint64_t k_ioChunk = 16ull * 1024 * 1024;
constexpr char k_magic[4] = { 'F', 'X', 'M', 'C' };

static_assert(sizeof(MeshVertex) == 76, "MeshVertex layout is part of the .fxmesh format");

// On-disk layout (little-endian): FileHeader, DependencyRecord[dependencyCount],
// SubMeshRecord[subMeshCount], uint32_t storedSize[vertexBlockCount + indexBlockCount],
// string blob, then vertex blocks followed by index blocks. A block whose stored
// size equals its raw size is stored uncompressed; smaller means zstd.
struct StringRef { uint32_t offset, length; };

struct FileHeader
{
    char magic[4];
    uint32_t version;
    uint32_t blockSize;
    uint32_t importerVersion[4]; // ImporterVersion(): importer id, then its version
    uint32_t dependencyCount, subMeshCount;
    uint32_t vertexCount, indexCount;
    uint32_t vertexBlockCount, indexBlockCount;
    uint32_t stringBytes;
    StringRef sourceKey;
    float boundsMin[3], boundsMax[3];
    uint64_t payloadBytes;
};

struct DependencyRecord
{
    StringRef path;
    uint64_t size;
    int64_t writeTime; // file_time_type::time_since_epoch().count()
};

// One material texture: path, its UV transform (uv * scale + offset) and UV set.
struct SlotRecord
{
    StringRef path;
    float scale[2], offset[2];
    uint8_t uvSet, pad[3];
};

// Slot order: albedo, normal, roughness, emissive, occlusion, detail albedo, detail normal.
constexpr size_t k_slotCount = 7;
// Terrain slot order: layer albedo A-D, layer normal A-D, splat, overlay.
constexpr size_t k_terrainSlotCount = 2 * k_maxTerrainLayers + 2;

struct SubMeshRecord
{
    uint32_t firstVertex, vertexCount, firstIndex, indexCount;
    SlotRecord slots[k_slotCount];
    float alphaCutoff, roughnessFactor, glossinessFactor;
    float occlusionStrength, detailAlbedoStrength, detailNormalStrength;
    uint8_t alphaMode, workflow, occlusionChannel, vertexTint;
    uint8_t detailAlbedoBlend, kind, terrainLayerCount, terrainAlbedoAlphaIsRoughness;
    float terrainLayerScale[k_maxTerrainLayers];
    float terrainOverlayStrength, terrainRoughnessScale;
    SlotRecord terrainSlots[k_terrainSlotCount];
    float clearcoat, clearcoatRoughness;
    uint8_t clearcoatMaskFromAlbedoAlpha, clearcoatPad[3];
    SlotRecord clearcoatSlots[2];
    float boundsMin[3], boundsMax[3]; // mesh space, this submesh only
};

static_assert(sizeof(StringRef) == 8, ".fxmesh StringRef size");
static_assert(sizeof(FileHeader) == 96, ".fxmesh FileHeader size");
static_assert(sizeof(DependencyRecord) == 24, ".fxmesh DependencyRecord size");
static_assert(sizeof(SlotRecord) == 28, ".fxmesh SlotRecord size");
static_assert(sizeof(SubMeshRecord) == 640, ".fxmesh SubMeshRecord size");

// SubMeshInfo slots in SubMeshRecord::slots order (const or mutable pointers).
template <typename Info>
auto Slots(Info& info)
{
    return std::array { &info.albedo, &info.normal, &info.roughness, &info.emissive,
                        &info.occlusion, &info.detailAlbedo, &info.detailNormal };
}

// TerrainMaterial slots in SubMeshRecord::terrainSlots order.
template <typename Terrain>
auto TerrainSlots(Terrain& terrain)
{
    return std::array { &terrain.layerAlbedo[0], &terrain.layerAlbedo[1], &terrain.layerAlbedo[2],
                        &terrain.layerAlbedo[3], &terrain.layerNormal[0], &terrain.layerNormal[1],
                        &terrain.layerNormal[2], &terrain.layerNormal[3], &terrain.splat, &terrain.overlay };
}

void WriteSlot(SlotRecord& record, const TextureSlot& slot, StringRef path)
{
    record.path = path;
    record.scale[0] = slot.scale[0];
    record.scale[1] = slot.scale[1];
    record.offset[0] = slot.offset[0];
    record.offset[1] = slot.offset[1];
    record.uvSet = slot.uvSet;
}

void ReadSlot(const SlotRecord& record, TextureSlot& slot, std::string path)
{
    slot.path = std::move(path);
    slot.scale[0] = record.scale[0];
    slot.scale[1] = record.scale[1];
    slot.offset[0] = record.offset[0];
    slot.offset[1] = record.offset[1];
    slot.uvSet = record.uvSet;
}

enum class ReadResult { Hit, Miss, Cancelled };

struct FileHandle
{
    HANDLE handle;

    explicit FileHandle(HANDLE h) : handle(h) {}
    ~FileHandle() { Close(); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    bool Valid() const { return handle != INVALID_HANDLE_VALUE; }
    void Close()
    {
        if (Valid()) CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }
};

struct CCtxDeleter { void operator()(ZSTD_CCtx* context) const { ZSTD_freeCCtx(context); } };
struct DCtxDeleter { void operator()(ZSTD_DCtx* context) const { ZSTD_freeDCtx(context); } };
using CCtxPtr = std::unique_ptr<ZSTD_CCtx, CCtxDeleter>;
using DCtxPtr = std::unique_ptr<ZSTD_DCtx, DCtxDeleter>;

std::string Win32Error(const char* operation, const fs::path& path)
{
    const DWORD code = GetLastError();
    return std::string(operation) + " '" + path.generic_string() + "' failed (Win32 error " +
           std::to_string(code) + ")";
}

double MillisecondsSince(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double Megabytes(uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

uint64_t RawBytes(const MeshSourceData& data)
{
    return uint64_t(data.vertices.size()) * sizeof(MeshVertex) +
           uint64_t(data.indices.size()) * sizeof(uint32_t);
}

// The importer that produces an entry: id (1 Assimp, 2 cgltf) and its version; an
// importer switch or upgrade can change import output.
std::array<uint32_t, 4> ImporterVersion(const std::string& sourcePath)
{
    if (Mesh::ResolveImporter(sourcePath) == MeshImporter::Gltf)
        return { 2, 1, 15, 0 };
    return { 1, aiGetVersionMajor(), aiGetVersionMinor(), aiGetVersionPatch() };
}

std::string CacheKey(const std::string& path)
{
    std::string key = fs::path(path).lexically_normal().generic_string();
    for (char& c : key)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return key;
}

uint64_t Fnv1a64(const std::string& text)
{
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char c : text)
    {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

uint32_t BlockCount(uint64_t streamBytes)
{
    return static_cast<uint32_t>((streamBytes + k_blockSize - 1) / k_blockSize);
}

// Maps a global block index (vertex blocks first, then index blocks) to its
// stream and raw byte range.
struct BlockRange
{
    bool index;
    uint64_t offset;
    size_t raw;
};

BlockRange LocateBlock(size_t block, uint64_t vertexBytes, uint32_t vertexBlocks, uint64_t indexBytes)
{
    const bool index = block >= vertexBlocks;
    const uint64_t offset = uint64_t(index ? block - vertexBlocks : block) * k_blockSize;
    const uint64_t streamBytes = index ? indexBytes : vertexBytes;
    return { index, offset, static_cast<size_t>((std::min)(uint64_t(k_blockSize), streamBytes - offset)) };
}

// Runs task(context, i) for every i in [0, count) on up to hardware_concurrency()
// threads; each worker owns one context from makeContext(). Returns false as soon
// as any task fails (remaining indices are skipped).
template <typename MakeContext, typename Task>
bool ParallelFor(size_t count, const MakeContext& makeContext, const Task& task)
{
    std::atomic<size_t> cursor { 0 };
    std::atomic<bool> failed { false };
    const auto work = [&] {
        const auto context = makeContext();
        for (;;)
        {
            const size_t index = cursor.fetch_add(1);
            if (index >= count || failed.load()) return;
            if (!task(context.get(), index)) failed.store(true);
        }
    };

    const size_t hardware = (std::max)(1u, std::thread::hardware_concurrency());
    const size_t workerCount = (std::min)(hardware, count);
    if (workerCount <= 1)
    {
        if (count > 0) work();
        return !failed.load();
    }
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (size_t w = 0; w < workerCount; ++w) workers.emplace_back(work);
    for (auto& worker : workers) worker.join();
    return !failed.load();
}

bool WriteAll(HANDLE file, const uint8_t* data, uint64_t bytes)
{
    while (bytes > 0)
    {
        const DWORD chunk = static_cast<DWORD>((std::min)(bytes, k_ioChunk));
        DWORD written = 0;
        if (!WriteFile(file, data, chunk, &written, nullptr) || written != chunk) return false;
        data += chunk;
        bytes -= chunk;
    }
    return true;
}

bool ReadAll(HANDLE file, uint8_t* data, uint64_t bytes)
{
    while (bytes > 0)
    {
        const DWORD chunk = static_cast<DWORD>((std::min)(bytes, k_ioChunk));
        DWORD read = 0;
        if (!ReadFile(file, data, chunk, &read, nullptr) || read != chunk) return false;
        data += chunk;
        bytes -= chunk;
    }
    return true;
}

template <typename T>
void Append(std::vector<uint8_t>& out, const T* items, size_t count)
{
    const size_t bytes = sizeof(T) * count;
    if (bytes == 0) return;
    const size_t at = out.size();
    out.resize(at + bytes);
    std::memcpy(out.data() + at, items, bytes);
}

bool WriteEntry(const std::string& entry, const std::string& key, const MeshSourceData& data,
                std::string& error, uint64_t& fileBytes)
{
    if (data.sourceFiles.empty())
    {
        error = "no source files recorded";
        return false;
    }

    const fs::path entryPath(entry);
    if (entryPath.has_parent_path())
    {
        std::error_code ec;
        fs::create_directories(entryPath.parent_path(), ec);
        if (ec)
        {
            error = "create directory '" + entryPath.parent_path().generic_string() + "': " + ec.message();
            return false;
        }
    }

    std::string strings;
    const auto addString = [&strings](const std::string& text) {
        const StringRef ref { static_cast<uint32_t>(strings.size()), static_cast<uint32_t>(text.size()) };
        strings += text;
        return ref;
    };

    FileHeader header {};
    std::memcpy(header.magic, k_magic, sizeof(k_magic));
    header.version = k_meshCacheVersion;
    header.blockSize = k_blockSize;
    const std::array<uint32_t, 4> importerVersion = ImporterVersion(key);
    std::memcpy(header.importerVersion, importerVersion.data(), sizeof(header.importerVersion));
    header.dependencyCount = static_cast<uint32_t>(data.sourceFiles.size());
    header.subMeshCount = static_cast<uint32_t>(data.subMeshes.size());
    header.vertexCount = static_cast<uint32_t>(data.vertices.size());
    header.indexCount = static_cast<uint32_t>(data.indices.size());
    const uint64_t vertexBytes = uint64_t(header.vertexCount) * sizeof(MeshVertex);
    const uint64_t indexBytes = uint64_t(header.indexCount) * sizeof(uint32_t);
    header.vertexBlockCount = BlockCount(vertexBytes);
    header.indexBlockCount = BlockCount(indexBytes);
    header.sourceKey = addString(key);
    header.boundsMin[0] = data.bounds.min.x;
    header.boundsMin[1] = data.bounds.min.y;
    header.boundsMin[2] = data.bounds.min.z;
    header.boundsMax[0] = data.bounds.max.x;
    header.boundsMax[1] = data.bounds.max.y;
    header.boundsMax[2] = data.bounds.max.z;

    std::vector<DependencyRecord> dependencies;
    dependencies.reserve(data.sourceFiles.size());
    for (const std::string& file : data.sourceFiles)
    {
        std::error_code ec;
        const fs::directory_entry source(file, ec);
        DependencyRecord record {};
        record.path = addString(file);
        if (!ec) record.size = source.file_size(ec);
        if (!ec) record.writeTime = source.last_write_time(ec).time_since_epoch().count();
        if (ec)
        {
            error = "stat '" + file + "': " + ec.message();
            return false;
        }
        dependencies.push_back(record);
    }

    std::vector<SubMeshRecord> subMeshes;
    subMeshes.reserve(data.subMeshes.size());
    for (const MeshSubMeshSource& sub : data.subMeshes)
    {
        SubMeshRecord record {};
        record.firstVertex = sub.firstVertex;
        record.vertexCount = sub.vertexCount;
        record.firstIndex = sub.firstIndex;
        record.indexCount = sub.indexCount;
        const auto slots = Slots(sub.material);
        for (size_t s = 0; s < k_slotCount; ++s)
            WriteSlot(record.slots[s], *slots[s], addString(slots[s]->path));
        record.alphaCutoff = sub.material.alphaCutoff;
        record.roughnessFactor = sub.material.roughnessFactor;
        record.glossinessFactor = sub.material.glossinessFactor;
        record.occlusionStrength = sub.material.occlusionStrength;
        record.detailAlbedoStrength = sub.material.detailAlbedoStrength;
        record.detailNormalStrength = sub.material.detailNormalStrength;
        record.alphaMode = static_cast<uint8_t>(sub.material.alphaMode);
        record.workflow = static_cast<uint8_t>(sub.material.workflow);
        record.occlusionChannel = sub.material.occlusionChannel;
        record.vertexTint = static_cast<uint8_t>(sub.material.vertexTint);
        record.detailAlbedoBlend = static_cast<uint8_t>(sub.material.detailAlbedoBlend);
        const TerrainMaterial& terrain = sub.material.terrain;
        record.kind = static_cast<uint8_t>(sub.material.kind);
        record.terrainLayerCount = terrain.layerCount;
        record.terrainAlbedoAlphaIsRoughness = terrain.albedoAlphaIsRoughness ? 1 : 0;
        for (uint32_t i = 0; i < k_maxTerrainLayers; ++i)
            record.terrainLayerScale[i] = terrain.layerScale[i];
        record.terrainOverlayStrength = terrain.overlayStrength;
        record.terrainRoughnessScale = terrain.roughnessScale;
        const auto terrainSlots = TerrainSlots(terrain);
        for (size_t s = 0; s < k_terrainSlotCount; ++s)
            WriteSlot(record.terrainSlots[s], *terrainSlots[s], addString(terrainSlots[s]->path));
        record.clearcoat = sub.material.clearcoat;
        record.clearcoatRoughness = sub.material.clearcoatRoughness;
        record.clearcoatMaskFromAlbedoAlpha = sub.material.clearcoatMaskFromAlbedoAlpha ? 1 : 0;
        WriteSlot(record.clearcoatSlots[0], sub.material.clearcoatTex, addString(sub.material.clearcoatTex.path));
        WriteSlot(record.clearcoatSlots[1], sub.material.clearcoatRoughnessTex,
                  addString(sub.material.clearcoatRoughnessTex.path));
        record.boundsMin[0] = sub.bounds.min.x;
        record.boundsMin[1] = sub.bounds.min.y;
        record.boundsMin[2] = sub.bounds.min.z;
        record.boundsMax[0] = sub.bounds.max.x;
        record.boundsMax[1] = sub.bounds.max.y;
        record.boundsMax[2] = sub.bounds.max.z;
        subMeshes.push_back(record);
    }
    header.stringBytes = static_cast<uint32_t>(strings.size());

    // Compress each block into its own fixed-size slot, then compact.
    const size_t blockCount = size_t(header.vertexBlockCount) + header.indexBlockCount;
    const size_t slotBytes = ZSTD_compressBound(k_blockSize);
    std::unique_ptr<uint8_t[]> payload(new uint8_t[blockCount * slotBytes]);
    std::vector<uint32_t> storedSizes(blockCount);
    const auto* vertexBase = reinterpret_cast<const uint8_t*>(data.vertices.data());
    const auto* indexBase = reinterpret_cast<const uint8_t*>(data.indices.data());
    const bool compressed = ParallelFor(blockCount,
        [] { return CCtxPtr(ZSTD_createCCtx()); },
        [&](ZSTD_CCtx* context, size_t block) {
            if (!context) return false;
            const BlockRange range = LocateBlock(block, vertexBytes, header.vertexBlockCount, indexBytes);
            const uint8_t* src = (range.index ? indexBase : vertexBase) + range.offset;
            uint8_t* dst = payload.get() + block * slotBytes;
            const size_t result = ZSTD_compressCCtx(context, dst, slotBytes, src, range.raw, k_compressionLevel);
            if (ZSTD_isError(result) || result >= range.raw)
            {
                std::memcpy(dst, src, range.raw);
                storedSizes[block] = static_cast<uint32_t>(range.raw);
            }
            else
                storedSizes[block] = static_cast<uint32_t>(result);
            return true;
        });
    if (!compressed)
    {
        error = "zstd context allocation failed";
        return false;
    }

    uint64_t payloadBytes = 0;
    for (size_t block = 0; block < blockCount; ++block)
    {
        std::memmove(payload.get() + payloadBytes, payload.get() + block * slotBytes, storedSizes[block]);
        payloadBytes += storedSizes[block];
    }
    header.payloadBytes = payloadBytes;

    std::vector<uint8_t> meta;
    meta.reserve(sizeof(FileHeader) + dependencies.size() * sizeof(DependencyRecord) +
                 subMeshes.size() * sizeof(SubMeshRecord) + storedSizes.size() * sizeof(uint32_t) +
                 strings.size());
    Append(meta, &header, 1);
    Append(meta, dependencies.data(), dependencies.size());
    Append(meta, subMeshes.data(), subMeshes.size());
    Append(meta, storedSizes.data(), storedSizes.size());
    Append(meta, strings.data(), strings.size());

    // Write a uniquely named temp file, then atomically replace the entry so
    // concurrent readers never observe a partial file.
    const fs::path tempPath(entryPath.native() + L"." + std::to_wstring(GetCurrentProcessId()) + L"_" +
                            std::to_wstring(GetCurrentThreadId()) + L".tmp");
    FileHandle file(CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file.Valid())
    {
        error = Win32Error("create", tempPath);
        return false;
    }
    if (!WriteAll(file.handle, meta.data(), meta.size()) ||
        !WriteAll(file.handle, payload.get(), payloadBytes))
    {
        error = Win32Error("write", tempPath);
        file.Close();
        DeleteFileW(tempPath.c_str());
        return false;
    }
    file.Close();
    if (!MoveFileExW(tempPath.c_str(), entryPath.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        error = Win32Error("replace", entryPath);
        DeleteFileW(tempPath.c_str());
        return false;
    }
    fileBytes = meta.size() + payloadBytes;
    return true;
}

// Validates the entry and, when `out` is non-null, decodes it. Miss reasons are
// empty only when the entry does not exist.
ReadResult ReadEntry(const std::string& entry, const std::string& key, bool verifySources,
                     MeshSourceData* out, const Mesh::ImportProgressCallback& progress,
                     std::string& missReason)
{
    missReason.clear();
    const auto miss = [&missReason](std::string reason) {
        missReason = std::move(reason);
        return ReadResult::Miss;
    };

    FileHandle file(CreateFileW(fs::path(entry).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                nullptr));
    if (!file.Valid())
    {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return ReadResult::Miss;
        return miss("open failed (Win32 error " + std::to_string(code) + ")");
    }

    LARGE_INTEGER size {};
    if (!GetFileSizeEx(file.handle, &size)) return miss("size query failed");
    const uint64_t fileBytes = static_cast<uint64_t>(size.QuadPart);
    if (fileBytes < sizeof(FileHeader)) return miss("truncated header");

    FileHeader header;
    if (!ReadAll(file.handle, reinterpret_cast<uint8_t*>(&header), sizeof(header))) return miss("read failed");
    if (std::memcmp(header.magic, k_magic, sizeof(k_magic)) != 0) return miss("bad magic");
    if (header.version != k_meshCacheVersion || header.blockSize != k_blockSize) return miss("format version");
    const std::array<uint32_t, 4> importerVersion = ImporterVersion(key);
    if (std::memcmp(header.importerVersion, importerVersion.data(), sizeof(header.importerVersion)) != 0)
        return miss("importer version");

    const uint64_t vertexBytes = uint64_t(header.vertexCount) * sizeof(MeshVertex);
    const uint64_t indexBytes = uint64_t(header.indexCount) * sizeof(uint32_t);
    if (header.vertexBlockCount != BlockCount(vertexBytes) || header.indexBlockCount != BlockCount(indexBytes))
        return miss("block count mismatch");
    const size_t blockCount = size_t(header.vertexBlockCount) + header.indexBlockCount;
    const uint64_t metaBytes = uint64_t(header.dependencyCount) * sizeof(DependencyRecord) +
                               uint64_t(header.subMeshCount) * sizeof(SubMeshRecord) +
                               uint64_t(blockCount) * sizeof(uint32_t) + header.stringBytes;
    if (header.payloadBytes > fileBytes || sizeof(FileHeader) + metaBytes != fileBytes - header.payloadBytes)
        return miss("size mismatch");

    std::vector<uint8_t> meta(static_cast<size_t>(metaBytes));
    if (!ReadAll(file.handle, meta.data(), metaBytes)) return miss("read failed");
    std::vector<DependencyRecord> dependencies(header.dependencyCount);
    std::vector<SubMeshRecord> subMeshes(header.subMeshCount);
    std::vector<uint32_t> storedSizes(blockCount);
    const uint8_t* cursor = meta.data();
    const auto take = [&cursor](auto& items) {
        const size_t bytes = items.size() * sizeof(items[0]);
        if (bytes > 0) std::memcpy(items.data(), cursor, bytes);
        cursor += bytes;
    };
    take(dependencies);
    take(subMeshes);
    take(storedSizes);
    const char* strings = reinterpret_cast<const char*>(cursor);
    const auto validRef = [&header](const StringRef& ref) {
        return ref.offset <= header.stringBytes && ref.length <= header.stringBytes - ref.offset;
    };
    const auto text = [strings](const StringRef& ref) { return std::string(strings + ref.offset, ref.length); };

    uint64_t storedTotal = 0;
    for (size_t block = 0; block < blockCount; ++block)
    {
        const size_t raw = LocateBlock(block, vertexBytes, header.vertexBlockCount, indexBytes).raw;
        if (storedSizes[block] == 0 || storedSizes[block] > raw) return miss("corrupt block table");
        storedTotal += storedSizes[block];
    }
    if (storedTotal != header.payloadBytes) return miss("corrupt block table");
    if (!validRef(header.sourceKey)) return miss("corrupt string table");
    for (const DependencyRecord& dependency : dependencies)
        if (!validRef(dependency.path)) return miss("corrupt string table");
    for (const SubMeshRecord& sub : subMeshes)
    {
        for (const SlotRecord& slot : sub.slots)
        {
            if (!validRef(slot.path)) return miss("corrupt string table");
            if (slot.uvSet > 2) return miss("corrupt submesh table");
        }
        for (const SlotRecord& slot : sub.terrainSlots)
        {
            if (!validRef(slot.path)) return miss("corrupt string table");
            if (slot.uvSet > 2) return miss("corrupt submesh table");
        }
        for (const SlotRecord& slot : sub.clearcoatSlots)
        {
            if (!validRef(slot.path)) return miss("corrupt string table");
            if (slot.uvSet > 2) return miss("corrupt submesh table");
        }
        if (uint64_t(sub.firstVertex) + sub.vertexCount > header.vertexCount ||
            uint64_t(sub.firstIndex) + sub.indexCount > header.indexCount ||
            sub.alphaMode > 2 || sub.workflow > 2 || sub.occlusionChannel > 1 ||
            sub.vertexTint > 2 || sub.detailAlbedoBlend > 2 || sub.kind > 1 ||
            sub.terrainLayerCount > k_maxTerrainLayers || sub.terrainAlbedoAlphaIsRoughness > 1 ||
            sub.clearcoatMaskFromAlbedoAlpha > 1)
            return miss("corrupt submesh table");
    }
    if (text(header.sourceKey) != key) return miss("key collision");

    if (verifySources)
    {
        for (const DependencyRecord& dependency : dependencies)
        {
            const std::string path = text(dependency.path);
            std::error_code ec;
            const fs::directory_entry source(path, ec);
            bool fresh = !ec && source.is_regular_file(ec) && !ec;
            if (fresh)
            {
                const auto bytes = source.file_size(ec);
                fresh = !ec && bytes == dependency.size;
            }
            if (fresh)
            {
                const auto writeTime = source.last_write_time(ec);
                fresh = !ec && writeTime.time_since_epoch().count() == dependency.writeTime;
            }
            if (!fresh) return miss("source changed: " + path);
        }
    }

    if (!out) return ReadResult::Hit;

    if (progress && !progress(0.0f)) return ReadResult::Cancelled;
    std::unique_ptr<uint8_t[]> payload(new uint8_t[static_cast<size_t>(header.payloadBytes)]);
    for (uint64_t done = 0; done < header.payloadBytes;)
    {
        const uint64_t chunk = (std::min)(k_ioChunk, header.payloadBytes - done);
        if (!ReadAll(file.handle, payload.get() + done, chunk)) return miss("read failed");
        done += chunk;
        if (progress && !progress(0.5f * static_cast<float>(done) / static_cast<float>(header.payloadBytes)))
            return ReadResult::Cancelled;
    }
    file.Close();

    *out = {};
    out->subMeshes.reserve(subMeshes.size());
    for (const SubMeshRecord& record : subMeshes)
    {
        MeshSubMeshSource sub;
        sub.firstVertex = record.firstVertex;
        sub.vertexCount = record.vertexCount;
        sub.firstIndex = record.firstIndex;
        sub.indexCount = record.indexCount;
        const auto slots = Slots(sub.material);
        for (size_t s = 0; s < k_slotCount; ++s)
            ReadSlot(record.slots[s], *slots[s], text(record.slots[s].path));
        sub.material.occlusionStrength = record.occlusionStrength;
        sub.material.occlusionChannel = record.occlusionChannel;
        sub.material.detailAlbedoBlend = static_cast<DetailBlend>(record.detailAlbedoBlend);
        sub.material.detailAlbedoStrength = record.detailAlbedoStrength;
        sub.material.detailNormalStrength = record.detailNormalStrength;
        sub.material.alphaMode = static_cast<AlphaMode>(record.alphaMode);
        sub.material.alphaCutoff = record.alphaCutoff;
        sub.material.workflow = static_cast<MaterialWorkflow>(record.workflow);
        sub.material.roughnessFactor = record.roughnessFactor;
        sub.material.glossinessFactor = record.glossinessFactor;
        sub.material.vertexTint = static_cast<VertexTint>(record.vertexTint);
        sub.material.kind = static_cast<MaterialKind>(record.kind);
        TerrainMaterial& terrain = sub.material.terrain;
        terrain.layerCount = record.terrainLayerCount;
        terrain.albedoAlphaIsRoughness = record.terrainAlbedoAlphaIsRoughness != 0;
        for (uint32_t i = 0; i < k_maxTerrainLayers; ++i)
            terrain.layerScale[i] = record.terrainLayerScale[i];
        terrain.overlayStrength = record.terrainOverlayStrength;
        terrain.roughnessScale = record.terrainRoughnessScale;
        const auto terrainSlots = TerrainSlots(terrain);
        for (size_t s = 0; s < k_terrainSlotCount; ++s)
            ReadSlot(record.terrainSlots[s], *terrainSlots[s], text(record.terrainSlots[s].path));
        sub.material.clearcoat = record.clearcoat;
        sub.material.clearcoatRoughness = record.clearcoatRoughness;
        sub.material.clearcoatMaskFromAlbedoAlpha = record.clearcoatMaskFromAlbedoAlpha != 0;
        ReadSlot(record.clearcoatSlots[0], sub.material.clearcoatTex, text(record.clearcoatSlots[0].path));
        ReadSlot(record.clearcoatSlots[1], sub.material.clearcoatRoughnessTex,
                 text(record.clearcoatSlots[1].path));
        sub.bounds.min = { record.boundsMin[0], record.boundsMin[1], record.boundsMin[2] };
        sub.bounds.max = { record.boundsMax[0], record.boundsMax[1], record.boundsMax[2] };
        out->subMeshes.push_back(std::move(sub));
    }
    out->sourceFiles.reserve(dependencies.size());
    for (const DependencyRecord& dependency : dependencies)
        out->sourceFiles.push_back(text(dependency.path));
    out->bounds.min = { header.boundsMin[0], header.boundsMin[1], header.boundsMin[2] };
    out->bounds.max = { header.boundsMax[0], header.boundsMax[1], header.boundsMax[2] };
    out->vertices.resize(header.vertexCount);
    out->indices.resize(header.indexCount);

    std::vector<uint64_t> storedOffsets(blockCount);
    for (size_t block = 0, offset = 0; block < blockCount; offset += storedSizes[block], ++block)
        storedOffsets[block] = offset;
    auto* vertexBase = reinterpret_cast<uint8_t*>(out->vertices.data());
    auto* indexBase = reinterpret_cast<uint8_t*>(out->indices.data());
    const bool decoded = ParallelFor(blockCount,
        [] { return DCtxPtr(ZSTD_createDCtx()); },
        [&](ZSTD_DCtx* context, size_t block) {
            const BlockRange range = LocateBlock(block, vertexBytes, header.vertexBlockCount, indexBytes);
            uint8_t* dst = (range.index ? indexBase : vertexBase) + range.offset;
            const uint8_t* src = payload.get() + storedOffsets[block];
            if (storedSizes[block] == range.raw)
            {
                std::memcpy(dst, src, range.raw);
                return true;
            }
            return context && ZSTD_decompressDCtx(context, dst, range.raw, src, storedSizes[block]) == range.raw;
        });
    if (!decoded)
    {
        *out = {};
        return miss("corrupt block");
    }

    if (progress && !progress(1.0f)) return ReadResult::Cancelled;
    return ReadResult::Hit;
}

} // namespace

MeshCache::MeshCache(std::string directory) : m_directory(std::move(directory)) {}

std::string MeshCache::EntryPath(const std::string& sourcePath) const
{
    char hash[17];
    std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(Fnv1a64(CacheKey(sourcePath))));
    const std::string name = fs::path(sourcePath).stem().string() + "-" + hash + ".fxmesh";
    return (fs::path(m_directory) / name).generic_string();
}

bool MeshCache::Load(const std::string& sourcePath, MeshSourceData& output,
                     const Mesh::ImportProgressCallback& progress,
                     std::string* error, bool* cacheHit) const
{
    output = {};
    if (cacheHit) *cacheHit = false;
    std::error_code ec;
    const bool sourceExists = fs::is_regular_file(sourcePath, ec);
    const std::string entry = EntryPath(sourcePath);
    const std::string key = CacheKey(sourcePath);

    const auto readStart = Clock::now();
    std::string reason;
    const ReadResult read = ReadEntry(entry, key, sourceExists, &output, progress, reason);
    if (read == ReadResult::Cancelled)
        return false;
    if (read == ReadResult::Hit)
    {
        output.directory = Mesh::SourceDirectory(sourcePath);
        if (!sourceExists)
            SE_LOG_INFO("MeshCache: source '%s' missing; using cooked entry", sourcePath.c_str());
        SE_LOG_INFO("MeshCache: hit '%s' (%zu sub-meshes, %.1f MB, %.1f ms)", sourcePath.c_str(),
                    output.subMeshes.size(), Megabytes(RawBytes(output)), MillisecondsSince(readStart));
        if (cacheHit) *cacheHit = true;
        return true;
    }
    if (reason.empty())
        SE_LOG_INFO("MeshCache: miss '%s' (no entry); importing", sourcePath.c_str());
    else if (reason == "format version" || reason == "importer version" ||
             reason.rfind("source changed", 0) == 0)
        SE_LOG_INFO("MeshCache: stale entry for '%s' (%s); importing", sourcePath.c_str(), reason.c_str());
    else
        SE_LOG_WARN("MeshCache: stale entry for '%s' (%s); importing", sourcePath.c_str(), reason.c_str());

    output = {};
    const auto importStart = Clock::now();
    if (!Mesh::Import(sourcePath.c_str(), output, progress, error))
        return false;
    const double importMs = MillisecondsSince(importStart);

    const auto storeStart = Clock::now();
    std::string storeError;
    uint64_t storedBytes = 0;
    if (!WriteEntry(entry, key, output, storeError, storedBytes))
        SE_LOG_WARN("MeshCache: could not store '%s': %s", sourcePath.c_str(), storeError.c_str());
    else
        SE_LOG_INFO("MeshCache: cooked '%s' -> '%s' (import %.1f ms, store %.1f ms, %.1f MB -> %.1f MB)",
                    sourcePath.c_str(), entry.c_str(), importMs, MillisecondsSince(storeStart),
                    Megabytes(RawBytes(output)), Megabytes(storedBytes));
    return true;
}

MeshCookResult MeshCache::Cook(const std::string& sourcePath, bool force, std::string* error) const
{
    std::error_code ec;
    if (!fs::is_regular_file(sourcePath, ec))
    {
        if (error) *error = "source file not found: " + sourcePath;
        return MeshCookResult::Failed;
    }

    const std::string entry = EntryPath(sourcePath);
    const std::string key = CacheKey(sourcePath);
    std::string reason;
    if (!force && ReadEntry(entry, key, true, nullptr, {}, reason) == ReadResult::Hit)
        return MeshCookResult::UpToDate;

    MeshSourceData data;
    if (!Mesh::Import(sourcePath.c_str(), data, {}, error))
        return MeshCookResult::Failed;
    std::string storeError;
    uint64_t storedBytes = 0;
    if (!WriteEntry(entry, key, data, storeError, storedBytes))
    {
        if (error) *error = storeError;
        return MeshCookResult::Failed;
    }
    return MeshCookResult::Cooked;
}

} // namespace SE
