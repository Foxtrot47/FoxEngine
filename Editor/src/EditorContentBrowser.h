#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include <imgui.h>
#include <DirectXMath.h>

#include "EditorSelection.h"
#include "EditorThumbnails.h"
#include "Engine/Scene/MeshAddOperation.h"

namespace SE {
class AssetManager;
class SceneRuntime;
}

namespace Editor {

class UndoStack;

// Asset browser panel: folder tree of Assets/ on the left, thumbnail grid on
// the right. Meshes are added to the scene via double-click or context menu
// (async import, undoable once added). Scene files are not opened directly:
// the request is handed to the app so it can guard unsaved changes.
class ContentBrowser
{
public:
    // Draws the panel and pumps async thumbnail/import work.
    void Draw(SE::SceneRuntime& scene, Selection& sel, UndoStack& undo,
              SE::AssetManager& assets, ID3D11Device* device, ID3D11DeviceContext* ctx,
              const DirectX::XMFLOAT3& spawnPos);

    // Scene path the user asked to open since the last call, or empty.
    std::string TakeSceneOpenRequest() { return std::exchange(m_sceneOpenRequest, {}); }
    // Rescans the current folder next frame (e.g. after Save As).
    void RequestRefresh() { m_entriesDirty = true; }

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
    void DrawFilters();
    void DrawGrid(SE::SceneRuntime& scene, SE::AssetManager& assets, bool canModify);
    void DrawTile(const Entry& entry, float tileSize, float labelHeight,
                  SE::SceneRuntime& scene, SE::AssetManager& assets, bool canModify);
    void DrawTileInput(const Entry& entry, float tileSize, float labelHeight,
                       SE::SceneRuntime& scene, SE::AssetManager& assets,
                       bool canModify);
    void DrawTileContextMenu(const Entry& entry, SE::SceneRuntime& scene,
                             SE::AssetManager& assets, bool canModify);
    void PumpMeshAdd(SE::SceneRuntime& scene, Selection& sel, UndoStack& undo,
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
    std::string m_sceneOpenRequest;
    std::string m_statusError;
    ImGuiTextFilter m_filter;
    int m_kindFilter = 0;
    float m_tileSize = 84.0f;
    ThumbnailCache m_thumbs;
    SE::MeshAddOperation m_meshAdd;
};

} // namespace Editor
