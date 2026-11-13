#include "EditorPropertyUndo.h"
#include "EditorUndo.h"

#include "Engine/Scene/SceneRuntime.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>

namespace Editor {

using Kind = Selection::Kind;

// Everything the Details panel can edit for one entity. Only the members
// matching sel.kind are captured.
struct EntityState
{
    Selection sel;

    // Camera
    float fovDeg = 0.0f, nearZ = 0.0f, farZ = 0.0f;

    // Camera / Sun script components (the other kinds keep them in their defs)
    std::vector<SE::LevelScriptDefinition> scripts;

    // Sun
    float elevDeg = 0.0f, azimDeg = 0.0f, sunIntensity = 0.0f;
    float sunColor[3] = {};
    float iblIntensity = 0.0f, diffuseIBLSaturation = 0.0f, pointShadowBias = 0.0f;

    // Spot light
    bool              spotEnabled = false;
    DirectX::XMFLOAT3 spotPosition = {}, spotDirection = {}, spotColor = {};
    float spotRange = 0.0f, spotInner = 0.0f, spotOuter = 0.0f;
    float spotIntensity = 0.0f, spotVolumetric = 0.0f, spotShadowBias = 0.0f;

    // Point light
    SE::SceneRuntime::DetachedPointLight point;

    // Mesh
    SE::SceneDescriptor::MeshEntry meshDef;
    DirectX::XMFLOAT3 meshPosition = {}, meshEuler = {}, meshScale = {};

    // Object
    SE::SceneDescriptor::SceneObject objectDef;

    // Emitter
    SE::ParticleEmitterConfig            emitterConfig;
    bool                                 emitterEnabled = true;
    SE::SceneDescriptor::ParticleEmitterDesc emitterDesc;
    SE::AssetHandle<SE::Texture2D>       emitterTexture;
    int                                  emitterPreset = -1;

    // Empty
    SE::SceneDescriptor::EmptyEntity emptyDef;
    DirectX::XMFLOAT3 emptyPosition = {}, emptyEuler = {}, emptyScale = {};
};

namespace {

const char* EditLabel(Kind kind)
{
    switch (kind)
    {
    case Kind::Camera:     return "Edit Camera";
    case Kind::Sun:        return "Edit Sun";
    case Kind::SpotLight:  return "Edit Spot Light";
    case Kind::PointLight: return "Edit Point Light";
    case Kind::Mesh:       return "Edit Mesh";
    case Kind::Object:     return "Edit Object";
    case Kind::Emitter:    return "Edit Emitter";
    case Kind::Empty:      return "Edit Empty";
    default:               return "Edit";
    }
}

// Returns nullptr when the selection has no tracked state (World, None,
// stale index).
std::shared_ptr<const EntityState> Capture(SE::SceneRuntime& scene, const Selection& sel)
{
    auto s = std::make_shared<EntityState>();
    s->sel = sel;
    const int i = sel.index;
    switch (sel.kind)
    {
    case Kind::Camera:
        if (!scene.camera) return nullptr;
        s->fovDeg = scene.camera->fovDeg;
        s->nearZ  = scene.camera->nearZ;
        s->farZ   = scene.camera->farZ;
        s->scripts = scene.currentDesc.camera.scripts;
        return s;
    case Kind::Sun:
    {
        const auto& l = scene.lights;
        s->elevDeg      = l.elevDeg;
        s->azimDeg      = l.azimDeg;
        s->sunIntensity = l.lightIntensity;
        std::copy(std::begin(l.lightColor), std::end(l.lightColor), s->sunColor);
        s->iblIntensity         = l.iblIntensity;
        s->diffuseIBLSaturation = l.diffuseIBLSaturation;
        s->pointShadowBias      = scene.pointShadowBias;
        s->scripts              = scene.currentDesc.sun.scripts;
        return s;
    }
    case Kind::SpotLight:
    {
        const auto& sp = scene.spotLight;
        s->spotEnabled    = sp.enabled;
        s->spotPosition   = sp.position;
        s->spotDirection  = sp.direction;
        s->spotColor      = sp.color;
        s->spotRange      = sp.range;
        s->spotInner      = sp.innerAngle;
        s->spotOuter      = sp.outerAngle;
        s->spotIntensity  = sp.intensity;
        s->spotVolumetric = sp.volumetricIntensity;
        s->spotShadowBias = sp.shadowBias;
        return s;
    }
    case Kind::PointLight:
        if (i < 0 || i >= scene.lights.numLights) return nullptr;
        s->point.light      = scene.lights.lights[i];
        s->point.castShadow = scene.lightCastsShadow[i];
        if (i < (int)scene.pointLightDescs.size())
            s->point.desc = scene.pointLightDescs[i];
        return s;
    case Kind::Mesh:
    {
        if (i < 0 || i >= (int)scene.loadedMeshes.size()) return nullptr;
        const auto& m = scene.loadedMeshes[i];
        if (!m.transform) return nullptr;
        s->meshDef      = m.def;
        s->meshPosition = m.transform->position;
        s->meshEuler    = m.transform->eulerDeg;
        s->meshScale    = m.transform->scale;
        return s;
    }
    case Kind::Object:
        if (i < 0 || i >= (int)scene.sceneObjects.size()) return nullptr;
        s->objectDef = scene.sceneObjects[i].def;
        return s;
    case Kind::Emitter:
    {
        if (i < 0 || i >= (int)scene.particleSystems.size()) return nullptr;
        const auto& ps = *scene.particleSystems[i];
        s->emitterConfig  = ps.config;
        s->emitterEnabled = ps.enabled;
        s->emitterTexture = ps.GetTexture();
        if (i < (int)scene.particleEmitterDescs.size())
            s->emitterDesc = scene.particleEmitterDescs[i];
        if (i < (int)scene.selectedParticlePreset.size())
            s->emitterPreset = scene.selectedParticlePreset[i];
        return s;
    }
    case Kind::Empty:
    {
        if (i < 0 || i >= (int)scene.emptyEntities.size()) return nullptr;
        const auto& e = scene.emptyEntities[i];
        if (!e.transform) return nullptr;
        s->emptyDef      = e.def;
        s->emptyPosition = e.transform->position;
        s->emptyEuler    = e.transform->eulerDeg;
        s->emptyScale    = e.transform->scale;
        return s;
    }
    default:
        return nullptr;
    }
}

void Apply(SE::SceneRuntime& scene, const EntityState& s, ID3D11Device* device,
           SE::ShaderLibrary& shaders)
{
    const int i = s.sel.index;
    switch (s.sel.kind)
    {
    case Kind::Camera:
        if (!scene.camera) return;
        scene.camera->fovDeg = s.fovDeg;
        scene.camera->nearZ  = s.nearZ;
        scene.camera->farZ   = s.farZ;
        scene.currentDesc.camera.scripts = s.scripts;
        return;
    case Kind::Sun:
    {
        auto& l = scene.lights;
        l.elevDeg        = s.elevDeg;
        l.azimDeg        = s.azimDeg;
        l.lightIntensity = s.sunIntensity;
        std::copy(std::begin(s.sunColor), std::end(s.sunColor), l.lightColor);
        l.iblIntensity         = s.iblIntensity;
        l.diffuseIBLSaturation = s.diffuseIBLSaturation;
        scene.pointShadowBias  = s.pointShadowBias;
        scene.currentDesc.sun.scripts = s.scripts;
        return;
    }
    case Kind::SpotLight:
    {
        auto& sp = scene.spotLight;
        sp.enabled             = s.spotEnabled;
        sp.position            = s.spotPosition;
        sp.direction           = s.spotDirection;
        sp.color               = s.spotColor;
        sp.range               = s.spotRange;
        sp.innerAngle          = s.spotInner;
        sp.outerAngle          = s.spotOuter;
        sp.intensity           = s.spotIntensity;
        sp.volumetricIntensity = s.spotVolumetric;
        sp.shadowBias          = s.spotShadowBias;
        return;
    }
    case Kind::PointLight:
        if (i < 0 || i >= scene.lights.numLights) return;
        scene.lights.lights[i]  = s.point.light;
        scene.lightCastsShadow[i] = s.point.castShadow;
        if (i < (int)scene.pointLightDescs.size())
            scene.pointLightDescs[i] = s.point.desc;
        return;
    case Kind::Mesh:
    {
        if (i < 0 || i >= (int)scene.loadedMeshes.size()) return;
        auto& m = scene.loadedMeshes[i];
        if (!m.transform) return;
        m.def                 = s.meshDef;
        m.transform->position = s.meshPosition;
        m.transform->eulerDeg = s.meshEuler;
        m.transform->scale    = s.meshScale;
        return;
    }
    case Kind::Object:
        if (i < 0 || i >= (int)scene.sceneObjects.size()) return;
        scene.sceneObjects[i].def = s.objectDef;
        return;
    case Kind::Emitter:
    {
        if (i < 0 || i >= (int)scene.particleSystems.size()) return;
        auto& ps = *scene.particleSystems[i];
        const int oldMax = ps.config.maxParticles;
        ps.config  = s.emitterConfig;
        ps.enabled = s.emitterEnabled;
        ps.SetTexture(s.emitterTexture);
        // Preset loads can change the pool size; mirror their re-init.
        if (oldMax != ps.config.maxParticles)
            ps.Init(device, shaders);
        if (i < (int)scene.particleEmitterDescs.size())
            scene.particleEmitterDescs[i] = s.emitterDesc;
        if (i < (int)scene.selectedParticlePreset.size())
            scene.selectedParticlePreset[i] = s.emitterPreset;
        return;
    }
    case Kind::Empty:
    {
        if (i < 0 || i >= (int)scene.emptyEntities.size()) return;
        auto& e = scene.emptyEntities[i];
        if (!e.transform) return;
        e.def                 = s.emptyDef;
        e.transform->position = s.emptyPosition;
        e.transform->eulerDeg = s.emptyEuler;
        e.transform->scale    = s.emptyScale;
        return;
    }
    default:
        return;
    }
}

} // namespace

void PropertyUndoTracker::Begin(SE::SceneRuntime& scene, const Selection& sel)
{
    m_editedBefore = ImGui::GetCurrentContext()->ActiveIdHasBeenEditedThisFrame;
    // Mid-interaction: keep the pre-edit snapshot.
    if (m_pending && m_before && m_before->sel.Is(sel.kind, sel.index))
        return;
    m_pending = false;
    m_before  = Capture(scene, sel);
}

void PropertyUndoTracker::End(SE::SceneRuntime& scene, UndoStack& undo,
                              ID3D11Device* device, SE::ShaderLibrary& shaders)
{
    if (!m_before)
        return;
    if (!m_editedBefore && ImGui::GetCurrentContext()->ActiveIdHasBeenEditedThisFrame)
        m_pending = true;
    // Wait for drags/text entry to finish so one gesture is one entry.
    if (!m_pending || ImGui::IsAnyItemActive())
        return;
    m_pending = false;

    std::shared_ptr<const EntityState> after = Capture(scene, m_before->sel);
    if (!after)
        return;
    std::shared_ptr<const EntityState> before = std::move(m_before);
    m_before = after;
    SE::SceneRuntime* scenePtr = &scene;
    SE::ShaderLibrary* shaderPtr = &shaders;
    undo.Push({ EditLabel(before->sel.kind),
                [scenePtr, before, device, shaderPtr] { Apply(*scenePtr, *before, device, *shaderPtr); },
                [scenePtr, after, device, shaderPtr]  { Apply(*scenePtr, *after, device, *shaderPtr); } });
}

} // namespace Editor
