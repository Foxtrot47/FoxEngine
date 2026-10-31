#include <windows.h>
#include <DirectXMath.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include "Engine/Core/Engine.h"
#include "Engine/Core/Logger.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Scene/SceneLoader.h"
#include "Engine/Scene/SceneRuntime.h"
#include "Engine/Scene/SceneSaver.h"
#include "EditorTheme.h"
#include "EditorCamera.h"
#include "EditorViewportToolbar.h"
#include "EditorPanels.h"
#include "EditorSelection.h"
#include "EditorUndo.h"
#include "EditorGizmo.h"
#include "EditorEntityOps.h"
#include "EditorOutliner.h"
#include "EditorDetails.h"
#include "EditorContentBrowser.h"
#include <ImGuizmo.h>

class EditorApp : public SE::Engine
{
public:
    ~EditorApp()
    {
        m_scene.Shutdown();
    }

    void Shutdown()
    {
        m_editorCam.CancelDrag();
        // The load worker may still be creating D3D resources. Join it before
        // the renderer and device are released by the base engine shutdown.
        m_scene.Shutdown();
        SE::Engine::Shutdown();
    }

    bool Setup(const std::string& scenePath = "")
    {
        // Keep editor dock layout separate from TestGame's imgui.ini.
        ImGui::GetIO().IniFilename = "editor.ini";
        SetTitleBase(L"FoxEditor");
        Editor::ApplyTheme();
        // Window close goes through the unsaved-changes prompt.
        GetWindow().SetCloseInterception(true);

        if (!m_scene.Init(GetRenderer(), GetShaders(), GetAssets(), GetInput(),
                          GetWindow().GetHandle(),
                          GetWindow().GetWidth(), GetWindow().GetHeight()))
            return false;

        const auto sceneFiles = SE::SceneLoader::ScanSceneDirectory("Assets/Scenes");
        if (sceneFiles.empty() && scenePath.empty())
        {
            SE_LOG_ERROR("No scene files found in Assets/Scenes/");
            return false;
        }

        std::string targetScene = scenePath.empty() ? sceneFiles[0] : scenePath;
        m_scene.RequestSceneLoad(targetScene);
        SE_LOG_INFO("Editor initial scene queued — %s", targetScene.c_str());
        return true;
    }

protected:
    void OnUpdate() override
    {
        if (m_scene.TickSceneLoad([this]() { return GetWindow().PumpMessages(); }))
            OnSceneReady();

        // Match the offscreen viewport RT to last frame's panel size before
        // this frame's ImGui::Image captures its SRV.
        UpdateViewportRT();

        const bool sceneActive = m_scene.HasScene() && !m_scene.IsLoading();
        if (sceneActive)
            UpdateDirtyFlag();
        // Resolve this frame's image, toolbar and gizmo ownership before
        // camera input. Using last frame's window hover let UI clicks navigate.
        DrawUI(sceneActive);
        if (m_scene.HasScene() && !m_scene.IsLoading() && m_viewportRT.GetRTV())
            UpdateSceneViewport();

        const std::string openRequest = m_contentBrowser.TakeSceneOpenRequest();
        if (!openRequest.empty())
            RequestSceneAction(SceneAction::Open, openRequest);
        if (GetWindow().ConsumeCloseRequest())
            RequestSceneAction(SceneAction::Exit);
    }

    void OnPostProcess() override
    {
        if (m_scene.HasScene() && !m_scene.IsLoading() && m_viewportRT.GetRTV())
        {
            m_scene.PostProcess(m_viewportRT.GetWidth(), m_viewportRT.GetHeight(),
                                m_viewOptions.realtime ? GetClock().GetDeltaTime() : 0.0f,
                                [this]() { BindViewportRT(); });
        }
        // Bind + resolve the swap chain so ImGui draws over a known background.
        GetRenderer().ResolveToBackBuffer();
    }

private:
    void UpdateSceneViewport()
    {
        const float dt = GetClock().GetDeltaTime();
        const bool blocked = ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive() ||
            ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
        if (blocked && !ImGuizmo::IsUsing()) m_editorCam.CancelDrag();
        m_editorCam.Update(dt, GetInput(), *m_scene.camera, GetWindow().GetHandle(),
            m_viewportHovered && !blocked, m_viewportSize.y);
        CheckViewportCameraCut();
        const float sceneDt = m_viewOptions.realtime ? dt : 0.0f;
        m_previewTime += sceneDt;
        m_scene.drawDebugHelpers = !m_viewOptions.gameView && m_viewOptions.showLights;
        m_scene.Update(sceneDt, m_previewTime);
        m_scene.Render(static_cast<float>(m_viewportRT.GetWidth()) /
                       static_cast<float>(m_viewportRT.GetHeight()));
    }

    void CheckViewportCameraCut()
    {
        const auto view = m_editorCam.GetViewMode();
        const auto shading = m_scene.pipeline.viewShading;
        if (view == m_previousView && shading == m_previousShading) return;
        m_scene.ResetTemporalHistory();
        m_previousView = view;
        m_previousShading = shading;
    }

    void OnSceneReady()
    {
        m_previewTime = 0.0f;
        m_editorCam.SyncFromCamera(*m_scene.camera);
        m_selection.Clear();
        m_undo.Clear(); // undo entries point into the previous scene
        TakeSavedSnapshot();
    }

    // --- Scene save, open/exit guard + dirty tracking ---

    enum class SceneAction { None, Open, Exit };

    bool SaveScene()
    {
        return SaveSceneAs(m_scene.currentScenePath);
    }

    bool SaveSceneAs(const std::string& path)
    {
        if (!m_scene.HasScene() || m_scene.IsLoading())
            return false;
        m_scene.SyncDescriptorFromRuntime();
        if (!SE::SceneSaver::SaveToFile(path, m_scene.currentDesc))
            return false;
        if (path != m_scene.currentScenePath)
        {
            m_scene.currentScenePath       = path;
            m_scene.lastRequestedScenePath = path;
            m_contentBrowser.RequestRefresh();
        }
        TakeSavedSnapshot();
        return true;
    }

    // Unthrottled dirty check for decisions that discard the scene.
    bool IsSceneDirtyNow()
    {
        if (!m_scene.HasScene() || m_scene.IsLoading())
            return false;
        m_scene.SyncDescriptorFromRuntime();
        return SE::SceneSaver::ToJson(m_scene.currentDesc) != m_savedSceneJson;
    }

    // Opening another scene, reloading, or exiting first asks to save
    // unsaved edits; clean scenes proceed immediately.
    void RequestSceneAction(SceneAction action, const std::string& path = {})
    {
        if (action == SceneAction::Open && m_scene.IsLoading())
            return;
        m_pendingAction = action;
        m_pendingScenePath = path;
        if (IsSceneDirtyNow())
            m_openUnsavedPrompt = true;
        else
            PerformSceneAction();
    }

    void PerformSceneAction()
    {
        const SceneAction action = std::exchange(m_pendingAction, SceneAction::None);
        if (action == SceneAction::Open && !m_scene.IsLoading())
            m_scene.RequestSceneLoad(m_pendingScenePath);
        if (action == SceneAction::Exit)
        {
            GetWindow().SetCloseInterception(false);
            PostMessageW(GetWindow().GetHandle(), WM_CLOSE, 0, 0);
        }
    }

    static std::string FileName(const std::string& path)
    {
        const size_t pos = path.find_last_of("\\/");
        return pos == std::string::npos ? path : path.substr(pos + 1);
    }

    // Snapshot the serialized scene as the clean state and clear the marker.
    void TakeSavedSnapshot()
    {
        m_scene.SyncDescriptorFromRuntime();
        m_savedSceneJson = SE::SceneSaver::ToJson(m_scene.currentDesc);
        m_sceneDirty       = false;
        m_dirtyCheckFrames = 0;
        UpdateWindowTitle();
    }

    // Serialize-and-compare dirty check, throttled — catches every edit path
    // (details, gizmos, undo/redo) without per-widget change wiring.
    void UpdateDirtyFlag()
    {
        if (++m_dirtyCheckFrames < 30)
            return;
        m_dirtyCheckFrames = 0;
        m_scene.SyncDescriptorFromRuntime();
        const bool dirty = SE::SceneSaver::ToJson(m_scene.currentDesc) != m_savedSceneJson;
        if (dirty != m_sceneDirty)
        {
            m_sceneDirty = dirty;
            UpdateWindowTitle();
        }
    }

    void UpdateWindowTitle()
    {
        const std::string file = FileName(m_scene.currentScenePath);
        std::wstring title = L"FoxEditor \u2014 " + std::wstring(file.begin(), file.end());
        if (m_sceneDirty)
            title += L"*";
        SetTitleBase(title);
    }

    void BindViewportRT()
    {
        ID3D11DeviceContext* ctx = GetRenderer().GetContext();
        ID3D11RenderTargetView* rtv = m_viewportRT.GetRTV();
        const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        ctx->ClearRenderTargetView(rtv, clear);
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp = {};
        vp.Width    = static_cast<float>(m_viewportRT.GetWidth());
        vp.Height   = static_cast<float>(m_viewportRT.GetHeight());
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
    }

    void UpdateViewportRT()
    {
        const uint32_t w = (std::max)(1u, static_cast<uint32_t>(m_viewportSize.x));
        const uint32_t h = (std::max)(1u, static_cast<uint32_t>(m_viewportSize.y));
        if (m_viewportRT.GetRTV() && m_viewportRT.GetWidth() == w &&
            m_viewportRT.GetHeight() == h)
            return;
        m_viewportRT.Shutdown();
        m_viewportRT.Init(GetRenderer().GetDevice(), w, h, DXGI_FORMAT_R8G8B8A8_UNORM);
    }

    void DrawUI(bool sceneActive)
    {
        ImGuizmo::BeginFrame();
        DrawMainMenu();
        DrawUnsavedChangesModal();
        DrawSaveAsModal();

        ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        if (m_firstFrame || m_resetLayout)
        {
            BuildDefaultLayout(dockspaceId, m_resetLayout);
            m_firstFrame  = false;
            m_resetLayout = false;
        }

        DrawViewportPanel(sceneActive);
        HandleHotkeys();
        if (m_viewOptions.immersive) return;
        Editor::DrawOutlinerPanel(m_scene, m_selection, m_undo, SpawnPosition());
        Editor::DrawDetailsPanel(m_scene, m_selection, m_undo, GetAssets(),
                                 GetRenderer().GetDevice(), GetShaders(),
                                 m_viewportRT.GetWidth(), m_viewportRT.GetHeight());
        m_contentBrowser.Draw(m_scene, m_selection, m_undo, GetAssets(),
                              GetRenderer().GetDevice(), GetRenderer().GetContext(),
                              SpawnPosition());
    }

    void DrawMainMenu()
    {
        if (!ImGui::BeginMainMenuBar())
            return;
        if (ImGui::BeginMenu("File"))
        {
            const bool canSave = m_scene.HasScene() && !m_scene.IsLoading();
            DrawOpenSceneMenu();
            ImGui::Separator();
            if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, canSave))
                SaveScene();
            if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S", false, canSave))
                BeginSaveAs();
            if (ImGui::MenuItem("Reload Scene", nullptr, false, canSave))
                RequestSceneAction(SceneAction::Open, m_scene.currentScenePath);
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4"))
                RequestSceneAction(SceneAction::Exit);
            ImGui::EndMenu();
        }
        DrawEditMenu();
        DrawToolsMenu();
        if (ImGui::BeginMenu("Window"))
        {
            if (ImGui::MenuItem("Reset Layout"))
                m_resetLayout = true;
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    void DrawOpenSceneMenu()
    {
        if (!ImGui::BeginMenu("Open Scene", !m_scene.IsLoading()))
            return;
        const auto scenes = SE::SceneLoader::ScanSceneDirectory("Assets/Scenes");
        if (scenes.empty())
            ImGui::TextDisabled("No scenes in Assets/Scenes");
        for (const std::string& path : scenes)
        {
            const bool current = FileName(path) == FileName(m_scene.currentScenePath);
            if (ImGui::MenuItem(FileName(path).c_str(), nullptr, current))
                RequestSceneAction(SceneAction::Open, path);
        }
        ImGui::EndMenu();
    }

    void DrawEditMenu()
    {
        if (!ImGui::BeginMenu("Edit"))
            return;
        const std::string undoText = m_undo.CanUndo()
            ? std::string("Undo ") + m_undo.UndoLabel() : std::string("Undo");
        const std::string redoText = m_undo.CanRedo()
            ? std::string("Redo ") + m_undo.RedoLabel() : std::string("Redo");
        if (ImGui::MenuItem(undoText.c_str(), "Ctrl+Z", false, m_undo.CanUndo()))
            m_undo.Undo();
        if (ImGui::MenuItem(redoText.c_str(), "Ctrl+Y", false, m_undo.CanRedo()))
            m_undo.Redo();
        ImGui::Separator();
        const bool canEdit = m_scene.HasScene() && !m_scene.IsLoading();
        if (ImGui::MenuItem("Rename", "F2", false, canEdit))
            Editor::RequestRenameSelection(m_scene, m_selection);
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, canEdit))
            Editor::DuplicateSelection(m_scene, m_selection, m_undo);
        if (ImGui::MenuItem("Delete", "Del", false, canEdit))
            Editor::DeleteSelection(m_scene, m_selection, m_undo);
        ImGui::EndMenu();
    }

    void DrawUnsavedChangesModal()
    {
        constexpr const char* k_popup = "Unsaved Changes";
        if (std::exchange(m_openUnsavedPrompt, false))
        {
            m_saveError = false;
            ImGui::OpenPopup(k_popup);
        }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(k_popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;
        ImGui::Text("Save changes to %s before %s?",
                    FileName(m_scene.currentScenePath).c_str(),
                    m_pendingAction == SceneAction::Exit ? "exiting" : "closing it");
        if (m_saveError)
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Save failed; see log.");
        ImGui::Spacing();
        bool proceed = false;
        if (ImGui::Button("Save", ImVec2(110.0f, 0.0f)))
        {
            m_saveError = !SaveScene();
            proceed = !m_saveError;
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", ImVec2(110.0f, 0.0f)))
            proceed = true;
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_pendingAction = SceneAction::None;
            ImGui::CloseCurrentPopup();
        }
        if (proceed)
        {
            ImGui::CloseCurrentPopup();
            PerformSceneAction();
        }
        ImGui::EndPopup();
    }

    void BeginSaveAs()
    {
        std::string stem = FileName(m_scene.currentScenePath);
        if (const size_t dot = stem.rfind('.'); dot != std::string::npos)
            stem.resize(dot);
        sprintf_s(m_saveAsName, "%s", stem.c_str());
        m_saveError = false;
        m_openSaveAs = true;
    }

    // Save As writes <name>.json into Assets/Scenes and switches the editor
    // to the new file.
    void DrawSaveAsModal()
    {
        constexpr const char* k_popup = "Save Scene As";
        if (std::exchange(m_openSaveAs, false))
            ImGui::OpenPopup(k_popup);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(k_popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(320.0f);
        const bool entered = ImGui::InputText("##name", m_saveAsName, sizeof(m_saveAsName),
                                              ImGuiInputTextFlags_EnterReturnsTrue |
                                              ImGuiInputTextFlags_AutoSelectAll);
        std::string name = m_saveAsName;
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0)
            name.resize(name.size() - 5);
        const bool valid = !name.empty() &&
            name.find_first_of("\\/:*?\"<>|") == std::string::npos;
        const std::string path = "Assets/Scenes/" + name + ".json";
        if (!valid)
            ImGui::TextDisabled("Enter a file name");
        else
            ImGui::TextDisabled("%s", path.c_str());
        if (valid && std::filesystem::exists(path))
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Overwrites the existing file.");
        if (m_saveError)
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Save failed; see log.");

        ImGui::BeginDisabled(!valid);
        const bool save = ImGui::Button("Save", ImVec2(110.0f, 0.0f)) || (entered && valid);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        if (save)
        {
            m_saveError = !SaveSceneAs(path);
            if (!m_saveError)
                ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void DrawToolsMenu()
    {
        if (!ImGui::BeginMenu("Tools"))
            return;
        const bool hasScene = m_scene.HasScene();
        if (ImGui::MenuItem("Fire All Single Emitters", nullptr, false, hasScene))
        {
            for (auto& emitter : m_scene.particleSystems)
                if (emitter->config.emitMode == 1)
                    emitter->Burst(8);
        }
        if (ImGui::MenuItem("Rescan Particle Presets", nullptr, false, hasScene))
            m_scene.RefreshParticlePresets();
        ImGui::EndMenu();
    }

    // UE-style bindings: Q/W/E/R select gizmo op over the viewport,
    // Ctrl+Z / Ctrl+Y undo/redo anywhere outside text input.
    bool CanUseEditorShortcuts()
    {
        return m_scene.HasScene() && !m_scene.IsLoading() &&
            !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() &&
            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
            !m_editorCam.IsNavigating() && !GetInput().IsKeyDown(VK_RBUTTON);
    }

    void HandleHotkeys()
    {
        if (!CanUseEditorShortcuts()) return;
        const ImGuiIO& io = ImGui::GetIO();
        auto& input = GetInput();
        if (io.KeyCtrl)
        {
            HandleControlKeys(input);
            return;
        }
        if (io.KeyAlt)
        {
            if (m_viewportHovered) HandleViewModeKeys(input, io.KeyShift);
            return;
        }
        HandleEditKeys(input);
        if (m_viewportHovered) HandleViewportKeys(input);
    }

    void HandleControlKeys(SE::InputManager& input)
    {
        const bool shift = ImGui::GetIO().KeyShift;
        if (input.IsKeyPressed('Z')) shift ? m_undo.Redo() : m_undo.Undo();
        if (input.IsKeyPressed('Y')) m_undo.Redo();
        if (input.IsKeyPressed('S')) shift ? BeginSaveAs() : (void)SaveScene();
        if (input.IsKeyPressed('D')) Editor::DuplicateSelection(m_scene, m_selection, m_undo);
        if (m_viewportHovered && input.IsKeyPressed('R'))
            m_viewOptions.realtime = !m_viewOptions.realtime;
    }

    void HandleEditKeys(SE::InputManager& input)
    {
        if (input.IsKeyPressed(VK_DELETE)) Editor::DeleteSelection(m_scene, m_selection, m_undo);
        if (input.IsKeyPressed(VK_F2)) Editor::RequestRenameSelection(m_scene, m_selection);
        if (input.IsKeyPressed(VK_ESCAPE)) m_selection.Clear();
    }

    void HandleViewportKeys(SE::InputManager& input)
    {
        if (input.IsKeyPressed('Q')) m_gizmoOp = Editor::GizmoOp::None;
        if (input.IsKeyPressed('W')) m_gizmoOp = Editor::GizmoOp::Translate;
        if (input.IsKeyPressed('E')) m_gizmoOp = Editor::GizmoOp::Rotate;
        if (input.IsKeyPressed('R')) m_gizmoOp = Editor::GizmoOp::Scale;
        if (input.IsKeyPressed('F')) FocusSelection();
        if (input.IsKeyPressed(VK_SPACE))
            m_gizmoOp = static_cast<Editor::GizmoOp>(static_cast<int>(m_gizmoOp) % 3 + 1);
        if (input.IsKeyPressed('G')) m_viewOptions.gameView = !m_viewOptions.gameView;
        if (input.IsKeyPressed(VK_F11)) m_viewOptions.immersive = !m_viewOptions.immersive;
    }

    // Unreal view shortcuts. Shift chooses the opposite orthographic side.
    void HandleViewModeKeys(SE::InputManager& input, bool opposite)
    {
        if (!m_scene.HasScene() || !m_scene.camera)
            return;
        using VM = Editor::EditorCamera::ViewMode;
        auto set = [this](VM mode) { m_editorCam.SetViewMode(mode, *m_scene.camera); };

        if (input.IsKeyPressed('G')) set(VM::Perspective);
        if (input.IsKeyPressed('J')) set(opposite ? VM::Bottom : VM::Top);
        if (input.IsKeyPressed('H')) set(opposite ? VM::Back : VM::Front);
        if (input.IsKeyPressed('K')) set(opposite ? VM::Right : VM::Left);
    }

    // New entities spawn a few metres in front of the editor camera.
    DirectX::XMFLOAT3 SpawnPosition() const
    {
        using namespace DirectX;
        if (!m_scene.HasScene() || !m_scene.camera)
            return { 0.0f, 0.0f, 0.0f };
        const auto& cam = *m_scene.camera;
        const XMVECTOR eye = XMLoadFloat3(&cam.eye);
        const XMVECTOR dir = XMVector3Normalize(
            XMVectorSubtract(XMLoadFloat3(&cam.target), eye));
        XMFLOAT3 out;
        XMStoreFloat3(&out, XMVectorAdd(eye, XMVectorScale(dir, 8.0f)));
        return out;
    }

    void FocusSelection()
    {
        DirectX::XMFLOAT3 center;
        float radius;
        if (!Editor::GetSelectionBounds(m_scene, m_selection, center, radius))
            return;
        if (m_scene.camera)
        {
            m_editorCam.FocusOn(center, radius, *m_scene.camera,
                m_viewportSize.x / (std::max)(m_viewportSize.y, 1.0f));
            m_scene.ResetTemporalHistory();
        }
    }

    void BuildDefaultLayout(ImGuiID dockspaceId, bool force)
    {
        FILE* f = nullptr;
        fopen_s(&f, "editor.ini", "r");
        const bool hasIni = (f != nullptr);
        if (f) fclose(f);
        if (hasIni && !force)
            return;

        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

        ImGuiID dockCenter = dockspaceId;
        ImGuiID dockRight;
        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Right, 0.22f, &dockRight, &dockCenter);
        ImGuiID dockBottom;
        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Down, 0.28f, &dockBottom, &dockCenter);
        ImGuiID dockRightTop, dockRightBottom;
        ImGui::DockBuilderSplitNode(dockRight, ImGuiDir_Up, 0.30f, &dockRightTop, &dockRightBottom);

        ImGui::DockBuilderDockWindow("Viewport", dockCenter);

        ImGui::DockBuilderDockWindow("Outliner", dockRightTop);

        ImGui::DockBuilderDockWindow("Details", dockRightBottom);
        ImGui::DockBuilderDockWindow("Content Browser", dockBottom);
        ImGui::DockBuilderFinish(dockspaceId);
    }

    void DrawViewportPanel(bool sceneActive)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGuiWindowFlags flags = ImGuiWindowFlags_MenuBar;
        if (m_viewOptions.immersive)
        {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            flags |= ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoSavedSettings;
        }
        const char* title = m_viewOptions.immersive ? "Immersive Viewport" : "Viewport";
        if (!ImGui::Begin(title, nullptr, flags))
        {
            m_viewportHovered = false;
            ImGui::End();
            ImGui::PopStyleVar();
            return;
        }
        if (sceneActive)
            Editor::DrawViewportToolbar(m_editorCam, *m_scene.camera, m_gizmoOp,
                                         m_gizmoSettings, m_viewOptions, m_scene.pipeline);
        m_viewportHovered = false;
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        m_viewportSize = ImVec2((std::max)(avail.x, 1.0f), (std::max)(avail.y, 1.0f));

        if (sceneActive && m_viewportRT.GetSRV()) DrawViewportImage(avail);
        else DrawViewportLoadingOverlay();

        ImGui::End();
        ImGui::PopStyleVar();
    }

    void DrawViewportImage(ImVec2 size)
    {
        ImGui::Image(reinterpret_cast<ImTextureID>(m_viewportRT.GetSRV()), size);
        const ImVec2 imageMin = ImGui::GetItemRectMin();
        const ImVec2 imageMax = ImGui::GetItemRectMax();
        m_viewportHovered = ImGui::IsItemHovered();
        if (!m_viewOptions.gameView && m_viewOptions.showGrid)
            Editor::DrawOrthographicGrid(*m_scene.camera, imageMin, imageMax);
        DrawViewportOverlays(imageMin, imageMax);
        if (!m_viewOptions.gameView && m_viewOptions.showAxes)
            Editor::DrawOrientationAxes(*m_scene.camera, imageMin, imageMax);
        if (m_viewOptions.showStats) DrawViewportStats(imageMin, imageMax);
    }

    // Gizmo, light indicators, and click picking on top of the viewport image.
    void DrawViewportOverlays(ImVec2 imageMin, ImVec2 imageMax)
    {
        const bool navigating = m_editorCam.IsNavigating() || GetInput().IsKeyDown(VK_RBUTTON);
        bool gizmoBusy = false;
        bool lightClicked = false;
        if (!m_viewOptions.gameView && !navigating)
        {
            gizmoBusy = Editor::DrawGizmo(m_scene, m_selection, m_undo, m_gizmoOp,
                                           m_gizmoSettings, imageMin, imageMax);
            if (m_viewOptions.showLights)
                lightClicked = Editor::DrawLightIndicators(m_scene, imageMin, imageMax, m_selection);
        }
        HandleViewportPicking(imageMin, imageMax, gizmoBusy || lightClicked);
        m_viewportHovered = m_viewportHovered && !gizmoBusy && !lightClicked;
    }

    void HandleViewportPicking(ImVec2 imageMin, ImVec2 imageMax, bool blocked)
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            m_pendingPick = m_viewportHovered && !blocked && !io.KeyAlt;
        // Select on release, not press: LMB navigation must not change selection.
        if (blocked || IsPickingGestureCancelled())
            m_pendingPick = false;
        if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left)) return;
        if (m_pendingPick && m_viewportHovered)
            m_selection = Editor::PickScene(m_scene, imageMin, imageMax, ImGui::GetMousePos());
        m_pendingPick = false;
    }

    bool IsPickingGestureCancelled() const
    {
        return ImGui::GetIO().KeyAlt || ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
            ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    }

    // Frame/scene stats overlaid at the bottom-left of the viewport image.
    void DrawViewportStats(ImVec2 imageMin, ImVec2 imageMax)
    {
        uint32_t subMeshCount = 0;
        for (const auto& loaded : m_scene.loadedMeshes)
            if (loaded.mesh)
                subMeshCount += loaded.mesh->GetSubMeshCount();

        char line[160];
        sprintf_s(line, "%.1f fps  %.2f ms   |   %ux%u   |   meshes %u  tex %u  submeshes %u",
                  GetClock().GetFPS(), GetClock().GetDeltaTime() * 1000.0f,
                  m_viewportRT.GetWidth(), m_viewportRT.GetHeight(),
                  GetAssets().CachedMeshCount(), GetAssets().CachedTextureCount(),
                  subMeshCount);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 pos = { imageMin.x + 8.0f, imageMax.y - 24.0f };
        dl->AddText({ pos.x + 1.0f, pos.y + 1.0f }, IM_COL32(0, 0, 0, 200), line);
        dl->AddText(pos, IM_COL32(220, 220, 220, 230), line);
    }

    void DrawViewportLoadingOverlay()
    {
        const SE::LevelLoadStatus status = m_scene.GetLoadStatus();
        const char* phaseText = "READING LEVEL DATA";
        switch (status.state)
        {
        case SE::LevelLoadState::Preparing: phaseText = "PREPARING LEVEL DEPENDENCIES"; break;
        case SE::LevelLoadState::Uploading: phaseText = "UPLOADING LEVEL RESOURCES"; break;
        case SE::LevelLoadState::Complete:  phaseText = "READY"; break;
        case SE::LevelLoadState::Failed:    phaseText = "LOAD FAILED"; break;
        case SE::LevelLoadState::Cancelled: phaseText = "LOAD CANCELLED"; break;
        default: break;
        }

        const ImVec2 region = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPos(ImVec2(region.x * 0.5f - 120.0f, region.y * 0.5f - 20.0f));
        ImGui::BeginGroup();
        if (m_scene.sceneLoadError.empty())
        {
            ImGui::TextUnformatted(phaseText);
            ImGui::ProgressBar(std::clamp(status.progress, 0.0f, 1.0f),
                               ImVec2(240.0f, 6.0f), "");
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
                               m_scene.sceneLoadError.c_str());
            if (ImGui::Button("Retry"))
            {
                m_scene.sceneLoadError.clear();
                m_scene.RequestSceneLoad(m_scene.lastRequestedScenePath);
            }
        }
        ImGui::EndGroup();
    }

    SE::SceneRuntime      m_scene;
    Editor::EditorCamera  m_editorCam;
    Editor::Selection     m_selection;
    Editor::UndoStack     m_undo;
    Editor::ContentBrowser m_contentBrowser;
    Editor::GizmoOp       m_gizmoOp = Editor::GizmoOp::Translate;
    Editor::GizmoSettings m_gizmoSettings;
    Editor::ViewportOptions m_viewOptions;
    float m_previewTime = 0.0f;
    bool m_pendingPick = false;
    Editor::EditorCamera::ViewMode m_previousView = Editor::EditorCamera::ViewMode::Perspective;
    SE::ForwardPipeline::ViewShading m_previousShading = SE::ForwardPipeline::ViewShading::Lit;
    SE::RenderTarget      m_viewportRT;
    ImVec2           m_viewportSize    = ImVec2(1280.0f, 720.0f);
    bool             m_viewportHovered = false;
    bool             m_firstFrame      = true;
    bool             m_resetLayout     = false;

    std::string              m_savedSceneJson;
    bool                     m_sceneDirty       = false;
    int                      m_dirtyCheckFrames = 0;

    SceneAction              m_pendingAction     = SceneAction::None;
    std::string              m_pendingScenePath;
    bool                     m_openUnsavedPrompt = false;
    bool                     m_openSaveAs        = false;
    bool                     m_saveError         = false;
    char                     m_saveAsName[128]   = {};
};

// Set working directory to project root (walk up from exe location until we
// find Engine/ and Assets/).
static void SetProjectRootCwd()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; ++i)
    {
        if (std::filesystem::exists(dir / "Engine") && std::filesystem::exists(dir / "Assets"))
        {
            SetCurrentDirectoryW(dir.c_str());
            return;
        }
        dir = dir.parent_path();
    }
}

static std::string ParseSceneArg(LPSTR lpCmdLine)
{
    if (!lpCmdLine || strlen(lpCmdLine) == 0)
        return "";
    std::string args(lpCmdLine);
    auto pos = args.find("--scene");
    if (pos == std::string::npos)
        return "";
    pos += 7; // skip "--scene"
    while (pos < args.size() && (args[pos] == ' ' || args[pos] == '=')) ++pos;
    auto end = args.find(' ', pos);
    std::string scenePath = args.substr(pos, end - pos);
    if (scenePath.find('/') == std::string::npos && scenePath.find('\\') == std::string::npos)
        scenePath = "Assets/Scenes/" + scenePath;
    return scenePath;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int)
{
    SetProjectRootCwd();

    SE::WindowDesc desc;
    desc.title  = L"FoxEditor";
    desc.width  = 1600;
    desc.height = 900;

    std::string scenePath = ParseSceneArg(lpCmdLine);

    EditorApp app;
    if (!app.Initialize(desc)) return 1;
    if (!app.Setup(scenePath)) return 1;
    app.Run();
    app.Shutdown();
    return 0;
}
