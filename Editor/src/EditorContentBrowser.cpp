#include "EditorContentBrowser.h"

#include <algorithm>
#include <imgui.h>

#include "EditorIcons.h"
#include "EditorTheme.h"
#include "Engine/Scene/SceneRuntime.h"

namespace fs = std::filesystem;

namespace Editor {

namespace {

std::string LowerExt(const std::string& path)
{
    const size_t dot = path.rfind('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return ext;
}

bool IsMeshExt(const std::string& ext)
{
    return ext == ".fbx" || ext == ".gltf" || ext == ".glb" || ext == ".obj";
}

bool IsTextureExt(const std::string& ext)
{
    return ext == ".dds" || ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".tga" || ext == ".hdr" || ext == ".bmp";
}

struct KindStyle
{
    const char* glyph;
    ImU32       color;
};

KindStyle StyleFor(int kind)
{
    switch (kind)
    {
    case 0:  return { ICON_MD_FOLDER,            IM_COL32(220, 185, 110, 255) }; // Folder
    case 1:  return { ICON_MD_CATEGORY,          IM_COL32(140, 200, 160, 255) }; // Mesh
    case 2:  return { ICON_MD_IMAGE,             IM_COL32(150, 190, 235, 255) }; // Texture
    case 3:  return { ICON_MD_PUBLIC,            IM_COL32(120, 190, 255, 255) }; // Scene
    case 4:  return { ICON_MD_GRAIN,             IM_COL32(210, 150, 255, 255) }; // Preset
    default: return { ICON_MD_INSERT_DRIVE_FILE, IM_COL32(160, 160, 165, 255) };
    }
}

bool HasSubdirectory(const fs::path& dir)
{
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(dir, error))
        if (entry.is_directory(error)) return true;
    return false;
}

} // namespace

void ContentBrowser::NavigateTo(const std::string& dir)
{
    m_pendingNavigate = dir;
}

void ContentBrowser::RefreshEntries()
{
    m_entries.clear();
    std::error_code error;
    for (const auto& item : fs::directory_iterator(m_currentDir, error))
    {
        Entry entry;
        entry.name = item.path().filename().string();
        entry.path = item.path().generic_string();
        if (item.is_directory(error))
            entry.kind = FileKind::Folder;
        else
        {
            const std::string ext = LowerExt(entry.name);
            if (IsMeshExt(ext))            entry.kind = FileKind::Mesh;
            else if (IsTextureExt(ext))    entry.kind = FileKind::Texture;
            else if (ext == ".json")
                entry.kind = entry.path.find("/Scenes/") != std::string::npos
                    ? FileKind::Scene
                    : entry.path.find("/Presets/") != std::string::npos
                        ? FileKind::Preset : FileKind::Other;
            else                           entry.kind = FileKind::Other;
        }
        m_entries.push_back(std::move(entry));
    }
    std::stable_sort(m_entries.begin(), m_entries.end(),
        [](const Entry& a, const Entry& b) {
            const bool folderA = a.kind == FileKind::Folder;
            const bool folderB = b.kind == FileKind::Folder;
            if (folderA != folderB) return folderA;
            return a.name < b.name;
        });
    m_entriesDirty = false;
}

void ContentBrowser::DrawFolderTree(const fs::path& dir)
{
    std::error_code error;
    for (const auto& item : fs::directory_iterator(dir, error))
    {
        if (!item.is_directory(error))
            continue;
        const std::string path = item.path().generic_string();
        const bool hasChildren = HasSubdirectory(item.path());
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_SpanAvailWidth;
        if (!hasChildren)   flags |= ImGuiTreeNodeFlags_Leaf;
        if (m_currentDir == path) flags |= ImGuiTreeNodeFlags_Selected;
        const std::string label =
            ICON_MD_FOLDER " " + item.path().filename().string();
        const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
            NavigateTo(path);
        if (open)
        {
            if (hasChildren)
                DrawFolderTree(item.path());
            ImGui::TreePop();
        }
    }
}

void ContentBrowser::DrawBreadcrumb()
{
    // "Assets / Bistro / Textures" with clickable segments.
    std::string accumulated;
    size_t begin = 0;
    bool first = true;
    while (begin <= m_currentDir.size())
    {
        const size_t slash = m_currentDir.find('/', begin);
        const std::string segment =
            m_currentDir.substr(begin, slash == std::string::npos
                                           ? std::string::npos : slash - begin);
        accumulated = accumulated.empty() ? segment : accumulated + "/" + segment;
        if (!first)
        {
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::TextDisabled("/");
            ImGui::SameLine(0.0f, 4.0f);
        }
        first = false;
        ImGui::PushID((int)begin);
        if (ImGui::SmallButton(segment.c_str()) && accumulated != m_currentDir)
            NavigateTo(accumulated);
        ImGui::PopID();
        if (slash == std::string::npos)
            break;
        begin = slash + 1;
    }
    ImGui::SameLine(0.0f, 12.0f);
    if (ImGui::SmallButton(ICON_MD_REFRESH))
        m_entriesDirty = true;
}

void ContentBrowser::DrawTileContextMenu(const Entry& entry, SE::SceneRuntime& scene,
                                         SE::AssetManager& assets, bool canModify)
{
    // Only kinds with actions get a context menu; otherwise right-click would
    // open an empty popup.
    if (entry.kind != FileKind::Mesh && entry.kind != FileKind::Scene &&
        entry.kind != FileKind::Folder)
        return;
    if (!ImGui::BeginPopupContextItem("##ctx"))
        return;
    m_selectedPath = entry.path;
    if (entry.kind == FileKind::Mesh &&
        ImGui::MenuItem("Add to Scene", nullptr, false, canModify))
        StartMeshAdd(entry.path, assets);
    if (entry.kind == FileKind::Scene &&
        ImGui::MenuItem("Open Scene", nullptr, false, !scene.IsLoading()))
        scene.RequestSceneLoad(entry.path);
    if (entry.kind == FileKind::Folder && ImGui::MenuItem("Open"))
        NavigateTo(entry.path);
    ImGui::EndPopup();
}

void ContentBrowser::DrawTileInput(const Entry& entry, float tileSize,
                                   float labelHeight, SE::SceneRuntime& scene,
                                   SE::AssetManager& assets, bool canModify)
{
    if (ImGui::Selectable("##tile", m_selectedPath == entry.path, 0,
                          { tileSize, tileSize + labelHeight }))
        m_selectedPath = entry.path;
    const bool doubleClicked = ImGui::IsItemHovered() &&
                               ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", entry.name.c_str());

    DrawTileContextMenu(entry, scene, assets, canModify);
    if (!doubleClicked)
        return;
    switch (entry.kind)
    {
    case FileKind::Folder: NavigateTo(entry.path); break;
    case FileKind::Mesh:   if (canModify) StartMeshAdd(entry.path, assets); break;
    case FileKind::Scene:  if (!scene.IsLoading()) scene.RequestSceneLoad(entry.path); break;
    default: break;
    }
}

void ContentBrowser::DrawTile(const Entry& entry, float tileSize, float labelHeight,
                              SE::SceneRuntime& scene, SE::AssetManager& assets,
                              bool canModify)
{
    ImGui::PushID(entry.path.c_str());
    ImGui::BeginGroup();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    DrawTileInput(entry, tileSize, labelHeight, scene, assets, canModify);

    // Thumbnail area backdrop + image or icon.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 thumbMax = { pos.x + tileSize, pos.y + tileSize };
    dl->AddRectFilled(pos, thumbMax, IM_COL32(38, 38, 42, 255), 4.0f);
    ID3D11ShaderResourceView* srv = nullptr;
    if (entry.kind == FileKind::Texture &&
        ImGui::IsRectVisible(pos, thumbMax))
        srv = m_thumbs.Get(entry.path);
    if (srv)
    {
        const float inset = 4.0f;
        dl->AddImageRounded((ImTextureID)srv,
                            { pos.x + inset, pos.y + inset },
                            { thumbMax.x - inset, thumbMax.y - inset },
                            { 0, 0 }, { 1, 1 }, IM_COL32_WHITE, 3.0f);
    }
    else
    {
        const KindStyle style = StyleFor((int)entry.kind);
        ImFont* font = LargeIconFont();
        const float glyphSize = 42.0f;
        const ImVec2 sz = font
            ? font->CalcTextSizeA(glyphSize, FLT_MAX, 0.0f, style.glyph)
            : ImGui::CalcTextSize(style.glyph);
        const ImVec2 center = { pos.x + (tileSize - sz.x) * 0.5f,
                                pos.y + (tileSize - sz.y) * 0.5f };
        if (font)
            dl->AddText(font, glyphSize, center, style.color, style.glyph);
        else
            dl->AddText(center, style.color, style.glyph);
    }

    // Importing marker on the mesh currently being added.
    if (entry.kind == FileKind::Mesh && entry.path == m_meshAdd.GetPath())
    {
        const SE::MeshAddState state = m_meshAdd.GetState();
        if (state == SE::MeshAddState::Preparing || state == SE::MeshAddState::Uploading)
            dl->AddRect(pos, thumbMax, IM_COL32(120, 190, 255, 255), 4.0f, 0, 2.0f);
    }

    // File name, clipped to the tile.
    ImGui::PushClipRect({ pos.x, pos.y + tileSize },
                        { pos.x + tileSize, pos.y + tileSize + labelHeight }, true);
    dl->AddText({ pos.x + 2.0f, pos.y + tileSize + 3.0f },
                ImGui::GetColorU32(ImGuiCol_Text), entry.name.c_str());
    ImGui::PopClipRect();

    ImGui::EndGroup();
    ImGui::PopID();
}

void ContentBrowser::DrawGrid(SE::SceneRuntime& scene, SE::AssetManager& assets,
                              bool canModify)
{
    const float tileSize = 84.0f;
    const float pad = 10.0f;
    const float labelHeight = ImGui::GetTextLineHeight() + 8.0f;
    // Wrap against the visible clip edge rather than a precomputed column
    // count so tiles never straddle the right border.
    const float clipRight = ImGui::GetWindowDrawList()->GetClipRectMax().x;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f); // top margin below breadcrumb
    bool first = true;
    for (const auto& entry : m_entries)
    {
        if (!first && ImGui::GetItemRectMax().x + pad + tileSize <= clipRight)
            ImGui::SameLine(0.0f, pad);
        DrawTile(entry, tileSize, labelHeight, scene, assets, canModify);
        first = false;
    }
}

void ContentBrowser::StartMeshAdd(const std::string& path, SE::AssetManager& assets)
{
    const SE::MeshAddState state = m_meshAdd.GetState();
    if (state == SE::MeshAddState::Preparing || state == SE::MeshAddState::Uploading)
        return; // one import at a time
    m_statusError.clear();
    m_meshAdd.Start(path, assets.CaptureSnapshot());
}

// Pumps async import work; returns true when a finished mesh was added.
bool ContentBrowser::PumpMeshAdd(SE::SceneRuntime& scene, Selection& sel,
                                 SE::AssetManager& assets, ID3D11Device* device,
                                 const DirectX::XMFLOAT3& spawnPos)
{
    m_meshAdd.PumpUploads(device, assets);
    bool added = false;
    if (auto result = m_meshAdd.TakeResult())
    {
        const int index = scene.AddLoadedMesh(result->path, std::move(result->mesh),
                                              std::move(result->materials), spawnPos);
        if (index >= 0)
        {
            sel.Set(Selection::Kind::Mesh, index);
            added = true;
        }
    }
    if (m_meshAdd.GetState() == SE::MeshAddState::Failed && m_statusError.empty())
        m_statusError = m_meshAdd.GetError();
    return added;
}

void ContentBrowser::DrawStatusLine()
{
    const SE::MeshAddState state = m_meshAdd.GetState();
    if (state == SE::MeshAddState::Preparing || state == SE::MeshAddState::Uploading)
    {
        const int dots = 1 + (int)(ImGui::GetTime() * 2.0) % 3;
        ImGui::Text("Importing %s%.*s", m_meshAdd.GetPath().c_str(), dots, "...");
    }
    else if (!m_statusError.empty())
        ImGui::TextColored({ 1.0f, 0.35f, 0.35f, 1.0f }, "%s", m_statusError.c_str());
}

bool ContentBrowser::Draw(SE::SceneRuntime& scene, Selection& sel,
                          SE::AssetManager& assets, ID3D11Device* device,
                          ID3D11DeviceContext* ctx,
                          const DirectX::XMFLOAT3& spawnPos)
{
    m_thumbs.Update(device, ctx);
    const bool added = PumpMeshAdd(scene, sel, assets, device, spawnPos);

    ImGui::Begin("Content Browser");
    if (!m_pendingNavigate.empty())
    {
        m_currentDir = m_pendingNavigate;
        m_pendingNavigate.clear();
        m_entriesDirty = true;
        m_selectedPath.clear();
    }
    if (m_entriesDirty)
        RefreshEntries();

    const bool canModify = scene.HasScene() && !scene.IsLoading();
    const float statusHeight = ImGui::GetTextLineHeightWithSpacing() + 4.0f;

    if (ImGui::BeginChild("##folders", { 190.0f, -statusHeight }, ImGuiChildFlags_ResizeX))
    {
        ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                       ImGuiTreeNodeFlags_SpanAvailWidth |
                                       ImGuiTreeNodeFlags_DefaultOpen;
        if (m_currentDir == "Assets")
            rootFlags |= ImGuiTreeNodeFlags_Selected;
        const bool open = ImGui::TreeNodeEx(ICON_MD_FOLDER " Assets", rootFlags);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
            NavigateTo("Assets");
        if (open)
        {
            DrawFolderTree("Assets");
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginGroup();
    DrawBreadcrumb();
    if (ImGui::BeginChild("##grid", { 0.0f, -statusHeight }))
        DrawGrid(scene, assets, canModify);
    ImGui::EndChild();
    ImGui::EndGroup();

    DrawStatusLine();
    ImGui::End();
    return added;
}

} // namespace Editor
