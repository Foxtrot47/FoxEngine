#include "EditorPanels.h"

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

} // namespace

bool BeginScenePanel(const char* name, SE::SceneRuntime& scene)
{
    ImGui::Begin(name);
    if (scene.HasScene())
        return true;
    ImGui::TextDisabled("No scene loaded");
    ImGui::End();
    return false;
}

void DrawSunProps(SE::SceneRuntime& scene)
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

void DrawSpotLightProps(SE::SceneRuntime& scene)
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

void DrawPointLightProps(SE::SceneRuntime& scene, int index)
{
    auto& lights = scene.lights;
    ImGui::DragFloat3("Position", &lights.lights[index].position.x, 1.0f, -1000.0f, 1000.0f);
    if (index < static_cast<int>(scene.pointLightDescs.size()))
    {
        // Scene-driven light: edit the base color; the per-frame
        // flicker factor is applied on top of it.
        auto& source = scene.pointLightDescs[index];
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
        ImGui::ColorEdit3("Color", &lights.lights[index].color.x,
                          ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
    }
    ImGui::SliderFloat("Radius",  &lights.lights[index].radius, 0.5f, 500.0f);
    if (index < 2)
        ImGui::Checkbox("Cast Shadow", &scene.lightCastsShadow[index]);
}

namespace {

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
        if (ImGui::Button("Rescan"))
            scene.RefreshParticlePresets();
        return;
    }
    int& selected = scene.selectedParticlePreset[i];
    DrawPresetCombo(scene.particlePresetFiles, selected);
    if (selected < 0 || selected >= (int)scene.particlePresetFiles.size())
        return;
    if (ImGui::Button("Load Preset"))
        LoadPresetIntoEmitter(scene, i, ps, scene.particlePresetFiles[selected],
                              assets, device, shaders);
    ImGui::SameLine();
    if (ImGui::Button("Rescan"))
        scene.RefreshParticlePresets();
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

void DrawEmitterProps(SE::SceneRuntime& scene, int index, SE::AssetManager& assets,
                      ID3D11Device* device, SE::ShaderLibrary& shaders)
{
    if (index < 0 || index >= (int)scene.particleSystems.size())
        return;
    auto& ps = *scene.particleSystems[index];
    DrawEmitterPreset(scene, index, ps, assets, device, shaders);
    ImGui::Separator();
    DrawEmitterConfig(ps);
}

namespace {

// Shared context for viewport overlay drawing: projection into the image
// rect plus click hit-testing against indicator circles.
struct IndicatorContext
{
    ImDrawList* dl;
    ImVec2      imageMin;
    float       sw, sh;
    XMMATRIX    viewProj;
    ImVec2      mouse;
    bool        tryClick;

    bool Project(XMVECTOR wp, float& sx, float& sy) const
    {
        XMVECTOR cp = XMVector4Transform(wp, viewProj);
        float    cw = XMVectorGetW(cp);
        if (cw <= 0.0f) return false;
        sx = imageMin.x + ( XMVectorGetX(cp) / cw * 0.5f + 0.5f) * sw;
        sy = imageMin.y + (-XMVectorGetY(cp) / cw * 0.5f + 0.5f) * sh;
        return true;
    }

    bool HitTest(float sx, float sy) const
    {
        const float dx = mouse.x - sx, dy = mouse.y - sy;
        return tryClick && (dx * dx + dy * dy) <= 12.0f * 12.0f;
    }
};

bool DrawSunIndicator(SE::SceneRuntime& scene, Selection& sel,
                      const IndicatorContext& ctx)
{
    float er = XMConvertToRadians(scene.lights.elevDeg);
    float ar = XMConvertToRadians(scene.lights.azimDeg);
    XMVECTOR dir    = XMVectorSet(cosf(er) * sinf(ar), sinf(er), cosf(er) * cosf(ar), 0.0f);
    XMVECTOR eyeV   = XMVectorSet(scene.camera->eye.x, scene.camera->eye.y, scene.camera->eye.z, 1.0f);
    XMVECTOR sunPos = XMVectorSetW(XMVectorAdd(eyeV, XMVectorScale(dir, 80.0f)), 1.0f);
    float sx, sy;
    if (!ctx.Project(sunPos, sx, sy))
        return false;

    const bool clicked = ctx.HitTest(sx, sy);
    if (clicked)
        sel.Set(Selection::Kind::Sun);
    if (sel.Is(Selection::Kind::Sun))
        ctx.dl->AddCircle({ sx, sy }, 20.0f, IM_COL32(0, 122, 204, 255), 0, 2.0f);
    ctx.dl->AddCircleFilled({ sx, sy }, 10.0f, IM_COL32(255, 220, 50, 255));
    for (int r = 0; r < 8; ++r)
    {
        float a = r * 3.14159f / 4.0f;
        ctx.dl->AddLine({ sx + cosf(a) * 12.0f, sy + sinf(a) * 12.0f },
                        { sx + cosf(a) * 17.0f, sy + sinf(a) * 17.0f },
                        IM_COL32(255, 220, 50, 200), 1.5f);
    }
    ctx.dl->AddText({ sx + 14.0f, sy - 7.0f }, IM_COL32_WHITE, "Sun");
    return clicked;
}

bool DrawPointLightIndicators(SE::SceneRuntime& scene, Selection& sel,
                              const IndicatorContext& ctx)
{
    bool clickConsumed = false;
    for (int i = 0; i < scene.lights.numLights; ++i)
    {
        const auto& pl = scene.lights.lights[i];
        float sx, sy;
        if (!ctx.Project(XMVectorSet(pl.position.x, pl.position.y, pl.position.z, 1.0f), sx, sy))
            continue;
        if (ctx.HitTest(sx, sy))
        {
            sel.Set(Selection::Kind::PointLight, i);
            clickConsumed = true;
        }
        ImU32 fill = ImGui::ColorConvertFloat4ToU32({ pl.color.x, pl.color.y, pl.color.z, 1.0f });
        if (sel.Is(Selection::Kind::PointLight, i))
            ctx.dl->AddCircle({ sx, sy }, 13.0f, IM_COL32(0, 122, 204, 255), 0, 2.0f);
        ctx.dl->AddCircleFilled({ sx, sy }, 7.0f, fill);
        ctx.dl->AddCircle({ sx, sy }, 8.0f, IM_COL32(255, 255, 255, 200), 0, 1.5f);
        char tag[8];
        sprintf_s(tag, "PL%d", i + 1);
        ctx.dl->AddText({ sx + 11.0f, sy - 7.0f }, IM_COL32_WHITE, tag);
    }
    return clickConsumed;
}

} // namespace

bool DrawLightIndicators(SE::SceneRuntime& scene, ImVec2 imageMin, ImVec2 imageMax,
                         Selection& sel)
{
    if (!scene.HasScene())
        return false;

    IndicatorContext ctx;
    ctx.dl       = ImGui::GetWindowDrawList();
    ctx.imageMin = imageMin;
    ctx.sw       = imageMax.x - imageMin.x;
    ctx.sh       = imageMax.y - imageMin.y;
    ctx.viewProj = XMMatrixMultiply(scene.GetView(), scene.GetProj());
    ctx.mouse    = ImGui::GetMousePos();
    ctx.tryClick = ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                   !ImGui::GetIO().KeyAlt;

    ctx.dl->PushClipRect(imageMin, imageMax, true);
    bool clickConsumed = DrawSunIndicator(scene, sel, ctx);
    clickConsumed |= DrawPointLightIndicators(scene, sel, ctx);
    ctx.dl->PopClipRect();
    return clickConsumed;
}

} // namespace Editor
