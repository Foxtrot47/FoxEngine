#include "EditorSettings.h"

#include "Engine/Scene/SceneRuntime.h"
#include <imgui.h>

namespace Editor {

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

void DrawColorGradingSection(SE::SceneRuntime& scene)
{
    auto& tm = scene.toneMap;
    ImGui::SliderFloat("Temperature", &tm.temperature, -1.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Tint",        &tm.tint,        -1.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Saturation",  &tm.saturation,   0.0f, 2.0f, "%.2f");
    ImGui::SliderFloat("Contrast",    &tm.contrast,     0.5f, 1.5f, "%.2f");
    ImGui::SliderFloat("Vignette",    &tm.vignette,     0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Vig Feather", &tm.vignetteFeather, 0.05f, 1.0f, "%.2f");
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

void DrawWorldMaterialSection(SE::SceneRuntime& scene)
{
    ImGui::ColorEdit3("Albedo Tint",      scene.matTint);
    ImGui::SliderFloat("Roughness Scale", &scene.roughnessScale, 0.0f, 2.0f);
    ImGui::SliderFloat("Metallic",        &scene.metallic,       0.0f, 1.0f);
}

void DrawWorldLightingSection(SE::SceneRuntime& scene)
{
    ImGui::SliderInt("Active Point Lights", &scene.lights.numLights, 0, 8);
    ImGui::Checkbox("Show Shadow Factor", &scene.debugShadow);
    int dlm = (int)scene.lights.debugLightMode;
    ImGui::RadioButton("Normal",    &dlm, 0); ImGui::SameLine();
    ImGui::RadioButton("Force Lit", &dlm, 1); ImGui::SameLine();
    ImGui::RadioButton("NdotL",     &dlm, 2); ImGui::SameLine();
    ImGui::RadioButton("Roughness", &dlm, 3);
    scene.lights.debugLightMode = (float)dlm;
}

void DrawRaycastReadout(SE::SceneRuntime& scene)
{
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

void DrawWorldPhysicsSection(SE::SceneRuntime& scene)
{
    if (ImGui::Checkbox("Gravity", &scene.gravityEnabled))
    {
        scene.ballRigidBody->useGravity = scene.gravityEnabled;
        scene.cc.gravityEnabled         = scene.gravityEnabled;
    }
    if (ImGui::SliderFloat("Floor Y", &scene.floorY, -50.0f, 50.0f))
    {
        scene.physicsWorld.Clear();
        scene.physicsWorld.AddSphere(scene.ballTransform, scene.ballRigidBody,
                                     scene.ballRadius);
        scene.obbFloor = SE::OBB::FromAABB({ -30.0f, scene.floorY - 0.5f, -30.0f },
                                           {  30.0f, scene.floorY,        30.0f });
        scene.physicsWorld.AddStaticOBB(scene.obbFloor, 0.6f, 0.4f);
    }
    ImGui::Checkbox("Show Colliders", &scene.showColliders);
    ImGui::SameLine();
    ImGui::Checkbox("Raycast", &scene.castRay);
    if (scene.castRay)
        DrawRaycastReadout(scene);
}

} // namespace

void DrawWorldProps(SE::SceneRuntime& scene, uint32_t outWidth, uint32_t outHeight,
                    const ImGuiTextFilter& filter)
{
    ImGui::Text("%s", scene.currentDesc.name.c_str());
    ImGui::TextDisabled("World Settings");
    ImGui::Separator();
    struct Section { const char* name; const char* keywords; void (*draw)(SE::SceneRuntime&); };
    static const Section sections[] = {
        { "Materials", "Materials Tint Roughness Metallic", DrawWorldMaterialSection },
        { "Lighting", "Lighting Sun IBL Shadow Debug", DrawWorldLightingSection },
        { "Physics", "Physics Gravity Floor Colliders Raycast", DrawWorldPhysicsSection },
        { "Tone Mapping", "Tone Mapping Exposure Gamma", DrawToneMapSection },
        { "Color Grading", "Color Grading Temperature Tint Saturation Contrast Vignette", DrawColorGradingSection },
        { "Bloom", "Bloom Intensity Threshold", DrawBloomSection },
        { "SSAO", "SSAO Ambient Occlusion Radius Bias Power", DrawSsaoSection },
        { "Volumetric Fog", "Volumetric Fog Density Height Noise Distance Scattering", DrawFogSection },
        { "FXAA", "FXAA Anti Aliasing", DrawFxaaSection }
    };
    for (const auto& section : sections)
    {
        if (!filter.PassFilter(section.keywords)) continue;
        if (filter.IsActive()) ImGui::SetNextItemOpen(true);
        if (ImGui::CollapsingHeader(section.name)) section.draw(scene);
    }
    if (filter.PassFilter("Temporal AA Upscaling FSR TAA Sharpening"))
    {
        if (filter.IsActive()) ImGui::SetNextItemOpen(true);
        if (ImGui::CollapsingHeader("Temporal AA / Upscaling"))
            DrawTemporalAASection(scene, outWidth, outHeight);
    }
}

} // namespace Editor
