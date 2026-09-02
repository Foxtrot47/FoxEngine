#include <windows.h>
#include <DirectXMath.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <filesystem>
#include <cmath>
#include "Engine/Core/Engine.h"
#include "Engine/Core/Logger.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Scene/SceneLoader.h"
#include "Engine/Scene/SceneRuntime.h"
#include "Engine/Input/GamepadState.h"

using namespace DirectX;

class TestScene : public SE::Engine
{
public:
    ~TestScene()
    {
        m_scene.Shutdown();
    }

    void Shutdown()
    {
        // The load worker may still be creating D3D resources. Join it before
        // the renderer and device are released by the base engine shutdown.
        m_scene.Shutdown();
        SE::Engine::Shutdown();
    }

    bool Setup(const std::string& scenePath = "")
    {
        if (!m_scene.Init(GetRenderer(), GetShaders(), GetAssets(), GetInput(),
                          GetWindow().GetHandle(),
                          GetWindow().GetWidth(), GetWindow().GetHeight()))
            return false;

        // Scan available scenes
        m_sceneFiles = SE::SceneLoader::ScanSceneDirectory("Assets/Scenes");
        if (m_sceneFiles.empty())
        {
            SE_LOG_ERROR("No scene files found in Assets/Scenes/");
            return false;
        }

        // Determine which scene to load
        std::string targetScene = scenePath;
        if (targetScene.empty() && !m_sceneFiles.empty())
            targetScene = m_sceneFiles[0];

        // Queue the first load so the render loop can present loading feedback
        // before potentially expensive asset import begins.
        m_initialScenePath = targetScene;
        m_scene.RequestSceneLoad(targetScene);
        m_initialSceneLoad = true;
        SE_LOG_INFO("TestScene initial scene queued — %s", targetScene.c_str());
        return true;
    }

protected:
    void OnUpdate() override
    {
        if (m_scene.TickSceneLoad([this]() { return GetWindow().PumpMessages(); }))
            OnSceneReady();

        if (m_initialSceneLoad)
        {
            if (!m_scene.sceneLoadError.empty())
            {
                if (GetInput().IsKeyPressed('R'))
                {
                    m_scene.sceneLoadError.clear();
                    m_scene.RequestSceneLoad(m_initialScenePath);
                }
                if (GetInput().IsKeyPressed(VK_ESCAPE))
                    PostMessageW(GetWindow().GetHandle(), WM_CLOSE, 0, 0);
            }
            DrawLoadingUI();
            return;
        }
        if (m_scene.IsLoading())
        {
            DrawLoadingUI();
            return;
        }

        float dt     = GetClock().GetDeltaTime();
        float aspect = (float)GetWindow().GetWidth() / (float)GetWindow().GetHeight();

        UpdateCameraAndCharacter(dt);
        m_scene.Update(dt, static_cast<float>(GetClock().GetTotalTime()));
        m_scene.Render(aspect);
        DrawUI();
    }

    void OnPostProcess() override
    {
        if (m_initialSceneLoad || m_scene.IsLoading())
        {
            // BeginFrame clears the MSAA surface, not the swap chain. Resolve it so
            // the loading UI is drawn over a known background before Present.
            GetRenderer().ResolveToBackBuffer();
            return;
        }

        m_scene.PostProcess(GetWindow().GetWidth(), GetWindow().GetHeight(),
                            GetClock().GetDeltaTime(),
                            [this]() { GetRenderer().BindBackBuffer(GetRenderer().GetContext()); });
    }

private:
    // Sync UI state that mirrors the newly applied scene.
    void OnSceneReady()
    {
        m_initialSceneLoad = false;
        for (int i = 0; i < (int)m_sceneFiles.size(); ++i)
            if (m_sceneFiles[i] == m_scene.currentScenePath) { m_selectedScene = i; break; }

        const std::string& name = m_scene.currentDesc.name;
        std::wstring title = L"FoxEngine " + std::wstring(name.begin(), name.end());
        SetWindowTextW(GetWindow().GetHandle(), title.c_str());
    }

    void UpdateCameraAndCharacter(float dt)
    {
        bool mouseBlocked = ImGui::GetIO().WantCaptureMouse;
        SE::CameraController::Mode prevMode = m_scene.camCtrl.GetMode();

        m_scene.camCtrl.Update(dt, GetInput(), *m_scene.camera,
                               GetWindow().GetHandle(), mouseBlocked);

        SE::CameraController::Mode mode = m_scene.camCtrl.GetMode();

        // Init character position when first entering FPS mode.
        if (prevMode == SE::CameraController::Mode::FreeFly &&
            mode     == SE::CameraController::Mode::FPS)
        {
            m_scene.cc.position = { m_scene.camera->eye.x,
                                    m_scene.camera->eye.y - m_scene.cc.eyeHeight,
                                    m_scene.camera->eye.z };
            m_scene.cc.velY = 0.0f;
        }

        // Drive character + camera in FPS mode.
        if (mode == SE::CameraController::Mode::FPS)
        {
            auto& inp = GetInput();
            auto& cc  = m_scene.cc;
            float yawRad = XMConvertToRadians(m_scene.camCtrl.fps.yawDeg);
            XMFLOAT3 fwd   = { sinf(yawRad), 0.0f,  cosf(yawRad) };
            XMFLOAT3 right = { cosf(yawRad), 0.0f, -sinf(yawRad) };

            XMFLOAT3 wishVel = { 0.0f, 0.0f, 0.0f };
            if (inp.IsKeyDown('W')) { wishVel.x += fwd.x;   wishVel.z += fwd.z; }
            if (inp.IsKeyDown('S')) { wishVel.x -= fwd.x;   wishVel.z -= fwd.z; }
            if (inp.IsKeyDown('D')) { wishVel.x += right.x; wishVel.z += right.z; }
            if (inp.IsKeyDown('A')) { wishVel.x -= right.x; wishVel.z -= right.z; }

            // Normalise diagonal, then scale to moveSpeed.
            float len2 = wishVel.x * wishVel.x + wishVel.z * wishVel.z;
            if (len2 > 1.0f) {
                float inv = 1.0f / sqrtf(len2);
                wishVel.x *= inv; wishVel.z *= inv;
            }
            wishVel.x *= cc.moveSpeed;
            wishVel.z *= cc.moveSpeed;

            if (inp.IsKeyPressed(VK_SPACE)) cc.Jump();

            m_scene.physicsWorld.StepCharacter(cc, wishVel, dt);

            // Sync camera to character eye.
            float pitchRad = XMConvertToRadians(m_scene.camCtrl.fps.pitchDeg);
            XMFLOAT3 eye  = cc.GetEyePosition();
            XMFLOAT3 look = {
                sinf(yawRad) * cosf(pitchRad),
                sinf(pitchRad),
                cosf(yawRad) * cosf(pitchRad)
            };
            m_scene.camera->eye    = eye;
            m_scene.camera->target = { eye.x + look.x, eye.y + look.y, eye.z + look.z };
            m_scene.camera->up     = { 0.0f, 1.0f, 0.0f };
        }
    }

    // FSR 3.1 and the engine TAA are mutually exclusive temporal AA paths.
    void DrawTemporalAAUI()
    {
        int method = m_scene.fsrEnabled ? 1 : (m_scene.taa.enabled ? 2 : 0);
        const char* methods[] = { "Off", "FSR 3.1", "Engine TAA" };
        if (ImGui::Combo("Method", &method, methods, IM_ARRAYSIZE(methods)))
        {
            m_scene.fsrEnabled  = method == 1;
            m_scene.taa.enabled = method == 2;
            m_scene.taa.ResetHistory();
        }
        if (m_scene.fsrEnabled)
            DrawFsrUI();
        else if (m_scene.taa.enabled)
            DrawTaaUI();
    }

    void DrawFsrUI()
    {
        const char* modes[] = { "Quality (1.5x)", "Balanced (1.7x)",
                                "Performance (2.0x)", "Native AA (1.0x)" };
        ImGui::Combo("Mode", &m_scene.fsrQuality, modes, IM_ARRAYSIZE(modes));
        ImGui::Checkbox("Sharpen", &m_scene.fsrSharpen);
        if (m_scene.fsrSharpen)
            ImGui::SliderFloat("Sharpness", &m_scene.fsrSharpness, 0.0f, 1.0f, "%.2f");
        if (ImGui::SliderFloat("Velocity Factor", &m_scene.fsrVelocityFactor, 0.0f, 1.0f, "%.2f"))
            m_scene.fsr.SetVelocityFactor(m_scene.fsrVelocityFactor);
        ImGui::Text("Render: %ux%u -> %ux%u",
                    m_scene.fsr.RenderWidth(), m_scene.fsr.RenderHeight(),
                    GetWindow().GetWidth(), GetWindow().GetHeight());
    }

    void DrawTaaUI()
    {
        ImGui::SliderFloat("History Blend", &m_scene.taa.blendFactor, 0.5f, 0.98f, "%.2f");
        ImGui::SliderFloat("Clamp Width", &m_scene.taa.clampGamma, 0.5f, 2.5f, "%.2f");
        if (ImGui::Button("Reset History"))
            m_scene.taa.ResetHistory();
    }

    void DrawLoadingUI()
    {
        auto getFilename = [](const std::string& path) -> std::string {
            auto pos = path.find_last_of("\\/");
            return (pos != std::string::npos) ? path.substr(pos + 1) : path;
        };

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

        const float progress = std::clamp(status.progress, 0.0f, 1.0f);
        const std::string& path = m_scene.GetLoadingScenePath().empty()
            ? m_initialScenePath : m_scene.GetLoadingScenePath();
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        constexpr float overlayHeight = 64.0f;
        constexpr float barHeight = 6.0f;
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x,
                                      viewport->Pos.y + viewport->Size.y - overlayHeight),
                                ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, overlayHeight), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 6.0f));
        ImGui::Begin("##LevelLoadingStrip", nullptr,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoInputs);

        // blue (#00A0DE), with a dark shadow so the
        // borderless overlay remains legible over bright level backgrounds.
        constexpr ImU32 blue = IM_COL32(0, 160, 222, 255);
        constexpr ImU32 textShadow = IM_COL32(0, 18, 32, 230);
        const ImVec4 blueFloat(0.0f, 160.0f / 255.0f, 222.0f / 255.0f, 1.0f);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        auto drawShadow = [&](const char* text) {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            drawList->AddText(ImVec2(pos.x + 2.0f, pos.y + 2.0f), textShadow, text);
        };

        char levelText[256];
        sprintf_s(levelText, "// LOADING LEVEL: %s", getFilename(path).c_str());
        drawShadow(levelText);
        ImGui::TextColored(blueFloat, "%s", levelText);
        if (m_scene.sceneLoadError.empty())
        {
            drawShadow(phaseText);
            ImGui::TextColored(blueFloat, "%s", phaseText);
        }
        else
        {
            char errorText[256];
            sprintf_s(errorText, "%s  //  [R] RETRY   [ESC] QUIT",
                      m_scene.sceneLoadError.c_str());
            drawShadow(errorText);
            ImGui::TextColored(ImVec4(1.0f, 0.18f, 0.08f, 1.0f), "%s", errorText);
        }


        const ImVec2 barMin(viewport->Pos.x,
                            viewport->Pos.y + viewport->Size.y - barHeight);
        const ImVec2 barMax(viewport->Pos.x + viewport->Size.x * progress,
                            viewport->Pos.y + viewport->Size.y);
        drawList->AddRectFilled(barMin, barMax, blue);

        ImGui::End();
        ImGui::PopStyleVar();
    }

    void DrawPointLightsPanel()
    {
        auto& lights = m_scene.lights;
        ImGui::SliderInt("Active", &lights.numLights, 0, 8);
        for (int i = 0; i < lights.numLights; ++i)
        {
            ImGui::PushID(i);
            char label[24];
            sprintf_s(label, "Point Light %d", i + 1);
            if (ImGui::TreeNode(label))
            {
                ImGui::DragFloat3("Position", &lights.lights[i].position.x, 1.0f, -1000.0f, 1000.0f);
                if (i < static_cast<int>(m_scene.pointLightDescs.size()))
                {
                    // Scene-driven light: edit the base color; the per-frame
                    // flicker factor is applied on top of it.
                    auto& source = m_scene.pointLightDescs[i];
                    ImGui::ColorEdit3("Color", source.color.data(),
                                      ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
                    ImGui::Checkbox("Flicker", &source.flicker);
                    if (source.flicker)
                    {
                        ImGui::SliderFloat("Amplitude", &source.flickerAmplitude, 0.0f, 1.0f);
                        ImGui::SliderFloat("Frequency", &source.flickerFrequency, 0.5f, 20.0f);
                    }
                }
                else
                {
                    ImGui::ColorEdit3("Color", &lights.lights[i].color.x,
                                      ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
                }
                ImGui::SliderFloat("Radius",  &lights.lights[i].radius, 0.5f, 500.0f);
                if (i < 2)
                    ImGui::Checkbox("Cast Shadow", &m_scene.lightCastsShadow[i]);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }

    void DrawUI()
    {
        XMMATRIX view = m_scene.GetView();
        XMMATRIX proj = m_scene.GetProj();

        // Full-viewport dockspace
        ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
            ImGuiDockNodeFlags_PassthruCentralNode);

        // Build default layout only on very first run (no saved layout)
        if (m_firstFrame)
        {
            m_firstFrame = false;

            // Check if imgui.ini exists — if not, build a programmatic default
            FILE* f = nullptr;
            fopen_s(&f, "imgui.ini", "r");
            bool hasIni = (f != nullptr);
            if (f) fclose(f);

            if (!hasIni)
            {
                ImGui::DockBuilderRemoveNode(dockspaceId);
                ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

                // Right sidebar (~14% width)
                ImGuiID dockCenter = dockspaceId;
                ImGuiID dockRight;
                ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Right, 0.14f, &dockRight, &dockCenter);

                // Split right sidebar into 4 vertical sections
                ImGuiID dockTop, dockRest;
                ImGui::DockBuilderSplitNode(dockRight, ImGuiDir_Up, 0.12f, &dockTop, &dockRest);
                ImGuiID dockMid, dockLower;
                ImGui::DockBuilderSplitNode(dockRest, ImGuiDir_Up, 0.35f, &dockMid, &dockLower);
                ImGuiID dockBottom, dockFinal;
                ImGui::DockBuilderSplitNode(dockLower, ImGuiDir_Up, 0.55f, &dockBottom, &dockFinal);

                // Top tabs: Scene active (last = default)
                ImGui::DockBuilderDockWindow("Camera", dockTop);
                ImGui::DockBuilderDockWindow("Scene",  dockTop);

                // Mid tabs: Particles active (last = default)
                ImGui::DockBuilderDockWindow("Physics",   dockMid);
                ImGui::DockBuilderDockWindow("Particles", dockMid);

                // Bottom tabs: Lighting active (last = default)
                ImGui::DockBuilderDockWindow("Material", dockBottom);
                ImGui::DockBuilderDockWindow("Lighting", dockBottom);

                // Final row: Post Processing
                ImGui::DockBuilderDockWindow("Post Processing", dockFinal);

                ImGui::DockBuilderFinish(dockspaceId);
            }
        }

        // On frames 2-4, force focus on desired default tabs one at a time
        m_frameCount++;
        if (m_frameCount == 2)
            ImGui::SetWindowFocus("Scene");
        else if (m_frameCount == 3)
            ImGui::SetWindowFocus("Particles");
        else if (m_frameCount == 4)
            ImGui::SetWindowFocus("Lighting");

        // --- Input ---
        ImGui::Begin("Input");
        const SE::GamepadState& gp = GetInput().GetGamepad(0);
        if (gp.connected)
        {
            ImGui::Text("Controller 0  connected");
            ImGui::Separator();
            ImGui::Text("Left  stick  (%.2f, %.2f)", gp.leftX,  gp.leftY);
            ImGui::Text("Right stick  (%.2f, %.2f)", gp.rightX, gp.rightY);
            ImGui::Text("LT  %.2f   RT  %.2f",       gp.leftTrigger, gp.rightTrigger);
        }
        else
        {
            ImGui::TextDisabled("Controller 0  not connected");
        }
        ImGui::End();

        // HUD — FPS + asset stats rendered directly on screen (no window chrome)
        {
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            char buf[128];
            sprintf_s(buf, "%.1f fps  |  %.2f ms",
                GetClock().GetFPS(), GetClock().GetDeltaTime() * 1000.0f);
            dl->AddText(ImVec2(10.0f, 10.0f), IM_COL32(255, 255, 255, 220), buf);
            uint32_t subMeshCount = 0;
            for (const auto& loaded : m_scene.loadedMeshes)
                if (loaded.mesh)
                    subMeshCount += loaded.mesh->GetSubMeshCount();
            sprintf_s(buf, "meshes:%u  textures:%u  submeshes:%u",
                GetAssets().CachedMeshCount(), GetAssets().CachedTextureCount(),
                subMeshCount);
            dl->AddText(ImVec2(10.0f, 26.0f), IM_COL32(200, 200, 200, 180), buf);
        }

        // --- Scene Picker ---
        auto getFilename = [](const std::string& path) -> std::string {
            auto pos = path.find_last_of("\\/");
            return (pos != std::string::npos) ? path.substr(pos + 1) : path;
        };

        ImGui::Begin("Scene");
        if (!m_sceneFiles.empty())
        {
            if (ImGui::BeginCombo("Scene File", getFilename(m_sceneFiles[m_selectedScene]).c_str()))
            {
                for (int i = 0; i < (int)m_sceneFiles.size(); ++i)
                {
                    bool selected = (i == m_selectedScene);
                    if (ImGui::Selectable(getFilename(m_sceneFiles[i]).c_str(), selected))
                    {
                        if (i != m_selectedScene)
                        {
                            m_scene.RequestSceneLoad(m_sceneFiles[i]);
                        }
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::Button("Reload"))
                m_scene.RequestSceneLoad(m_sceneFiles[m_selectedScene]);
            ImGui::SameLine();
            if (ImGui::Button("Rescan"))
            {
                m_sceneFiles = SE::SceneLoader::ScanSceneDirectory("Assets/Scenes");
                m_scene.RefreshParticlePresets();
            }
        }
        ImGui::Text("Active: %s", m_scene.currentDesc.name.c_str());
        if (!m_scene.sceneLoadError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", m_scene.sceneLoadError.c_str());
        ImGui::End();

        // --- Camera ---
        ImGui::Begin("Camera");
        ImGui::Text("[Tab] to switch mode");
        if (m_scene.camCtrl.GetMode() == SE::CameraController::Mode::FPS)
        {
            auto& cc = m_scene.cc;
            ImGui::Text("FPS — WASD move  RMB look  Space jump");
            ImGui::SliderFloat("Move Speed", &cc.moveSpeed, 1.0f, 20.0f);
            ImGui::SliderFloat("Jump Speed", &cc.jumpSpeed, 2.0f, 20.0f);
            ImGui::Text("pos (%.1f, %.1f, %.1f)  vy=%.2f  %s",
                cc.position.x, cc.position.y, cc.position.z,
                cc.velY, cc.isGrounded ? "GROUNDED" : "air");
            ImGui::Text("contact n (%.2f, %.2f, %.2f)  pvxz (%.1f, %.1f)",
                cc.contactNormal.x, cc.contactNormal.y, cc.contactNormal.z,
                cc.physVelX, cc.physVelZ);
        }
        else
        {
            ImGui::Text("Free Fly — WASD/QE move  RMB look  Shift fast");
            ImGui::SliderFloat("Move Speed",  &m_scene.camCtrl.freeFly.moveSpeed,   1.0f, 200.0f);
            ImGui::SliderFloat("Sensitivity", &m_scene.camCtrl.freeFly.sensitivity, 0.05f, 0.5f);
        }
        ImGui::SliderFloat("Near Z", &m_scene.camera->nearZ, 0.001f, 10.0f, "%.3f");
        ImGui::SliderFloat("Far Z", &m_scene.camera->farZ, 100.0f, 20000.0f, "%.0f");
        ImGui::Text("Eye (%.1f, %.1f, %.1f)",
            m_scene.camera->eye.x, m_scene.camera->eye.y, m_scene.camera->eye.z);
        ImGui::End();

        // --- Material ---
        ImGui::Begin("Material");
        ImGui::ColorEdit3("Albedo Tint",      m_scene.matTint);
        ImGui::SliderFloat("Roughness Scale", &m_scene.roughnessScale, 0.0f, 2.0f);
        ImGui::SliderFloat("Metallic",        &m_scene.metallic,       0.0f, 1.0f);
        ImGui::End();

        // --- Physics ---
        ImGui::Begin("Physics");
        ImGui::Checkbox("Show Colliders", &m_scene.showColliders);
        ImGui::SameLine();
        ImGui::Checkbox("Raycast", &m_scene.castRay);
        if (ImGui::Checkbox("Gravity", &m_scene.gravityEnabled))
        {
            m_scene.ballRigidBody->useGravity = m_scene.gravityEnabled;
            m_scene.cc.gravityEnabled         = m_scene.gravityEnabled;
        }
        ImGui::Separator();
        ImGui::Text("Ball");
        ImGui::DragFloat3("Spawn pos",    &m_scene.ballSpawn.x,                1.0f);
        ImGui::SliderFloat("Radius",      &m_scene.ballRadius,                 0.1f, 10.0f);
        ImGui::SliderFloat("Mass",        &m_scene.ballRigidBody->mass,        0.1f, 10.0f);
        ImGui::SliderFloat("Restitution", &m_scene.ballRigidBody->restitution, 0.0f, 1.0f);
        ImGui::SliderFloat("Friction",    &m_scene.ballRigidBody->friction,    0.0f, 1.0f);
        if (ImGui::SliderFloat("Floor Y", &m_scene.floorY, -50.0f, 50.0f))
        {
            m_scene.physicsWorld.Clear();
            m_scene.physicsWorld.AddSphere(m_scene.ballTransform, m_scene.ballRigidBody,
                                           m_scene.ballRadius);
            m_scene.obbFloor = SE::OBB::FromAABB({ -30.0f, m_scene.floorY - 0.5f, -30.0f },
                                                 {  30.0f, m_scene.floorY,        30.0f });
            m_scene.physicsWorld.AddStaticOBB(m_scene.obbFloor, 0.6f, 0.4f);
        }
        ImGui::Text("pos (%.1f, %.1f, %.1f)",
            m_scene.ballTransform->position.x, m_scene.ballTransform->position.y, m_scene.ballTransform->position.z);
        ImGui::Text("vel (%.2f, %.2f, %.2f)",
            m_scene.ballRigidBody->velocity.x, m_scene.ballRigidBody->velocity.y, m_scene.ballRigidBody->velocity.z);
        if (ImGui::Button("Reset"))     m_scene.ResetBall();
        ImGui::SameLine();
        if (ImGui::Button("Launch up")) m_scene.ballRigidBody->AddImpulse({ 0.0f, 20.0f, 0.0f });
        if (m_scene.castRay)
        {
            ImGui::Separator();
            ImGui::Text("Raycast");
            if (m_scene.rayHitValid)
            {
                const auto& hit = m_scene.rayHit;
                const char* what =
                    hit.kind == SE::PhysicsWorld::RaycastHit::Kind::Sphere ? "Sphere" :
                    hit.kind == SE::PhysicsWorld::RaycastHit::Kind::OBB    ? "OBB"    : "Floor";
                ImGui::Text("Hit: %s  t=%.2f", what, hit.t);
                ImGui::Text("pos  (%.1f, %.1f, %.1f)",
                    hit.point.x,  hit.point.y,  hit.point.z);
                ImGui::Text("norm (%.2f, %.2f, %.2f)",
                    hit.normal.x, hit.normal.y, hit.normal.z);
            }
            else
            {
                ImGui::TextDisabled("no hit");
            }
        }
        ImGui::End();

        // --- Lighting ---
        ImGui::Begin("Lighting");
        if (ImGui::CollapsingHeader("Sun", ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto& lights = m_scene.lights;
            ImGui::SliderFloat("Elevation",    &lights.elevDeg,  -90.0f, 90.0f,  "%.1f deg");
            ImGui::SliderFloat("Azimuth",      &lights.azimDeg, -180.0f, 180.0f, "%.1f deg");
            ImGui::SliderFloat("Intensity",    &lights.lightIntensity, 0.0f, 10.0f, "%.2f");
            ImGui::ColorEdit3("Light Color",   lights.lightColor);
            ImGui::SliderFloat("IBL Intensity", &lights.iblIntensity, 0.0f, 5.0f, "%.2f");
            ImGui::SliderFloat("Diffuse IBL Saturation", &lights.diffuseIBLSaturation,
                               0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Shadow Bias", &m_scene.pointShadowBias, 0.001f, 0.1f, "%.4f");
        }
        if (ImGui::CollapsingHeader("Spot Light"))
        {
            auto& spot = m_scene.spotLight;
            ImGui::Checkbox("Enable Spot", &spot.enabled);
            if (spot.enabled)
            {
                if (ImGui::Button("Aim From Camera"))
                {
                    XMVECTOR eye = XMLoadFloat3(&m_scene.camera->eye);
                    XMVECTOR target = XMLoadFloat3(&m_scene.camera->target);
                    XMVECTOR dir = XMVector3Normalize(XMVectorSubtract(target, eye));
                    spot.position = m_scene.camera->eye;
                    XMStoreFloat3(&spot.direction, dir);
                }
                ImGui::DragFloat3("Spot Pos", &spot.position.x, 0.5f, -500.0f, 500.0f);
                ImGui::DragFloat3("Spot Dir", &spot.direction.x, 0.01f, -1.0f, 1.0f);
                ImGui::ColorEdit3("Spot Color", &spot.color.x);
                ImGui::SliderFloat("Spot Range", &spot.range, 1.0f, 200.0f);
                ImGui::SliderFloat("Spot Inner", &spot.innerAngle, 1.0f, spot.outerAngle - 0.5f);
                ImGui::SliderFloat("Spot Outer", &spot.outerAngle, spot.innerAngle + 0.5f, 89.0f);
                ImGui::SliderFloat("Spot Intensity", &spot.intensity, 0.1f, 50.0f);
                ImGui::SliderFloat("Spot Volumetric", &spot.volumetricIntensity, 0.0f, 10.0f, "%.2f");
                ImGui::SliderFloat("Spot Bias", &spot.shadowBias, 0.0001f, 0.01f, "%.4f");
            }
        }
        if (ImGui::CollapsingHeader("Point Lights"))
            DrawPointLightsPanel();
        if (ImGui::CollapsingHeader("Debug"))
        {
            ImGui::Checkbox("Show Shadow Factor", &m_scene.debugShadow);
            int dlm = (int)m_scene.lights.debugLightMode;
            ImGui::RadioButton("Normal",    &dlm, 0); ImGui::SameLine();
            ImGui::RadioButton("Force Lit", &dlm, 1); ImGui::SameLine();
            ImGui::RadioButton("NdotL",     &dlm, 2); ImGui::SameLine();
            ImGui::RadioButton("Roughness", &dlm, 3);
            m_scene.lights.debugLightMode = (float)dlm;
        }
        ImGui::End();

        // --- Post Processing ---
        ImGui::Begin("Post Processing");
        if (ImGui::CollapsingHeader("Tone Mapping", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderFloat("Exposure", &m_scene.toneMap.exposure, 0.01f, 10.0f, "%.2f");
            {
                int op = static_cast<int>(m_scene.toneMap.op);
                ImGui::RadioButton("Reinhard", &op, 0); ImGui::SameLine();
                ImGui::RadioButton("ACES",     &op, 1);
                m_scene.toneMap.op = static_cast<SE::ToneMap::Operator>(op);
            }
            ImGui::Checkbox("Gamma Correct", &m_scene.toneMap.gammaCorrect);
        }
        if (ImGui::CollapsingHeader("Bloom"))
        {
            auto& bloom = m_scene.bloom;
            ImGui::Checkbox("Enable Bloom", &bloom.enabled);
            if (bloom.enabled)
            {
                ImGui::SliderFloat("Threshold", &bloom.threshold, 0.0f, 4.0f,  "%.2f");
                ImGui::SliderFloat("Strength",  &bloom.intensity, 0.0f, 0.5f,  "%.3f");
                ImGui::SliderFloat("Scatter",   &bloom.scatter,   0.0f, 1.0f,  "%.2f");
            }
        }
        if (ImGui::CollapsingHeader("SSAO"))
        {
            auto& ssao = m_scene.ssao;
            ImGui::Checkbox("Enable SSAO", &ssao.enabled);
            if (ssao.enabled)
            {
                ImGui::SliderFloat("AO Radius", &ssao.radius, 0.01f, 5.0f, "%.2f");
                ImGui::SliderFloat("AO Bias",   &ssao.bias,   0.001f, 0.1f, "%.3f");
                ImGui::SliderFloat("AO Power",  &ssao.power,  0.5f, 8.0f,   "%.1f");
            }
        }
        if (ImGui::CollapsingHeader("Volumetric Fog"))
        {
            auto& volFog = m_scene.volFog;
            ImGui::Checkbox("Enable Fog", &volFog.enabled);
            if (volFog.enabled)
            {
                ImGui::SliderFloat("Density",       &volFog.density,          0.0f, 0.5f,  "%.3f");
                ImGui::SliderFloat("Anisotropy",    &volFog.anisotropy,      -0.9f, 0.95f, "%.2f");
                ImGui::SliderFloat("Sun Scatter",   &volFog.scatterIntensity, 0.0f, 5.0f,  "%.2f");
                ImGui::SliderInt("Steps",           &volFog.steps,            8, 128);
                ImGui::SliderFloat("Max Distance",  &volFog.maxDistance,      5.0f, 300.0f, "%.0f");
                ImGui::ColorEdit3("Fog Tint",       volFog.ambientColor);
                ImGui::SliderFloat("Ambient Fog",   &volFog.ambientIntensity, 0.0f, 2.0f,  "%.3f");
                ImGui::SliderFloat("Height Falloff",&volFog.heightFalloff,    0.0f, 1.0f,  "%.3f");
                ImGui::SliderFloat("Height Base",   &volFog.heightBase,      -10.0f, 20.0f, "%.1f");
                ImGui::SliderFloat("Noise",         &volFog.noiseStrength,    0.0f, 1.0f,  "%.2f");
                ImGui::SliderFloat("Noise Scale",   &volFog.noiseScale,       0.001f, 1.0f, "%.3f");
                ImGui::SliderFloat("Noise Drift",   &volFog.noiseSpeed,       0.0f, 2.0f,  "%.2f");
                ImGui::SliderFloat("Near Clear Radius", &volFog.nearClearDistance, 0.0f, 1000.0f, "%.1f");
                ImGui::SliderFloat("Distance Density", &volFog.distanceDensity, 0.0f, 8.0f, "%.2f");
            }
        }
        if (ImGui::CollapsingHeader("Temporal AA / Upscaling"))
            DrawTemporalAAUI();
        if (ImGui::CollapsingHeader("FXAA"))
        {
            auto& fxaa = m_scene.fxaa;
            ImGui::Checkbox("Enable FXAA", &fxaa.enabled);
            if (fxaa.enabled)
            {
                ImGui::SliderFloat("Subpix Quality", &fxaa.subpixQuality, 0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("Edge Threshold", &fxaa.edgeThreshold, 0.063f, 0.333f, "%.3f");
                ImGui::SliderFloat("Edge Min",       &fxaa.edgeThresholdMin, 0.0f, 0.1f, "%.4f");
            }
        }
        ImGui::End();

        // --- Particles ---
        ImGui::Begin("Particles");
        ImGui::Text("Emitters: %d", (int)m_scene.particleSystems.size());
        ImGui::SameLine();
        if (ImGui::Button("Rescan Presets"))
            m_scene.RefreshParticlePresets();
        if (ImGui::Button("Fire All Single Emitters"))
        {
            for (auto& emitter : m_scene.particleSystems)
            {
                if (emitter->config.emitMode == 1)
                    emitter->Burst(8);
            }
        }
        ImGui::Separator();
        for (int i = 0; i < (int)m_scene.particleSystems.size(); ++i)
        {
            auto& ps = *m_scene.particleSystems[i];
            ImGui::PushID(i);
            char label[32];
            sprintf_s(label, "Emitter %d", i);
            if (ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Text("Preset");
                if (!m_scene.particlePresetFiles.empty())
                {
                    auto& presetFiles = m_scene.particlePresetFiles;
                    int& selected = m_scene.selectedParticlePreset[i];
                    std::string previewName = (selected >= 0 && selected < (int)presetFiles.size())
                        ? getFilename(presetFiles[selected])
                        : "Select preset...";
                    if (ImGui::BeginCombo("Preset File", previewName.c_str()))
                    {
                        for (int p = 0; p < (int)presetFiles.size(); ++p)
                        {
                            bool isSelected = (p == selected);
                            if (ImGui::Selectable(getFilename(presetFiles[p]).c_str(), isSelected))
                                selected = p;
                            if (isSelected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    if (selected >= 0 && selected < (int)presetFiles.size())
                    {
                        if (ImGui::Button("Load Preset"))
                        {
                            SE::SceneDescriptor::ParticleEmitterDesc preset;
                            if (SE::SceneLoader::LoadParticlePreset(presetFiles[selected], preset))
                            {
                                int oldMax = ps.config.maxParticles;
                                SE::SceneRuntime::ApplyParticleDescToSystem(preset, ps);
                                m_scene.particleEmitterDescs[i] = preset;
                                if (!preset.texture.empty())
                                {
                                    auto toW = [](const std::string& s) { return std::wstring(s.begin(), s.end()); };
                                    ps.SetTexture(GetAssets().FindTexture(toW(preset.texture)));
                                }
                                if (oldMax != ps.config.maxParticles)
                                    ps.Init(GetRenderer().GetDevice(), GetShaders());
                            }
                        }
                    }
                }
                else
                {
                    ImGui::TextDisabled("No presets in Assets/Particles/Presets");
                }
                ImGui::Separator();

                ImGui::Text("Emission");
                ImGui::Checkbox("Enable", &ps.enabled);
                ImGui::Combo("Mode", &ps.config.emitMode, "Continuous\0Single\0");
                if (ps.config.emitMode == 0)
                    ImGui::SliderFloat("Emit Rate", &ps.config.emitRate, 1.0f, 500.0f);
                else
                {
                    ImGui::TextDisabled("Emit Rate");
                    if (ImGui::Button("Press to Fire"))
                        ps.Burst(8);
                }
                ImGui::Separator();

                ImGui::Text("Transform");
                ImGui::DragFloat3("Position", &ps.config.position.x, 0.1f, -50.0f, 50.0f);
                ImGui::Combo("Spawn Shape", &ps.config.spawnShape, "Sphere\0Box\0");
                if (ps.config.spawnShape == 1)
                    ImGui::DragFloat3("Box Extents", &ps.config.spawnBoxExtents.x, 0.1f, 0.0f, 50.0f);
                else
                    ImGui::SliderFloat("Spawn Radius", &ps.config.spawnRadius, 0.0f, 5.0f);
                ImGui::SliderFloat("Aspect Ratio", &ps.config.aspectRatio, 0.5f, 4.0f);
                ImGui::SliderFloat("Rotation (deg)", &ps.config.rotationDegrees, -180.0f, 180.0f);
                ImGui::Checkbox("Random Rotation", &ps.config.randomRotation);
                ImGui::Separator();

                ImGui::Text("Lifetime & Size");
                ImGui::SliderFloat("Life Min",     &ps.config.lifetimeMin, 0.1f, 10.0f);
                ImGui::SliderFloat("Life Max",     &ps.config.lifetimeMax, 0.1f, 10.0f);
                ImGui::SliderFloat("Size Start",   &ps.config.sizeStart,   0.01f, 2.0f);
                ImGui::SliderFloat("Size End",     &ps.config.sizeEnd,     0.0f, 2.0f);
                ImGui::Separator();

                ImGui::Text("Motion");
                ImGui::DragFloat3("Velocity Min",  &ps.config.velocityMin.x, 0.1f, -20.0f, 20.0f);
                ImGui::DragFloat3("Velocity Max",  &ps.config.velocityMax.x, 0.1f, -20.0f, 20.0f);
                ImGui::DragFloat3("Gravity",       &ps.config.gravity.x, 0.1f, -20.0f, 20.0f);
                ImGui::SliderFloat("Drag", &ps.config.drag, 0.0f, 5.0f);
                ImGui::Separator();

                ImGui::Text("Color");
                ImGui::ColorEdit4("Color Start",   &ps.config.colorStart.x);
                ImGui::ColorEdit4("Color End",     &ps.config.colorEnd.x);
                ImGui::Separator();

                ImGui::Text("Atlas");
                ImGui::InputInt("Columns", &ps.config.atlasColumns);
                ImGui::InputInt("Rows",    &ps.config.atlasRows);
                ImGui::InputInt("Frames",  &ps.config.atlasFrameCount);
                ImGui::SliderFloat("Speed", &ps.config.atlasSpeed, 0.1f, 5.0f);
                ImGui::SliderFloat("Texture Rotation (deg)", &ps.config.textureRotationDegrees, -180.0f, 180.0f);
                ImGui::Separator();

                ImGui::Text("Render");
                ImGui::Combo("Blend Mode", &ps.config.blendMode, "Additive\0Alpha Blend\0");
                ImGui::Combo("Shader Mode", &ps.config.shaderMode, "Textured\0Ember\0");
                ImGui::SliderFloat("Soft Distance", &ps.config.softDistance, 0.0f, 3.0f);
                ImGui::SliderFloat("Tracer Stretch", &ps.config.tracerStretch, 0.0f, 0.25f);
                ImGui::SliderFloat("Lighting Influence", &ps.config.lightingInfluence, 0.0f, 1.0f);
            }
            ImGui::PopID();
        }
        ImGui::End();

        // Viewport light indicators
        {
            XMMATRIX    vp = XMMatrixMultiply(view, proj);
            float       sw = (float)GetWindow().GetWidth();
            float       sh = (float)GetWindow().GetHeight();
            ImDrawList* dl = ImGui::GetBackgroundDrawList();

            auto project = [&](XMVECTOR wp, float& sx, float& sy) -> bool
            {
                XMVECTOR cp = XMVector4Transform(wp, vp);
                float    cw = XMVectorGetW(cp);
                if (cw <= 0.0f) return false;
                sx = ( XMVectorGetX(cp) / cw * 0.5f + 0.5f) * sw;
                sy = (-XMVectorGetY(cp) / cw * 0.5f + 0.5f) * sh;
                return true;
            };

            // Sun
            {
                float er = XMConvertToRadians(m_scene.lights.elevDeg);
                float ar = XMConvertToRadians(m_scene.lights.azimDeg);
                XMVECTOR dir    = XMVectorSet(cosf(er) * sinf(ar), sinf(er), cosf(er) * cosf(ar), 0.0f);
                XMVECTOR eyeV   = XMVectorSet(m_scene.camera->eye.x, m_scene.camera->eye.y, m_scene.camera->eye.z, 1.0f);
                XMVECTOR sunPos = XMVectorSetW(XMVectorAdd(eyeV, XMVectorScale(dir, 80.0f)), 1.0f);
                float sx, sy;
                if (project(sunPos, sx, sy))
                {
                    dl->AddCircleFilled({ sx, sy }, 10.0f, IM_COL32(255, 220, 50, 255));
                    for (int r = 0; r < 8; ++r)
                    {
                        float a = r * 3.14159f / 4.0f;
                        dl->AddLine({ sx + cosf(a) * 12.0f, sy + sinf(a) * 12.0f },
                                    { sx + cosf(a) * 17.0f, sy + sinf(a) * 17.0f },
                                    IM_COL32(255, 220, 50, 200), 1.5f);
                    }
                    dl->AddText({ sx + 14.0f, sy - 7.0f }, IM_COL32_WHITE, "Sun");
                }
            }

            // Point lights
            for (int i = 0; i < m_scene.lights.numLights; ++i)
            {
                const auto& pl = m_scene.lights.lights[i];
                float sx, sy;
                if (!project(XMVectorSet(pl.position.x, pl.position.y, pl.position.z, 1.0f), sx, sy))
                    continue;
                ImU32 fill = ImGui::ColorConvertFloat4ToU32({ pl.color.x, pl.color.y, pl.color.z, 1.0f });
                dl->AddCircleFilled({ sx, sy }, 7.0f, fill);
                dl->AddCircle({ sx, sy }, 8.0f, IM_COL32(255, 255, 255, 200), 0, 1.5f);
                char tag[8];
                sprintf_s(tag, "PL%d", i + 1);
                dl->AddText({ sx + 11.0f, sy - 7.0f }, IM_COL32_WHITE, tag);
            }
        }
    }

    SE::SceneRuntime m_scene;

    // Scene selection / loading UI state
    std::vector<std::string> m_sceneFiles;
    int                      m_selectedScene    = 0;
    std::string              m_initialScenePath;
    bool                     m_initialSceneLoad = false;
    bool                     m_firstFrame       = true;
    int                      m_frameCount       = 0;
};

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int)
{
    // Set working directory to project root (walk up from exe location until we find Engine/ and Assets/)
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::filesystem::path dir = std::filesystem::path(exePath).parent_path();
        for (int i = 0; i < 5; ++i)
        {
            if (std::filesystem::exists(dir / "Engine") && std::filesystem::exists(dir / "Assets"))
            {
                SetCurrentDirectoryW(dir.c_str());
                break;
            }
            dir = dir.parent_path();
        }
    }

    SE::WindowDesc desc;
    desc.title  = L"FoxEngine";
    desc.width  = 1280;
    desc.height = 720;

    // Parse --scene argument
    std::string scenePath;
    if (lpCmdLine && strlen(lpCmdLine) > 0)
    {
        std::string args(lpCmdLine);
        auto pos = args.find("--scene");
        if (pos != std::string::npos)
        {
            pos += 7; // skip "--scene"
            while (pos < args.size() && (args[pos] == ' ' || args[pos] == '=')) ++pos;
            auto end = args.find(' ', pos);
            scenePath = args.substr(pos, end - pos);
            // If just a filename, prepend Assets/Scenes/
            if (scenePath.find('/') == std::string::npos && scenePath.find('\\') == std::string::npos)
                scenePath = "Assets/Scenes/" + scenePath;
        }
    }

    TestScene scene;
    if (!scene.Initialize(desc)) return 1;
    if (!scene.Setup(scenePath)) return 1;
    scene.Run();
    scene.Shutdown();
    return 0;
}
