#include "EditorPanels.h"
#include "EditorCamera.h"

#include "Engine/Scene/SceneRuntime.h"
#include "Engine/Scene/SceneLoader.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Renderer/ShaderLibrary.h"
#include <cstdio>
#include <string>

using namespace DirectX;

namespace Editor {

namespace {

std::string GetFilename(const std::string& path)
{
    auto pos = path.find_last_of("\\/");
    return (pos != std::string::npos) ? path.substr(pos + 1) : path;
}

// Begins a panel window; returns false (window ended) when no scene is loaded.
bool BeginScenePanel(const char* name, SE::SceneRuntime& scene)
{
    ImGui::Begin(name);
    if (scene.HasScene())
        return true;
    ImGui::TextDisabled("No scene loaded");
    ImGui::End();
    return false;
}

void DrawSunSection(SE::SceneRuntime& scene)
{
    auto& lights = scene.lights;
    ImGui::SliderFloat("Elevation",    &lights.elevDeg,  -90.0f, 90.0f,  "%.1f deg");
    ImGui::SliderFloat("Azimuth",      &lights.azimDeg, -180.0f, 180.0f, "%.1f deg");
    ImGui::SliderFloat("Intensity",    &lights.lightIntensity, 0.0f, 10.0f, "%.2f");
    ImGui::ColorEdit3("Light Color",   lights.lightColor);
    ImGui::SliderFloat("IBL Intensity", &lights.iblIntensity, 0.0f, 5.0f, "%.2f");
    ImGui::SliderFloat("Diffuse IBL Saturation", &lights.diffuseIBLSaturation,
                       0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Shadow Bias", &scene.pointShadowBias, 0.001f, 0.1f, "%.4f");
}

void DrawSpotLightSection(SE::SceneRuntime& scene)
{
    auto& spot = scene.spotLight;
    ImGui::Checkbox("Enable Spot", &spot.enabled);
    if (!spot.enabled)
        return;
    if (ImGui::Button("Aim From Camera"))
    {
        XMVECTOR eye = XMLoadFloat3(&scene.camera->eye);
        XMVECTOR target = XMLoadFloat3(&scene.camera->target);
        XMVECTOR dir = XMVector3Normalize(XMVectorSubtract(target, eye));
        spot.position = scene.camera->eye;
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

void DrawPointLightsSection(SE::SceneRuntime& scene)
{
    auto& lights = scene.lights;
    ImGui::SliderInt("Active", &lights.numLights, 0, 8);
    for (int i = 0; i < lights.numLights; ++i)
    {
        ImGui::PushID(i);
        char label[24];
        sprintf_s(label, "Point Light %d", i + 1);
        if (ImGui::TreeNode(label))
        {
            ImGui::DragFloat3("Position", &lights.lights[i].position.x, 1.0f, -1000.0f, 1000.0f);
            if (i < static_cast<int>(scene.pointLightDescs.size()))
            {
                // Scene-driven light: edit the base color; the per-frame
                // flicker factor is applied on top of it.
                auto& source = scene.pointLightDescs[i];
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
                ImGui::Checkbox("Cast Shadow", &scene.lightCastsShadow[i]);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void DrawLightingDebugSection(SE::SceneRuntime& scene)
{
    ImGui::Checkbox("Show Shadow Factor", &scene.debugShadow);
    int dlm = (int)scene.lights.debugLightMode;
    ImGui::RadioButton("Normal",    &dlm, 0); ImGui::SameLine();
    ImGui::RadioButton("Force Lit", &dlm, 1); ImGui::SameLine();
    ImGui::RadioButton("NdotL",     &dlm, 2); ImGui::SameLine();
    ImGui::RadioButton("Roughness", &dlm, 3);
    scene.lights.debugLightMode = (float)dlm;
}

void DrawFsrSection(SE::SceneRuntime& scene, uint32_t outWidth, uint32_t outHeight)
{
    const char* modes[] = { "Quality (1.5x)", "Balanced (1.7x)",
                            "Performance (2.0x)", "Native AA (1.0x)" };
    ImGui::Combo("Mode", &scene.fsrQuality, modes, IM_ARRAYSIZE(modes));
    ImGui::Checkbox("Sharpen", &scene.fsrSharpen);
    if (scene.fsrSharpen)
        ImGui::SliderFloat("Sharpness", &scene.fsrSharpness, 0.0f, 1.0f, "%.2f");
    if (ImGui::SliderFloat("Velocity Factor", &scene.fsrVelocityFactor, 0.0f, 1.0f, "%.2f"))
        scene.fsr.SetVelocityFactor(scene.fsrVelocityFactor);
    ImGui::Text("Render: %ux%u -> %ux%u",
                scene.fsr.RenderWidth(), scene.fsr.RenderHeight(),
                outWidth, outHeight);
}

void DrawTaaSection(SE::SceneRuntime& scene)
{
    ImGui::SliderFloat("History Blend", &scene.taa.blendFactor, 0.5f, 0.98f, "%.2f");
    ImGui::SliderFloat("Clamp Width", &scene.taa.clampGamma, 0.5f, 2.5f, "%.2f");
    if (ImGui::Button("Reset History"))
        scene.taa.ResetHistory();
}

// FSR 3.1 and the engine TAA are mutually exclusive temporal AA paths.
void DrawTemporalAASection(SE::SceneRuntime& scene, uint32_t outWidth, uint32_t outHeight)
{
    int method = scene.fsrEnabled ? 1 : (scene.taa.enabled ? 2 : 0);
    const char* methods[] = { "Off", "FSR 3.1", "Engine TAA" };
    if (ImGui::Combo("Method", &method, methods, IM_ARRAYSIZE(methods)))
    {
        scene.fsrEnabled  = method == 1;
        scene.taa.enabled = method == 2;
        scene.taa.ResetHistory();
    }
    if (scene.fsrEnabled)
        DrawFsrSection(scene, outWidth, outHeight);
    else if (scene.taa.enabled)
        DrawTaaSection(scene);
}

void DrawPresetCombo(const std::vector<std::string>& presetFiles, int& selected)
{
    std::string previewName = (selected >= 0 && selected < (int)presetFiles.size())
        ? GetFilename(presetFiles[selected])
        : "Select preset...";
    if (!ImGui::BeginCombo("Preset File", previewName.c_str()))
        return;
    for (int p = 0; p < (int)presetFiles.size(); ++p)
    {
        bool isSelected = (p == selected);
        if (ImGui::Selectable(GetFilename(presetFiles[p]).c_str(), isSelected))
            selected = p;
        if (isSelected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
}

void LoadPresetIntoEmitter(SE::SceneRuntime& scene, int i, SE::ParticleSystem& ps,
                           const std::string& presetPath, SE::AssetManager& assets,
                           ID3D11Device* device, SE::ShaderLibrary& shaders)
{
    SE::SceneDescriptor::ParticleEmitterDesc preset;
    if (!SE::SceneLoader::LoadParticlePreset(presetPath, preset))
        return;
    int oldMax = ps.config.maxParticles;
    SE::SceneRuntime::ApplyParticleDescToSystem(preset, ps);
    scene.particleEmitterDescs[i] = preset;
    if (!preset.texture.empty())
    {
        std::wstring wtex(preset.texture.begin(), preset.texture.end());
        ps.SetTexture(assets.FindTexture(wtex));
    }
    if (oldMax != ps.config.maxParticles)
        ps.Init(device, shaders);
}

void DrawEmitterPreset(SE::SceneRuntime& scene, int i, SE::ParticleSystem& ps,
                       SE::AssetManager& assets, ID3D11Device* device,
                       SE::ShaderLibrary& shaders)
{
    ImGui::Text("Preset");
    if (scene.particlePresetFiles.empty())
    {
        ImGui::TextDisabled("No presets in Assets/Particles/Presets");
        return;
    }
    int& selected = scene.selectedParticlePreset[i];
    DrawPresetCombo(scene.particlePresetFiles, selected);
    if (selected < 0 || selected >= (int)scene.particlePresetFiles.size())
        return;
    if (ImGui::Button("Load Preset"))
        LoadPresetIntoEmitter(scene, i, ps, scene.particlePresetFiles[selected],
                              assets, device, shaders);
}

void DrawEmitterConfig(SE::ParticleSystem& ps)
{
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

} // namespace

void DrawCameraPanel(SE::SceneRuntime& scene, EditorCamera& cam)
{
    if (!BeginScenePanel("Camera", scene))
        return;
    ImGui::Text("RMB fly  Alt+LMB orbit  MMB pan");
    ImGui::SliderFloat("Fly Speed",   &cam.moveSpeed,   1.0f, 200.0f);
    ImGui::SliderFloat("Sensitivity", &cam.sensitivity, 0.05f, 0.5f);
    ImGui::SliderFloat("FOV",    &scene.camera->fovDeg, 20.0f, 120.0f, "%.0f deg");
    ImGui::SliderFloat("Near Z", &scene.camera->nearZ, 0.001f, 10.0f, "%.3f");
    ImGui::SliderFloat("Far Z",  &scene.camera->farZ, 100.0f, 20000.0f, "%.0f");
    ImGui::Text("Eye (%.1f, %.1f, %.1f)",
        scene.camera->eye.x, scene.camera->eye.y, scene.camera->eye.z);
    ImGui::End();
}

void DrawMaterialPanel(SE::SceneRuntime& scene)
{
    if (!BeginScenePanel("Material", scene))
        return;
    ImGui::ColorEdit3("Albedo Tint",      scene.matTint);
    ImGui::SliderFloat("Roughness Scale", &scene.roughnessScale, 0.0f, 2.0f);
    ImGui::SliderFloat("Metallic",        &scene.metallic,       0.0f, 1.0f);
    ImGui::End();
}

void DrawPhysicsPanel(SE::SceneRuntime& scene)
{
    if (!BeginScenePanel("Physics", scene))
        return;
    ImGui::Checkbox("Show Colliders", &scene.showColliders);
    ImGui::SameLine();
    ImGui::Checkbox("Raycast", &scene.castRay);
    if (ImGui::Checkbox("Gravity", &scene.gravityEnabled))
    {
        scene.ballRigidBody->useGravity = scene.gravityEnabled;
        scene.cc.gravityEnabled         = scene.gravityEnabled;
    }
    ImGui::Separator();
    ImGui::Text("Ball");
    ImGui::DragFloat3("Spawn pos",    &scene.ballSpawn.x,                1.0f);
    ImGui::SliderFloat("Radius",      &scene.ballRadius,                 0.1f, 10.0f);
    ImGui::SliderFloat("Mass",        &scene.ballRigidBody->mass,        0.1f, 10.0f);
    ImGui::SliderFloat("Restitution", &scene.ballRigidBody->restitution, 0.0f, 1.0f);
    ImGui::SliderFloat("Friction",    &scene.ballRigidBody->friction,    0.0f, 1.0f);
    if (ImGui::SliderFloat("Floor Y", &scene.floorY, -50.0f, 50.0f))
    {
        scene.physicsWorld.Clear();
        scene.physicsWorld.AddSphere(scene.ballTransform, scene.ballRigidBody,
                                     scene.ballRadius);
        scene.obbFloor = SE::OBB::FromAABB({ -30.0f, scene.floorY - 0.5f, -30.0f },
                                           {  30.0f, scene.floorY,        30.0f });
        scene.physicsWorld.AddStaticOBB(scene.obbFloor, 0.6f, 0.4f);
    }
    ImGui::Text("pos (%.1f, %.1f, %.1f)",
        scene.ballTransform->position.x, scene.ballTransform->position.y,
        scene.ballTransform->position.z);
    ImGui::Text("vel (%.2f, %.2f, %.2f)",
        scene.ballRigidBody->velocity.x, scene.ballRigidBody->velocity.y,
        scene.ballRigidBody->velocity.z);
    if (ImGui::Button("Reset"))     scene.ResetBall();
    ImGui::SameLine();
    if (ImGui::Button("Launch up")) scene.ballRigidBody->AddImpulse({ 0.0f, 20.0f, 0.0f });
    if (scene.castRay)
    {
        ImGui::Separator();
        ImGui::Text("Raycast");
        if (scene.rayHitValid)
        {
            const auto& hit = scene.rayHit;
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
}

void DrawLightingPanel(SE::SceneRuntime& scene)
{
    if (!BeginScenePanel("Lighting", scene))
        return;
    if (ImGui::CollapsingHeader("Sun", ImGuiTreeNodeFlags_DefaultOpen))
        DrawSunSection(scene);
    if (ImGui::CollapsingHeader("Spot Light"))
        DrawSpotLightSection(scene);
    if (ImGui::CollapsingHeader("Point Lights"))
        DrawPointLightsSection(scene);
    if (ImGui::CollapsingHeader("Debug"))
        DrawLightingDebugSection(scene);
    ImGui::End();
}

namespace {

void DrawToneMapSection(SE::SceneRuntime& scene)
{
    ImGui::SliderFloat("Exposure", &scene.toneMap.exposure, 0.01f, 10.0f, "%.2f");
    int op = static_cast<int>(scene.toneMap.op);
    ImGui::RadioButton("Reinhard", &op, 0); ImGui::SameLine();
    ImGui::RadioButton("ACES",     &op, 1);
    scene.toneMap.op = static_cast<SE::ToneMap::Operator>(op);
    ImGui::Checkbox("Gamma Correct", &scene.toneMap.gammaCorrect);
}

void DrawBloomSection(SE::SceneRuntime& scene)
{
    auto& bloom = scene.bloom;
    ImGui::Checkbox("Enable Bloom", &bloom.enabled);
    if (!bloom.enabled)
        return;
    ImGui::SliderFloat("Threshold", &bloom.threshold, 0.0f, 4.0f,  "%.2f");
    ImGui::SliderFloat("Strength",  &bloom.intensity, 0.0f, 0.5f,  "%.3f");
    ImGui::SliderFloat("Scatter",   &bloom.scatter,   0.0f, 1.0f,  "%.2f");
}

void DrawSsaoSection(SE::SceneRuntime& scene)
{
    auto& ssao = scene.ssao;
    ImGui::Checkbox("Enable SSAO", &ssao.enabled);
    if (!ssao.enabled)
        return;
    ImGui::SliderFloat("AO Radius", &ssao.radius, 0.01f, 5.0f, "%.2f");
    ImGui::SliderFloat("AO Bias",   &ssao.bias,   0.001f, 0.1f, "%.3f");
    ImGui::SliderFloat("AO Power",  &ssao.power,  0.5f, 8.0f,   "%.1f");
}

void DrawFogSection(SE::SceneRuntime& scene)
{
    auto& volFog = scene.volFog;
    ImGui::Checkbox("Enable Fog", &volFog.enabled);
    if (!volFog.enabled)
        return;
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

void DrawFxaaSection(SE::SceneRuntime& scene)
{
    auto& fxaa = scene.fxaa;
    ImGui::Checkbox("Enable FXAA", &fxaa.enabled);
    if (!fxaa.enabled)
        return;
    ImGui::SliderFloat("Subpix Quality", &fxaa.subpixQuality, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Edge Threshold", &fxaa.edgeThreshold, 0.063f, 0.333f, "%.3f");
    ImGui::SliderFloat("Edge Min",       &fxaa.edgeThresholdMin, 0.0f, 0.1f, "%.4f");
}

} // namespace

void DrawPostProcessPanel(SE::SceneRuntime& scene, uint32_t outWidth, uint32_t outHeight)
{
    if (!BeginScenePanel("Post Processing", scene))
        return;
    if (ImGui::CollapsingHeader("Tone Mapping", ImGuiTreeNodeFlags_DefaultOpen))
        DrawToneMapSection(scene);
    if (ImGui::CollapsingHeader("Bloom"))
        DrawBloomSection(scene);
    if (ImGui::CollapsingHeader("SSAO"))
        DrawSsaoSection(scene);
    if (ImGui::CollapsingHeader("Volumetric Fog"))
        DrawFogSection(scene);
    if (ImGui::CollapsingHeader("Temporal AA / Upscaling"))
        DrawTemporalAASection(scene, outWidth, outHeight);
    if (ImGui::CollapsingHeader("FXAA"))
        DrawFxaaSection(scene);
    ImGui::End();
}

void DrawParticlesPanel(SE::SceneRuntime& scene, SE::AssetManager& assets,
                        ID3D11Device* device, SE::ShaderLibrary& shaders)
{
    if (!BeginScenePanel("Particles", scene))
        return;
    ImGui::Text("Emitters: %d", (int)scene.particleSystems.size());
    ImGui::SameLine();
    if (ImGui::Button("Rescan Presets"))
        scene.RefreshParticlePresets();
    if (ImGui::Button("Fire All Single Emitters"))
    {
        for (auto& emitter : scene.particleSystems)
        {
            if (emitter->config.emitMode == 1)
                emitter->Burst(8);
        }
    }
    ImGui::Separator();
    for (int i = 0; i < (int)scene.particleSystems.size(); ++i)
    {
        auto& ps = *scene.particleSystems[i];
        ImGui::PushID(i);
        char label[32];
        sprintf_s(label, "Emitter %d", i);
        if (ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawEmitterPreset(scene, i, ps, assets, device, shaders);
            ImGui::Separator();
            DrawEmitterConfig(ps);
        }
        ImGui::PopID();
    }
    ImGui::End();
}

void DrawLightIndicators(SE::SceneRuntime& scene, ImVec2 imageMin, ImVec2 imageMax)
{
    if (!scene.HasScene())
        return;

    const XMMATRIX vp = XMMatrixMultiply(scene.GetView(), scene.GetProj());
    const float sw = imageMax.x - imageMin.x;
    const float sh = imageMax.y - imageMin.y;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(imageMin, imageMax, true);

    auto project = [&](XMVECTOR wp, float& sx, float& sy) -> bool
    {
        XMVECTOR cp = XMVector4Transform(wp, vp);
        float    cw = XMVectorGetW(cp);
        if (cw <= 0.0f) return false;
        sx = imageMin.x + ( XMVectorGetX(cp) / cw * 0.5f + 0.5f) * sw;
        sy = imageMin.y + (-XMVectorGetY(cp) / cw * 0.5f + 0.5f) * sh;
        return true;
    };

    // Sun
    {
        float er = XMConvertToRadians(scene.lights.elevDeg);
        float ar = XMConvertToRadians(scene.lights.azimDeg);
        XMVECTOR dir    = XMVectorSet(cosf(er) * sinf(ar), sinf(er), cosf(er) * cosf(ar), 0.0f);
        XMVECTOR eyeV   = XMVectorSet(scene.camera->eye.x, scene.camera->eye.y, scene.camera->eye.z, 1.0f);
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
    for (int i = 0; i < scene.lights.numLights; ++i)
    {
        const auto& pl = scene.lights.lights[i];
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

    dl->PopClipRect();
}

} // namespace Editor
