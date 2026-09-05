#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <DirectXMath.h>

#include "EditorSelection.h"
#include "EditorThumbnails.h"
#include "Engine/Scene/MeshAddOperation.h"

namespace SE {
class AssetManager;
class SceneRuntime;
}

namespace Editor {

// Asset browser panel: folder tree of Assets/ on the left, thumbnail grid on
// the right. Meshes are added to the scene via double-click or context menu
// (async import), scene files open on double-click.
class ContentBrowser
{
public:
    // Draws the panel and pumps async thumbnail/import work. Returns true
    // when a mesh finished importing and was added to the scene.
    bool Draw(SE::SceneRuntime& scene, Selection& sel, SE::AssetManager& assets,
              ID3D11Device* device, ID3D11DeviceContext* ctx,
              const DirectX::XMFLOAT3& spawnPos);

private:
    enum class FileKind { Folder, Mesh, Texture, Scene, Preset, Other };
    struct Entry
    {
        std::string name;
        std::string path; // generic (forward-slash) path
        FileKind kind = FileKind::Other;
    };

    void RefreshEntries();
    void DrawFolderTree(const std::filesystem::path& dir);
    void DrawBreadcrumb();
    void DrawGrid(SE::SceneRuntime& scene, SE::AssetManager& assets, bool canModify);
    void DrawTile(const Entry& entry, float tileSize, float labelHeight,
                  SE::SceneRuntime& scene, SE::AssetManager& assets, bool canModify);
    void DrawTileInput(const Entry& entry, float tileSize, float labelHeight,
                       SE::SceneRuntime& scene, SE::AssetManager& assets,
                       bool canModify);
    void DrawTileContextMenu(const Entry& entry, SE::SceneRuntime& scene,
                             SE::AssetManager& assets, bool canModify);
    bool PumpMeshAdd(SE::SceneRuntime& scene, Selection& sel,
                     SE::AssetManager& assets, ID3D11Device* device,
                     const DirectX::XMFLOAT3& spawnPos);
    void DrawStatusLine();
    void NavigateTo(const std::string& dir);
    void StartMeshAdd(const std::string& path, SE::AssetManager& assets);

    std::string m_currentDir = "Assets";
    std::vector<Entry> m_entries;
    bool m_entriesDirty = true;
    std::string m_selectedPath;
    std::string m_pendingNavigate;
    std::string m_statusError;
    ThumbnailCache m_thumbs;
    SE::MeshAddOperation m_meshAdd;
};

} // namespace Editor
