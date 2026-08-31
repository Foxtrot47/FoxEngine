# Repository Guidelines

## Project Overview

A from-scratch DirectX 11 engine on Windows. Forward PBR renderer with HDR pipeline, custom physics, and ImGui tooling. Built with C++17/MSVC, CMake, vcpkg dependencies.

## Build & Run

```powershell
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug
# Run: build/Game/Debug/TestGame.exe
# Or from project root: .\build\Game\Debug\TestGame.exe --scene showcase.json
```

The exe auto-detects the project root by walking up from its location until it finds `Engine/` + `Assets/`. No need to set working directory manually.

New `.cpp` files in `Engine/src/` are auto-discovered by `GLOB_RECURSE` — no need to edit CMakeLists, but you must re-run CMake configure for them to be picked up.

**Shader/asset iteration**: Edit shaders in `Engine/Shaders/` or assets in `Assets/` and re-run the exe — no rebuild needed. Only C++ changes require a build.

## Architecture

- **Engine/** — Static library (`FoxEngine.lib`). All code in `namespace SE {}`.
- **Game/** — Test executable. Links `FoxEngine`. Integration target for all features.
- **Engine/Shaders/** — HLSL files loaded directly from source at runtime via `D3DCompileFromFile`.

### Renderer Pipeline (forward-only)

1. `ForwardPipeline::Begin()` → caches view/proj, extracts frustum
2. `SubmitMesh()` → frustum cull, push to render queue
3. `Flush()` → sort queue (opaque front-to-back, transparent back-to-front), bind per-draw state, draw
4. Post-process chain: **SSAO → Volumetric Fog → Bloom → FXAA → ToneMap → Present**

### Key Classes

| Class | Responsibility |
|-------|---------------|
| `Renderer` | D3D11 device, swap chain, surface management |
| `ForwardPipeline` | PBR shading, vertex layout, transform/material CBs, render queue |
| `LightEnvironment` | Directional + point light state (b1, b2 cbuffers) |
| `ShadowMap` | Directional light depth pass |
| `PointShadowMap` | Cube depth map (6-face) for point lights |
| `CascadedShadowMap` | 4-cascade directional shadow mapping |
| `SpotLight` | Spot light with cone angle + shadow map |
| `VolumetricFog` | Single-scattering fog, god rays, IBL ambient |
| `Bloom` | Dual Kawase blur chain |
| `ToneMap` | Reinhard/ACES, auto-exposure |
| `EnvironmentLighting` | Irradiance, GGX-prefiltered specular IBL, BRDF LUT |
| `SSAO` | Hemisphere sampling, bilateral blur, multiply composite |
| `FXAA` | Post-process anti-aliasing |
| `ParticleSystem` | GPU compute emit/update, billboard render, flipbook atlas, soft particles |
| `ShaderLibrary` | Compile + cache shader permutations |
| `RenderStateCache` | Deduplicate blend/raster/depth-stencil states |
| `AssetManager` | Path-keyed cache, ref-counted handles |

### Constant Buffer Layout (Basic.hlsl)

| Slot | Name | Contents |
|------|------|----------|
| b0 | TransformCB | Model, View, Projection (row_major) |
| b1 | LightCB | LightDir, IBLIntensity, LightColor, CameraPos, LightViewProj |
| b2 | PointLightCB | PointLight[8] (pos, radius, color), NumPointLights |
| b3 | MaterialCB | AlbedoTint, RoughnessScale, Metallic, Unlit, DebugShadow, AlphaCutoff |
| b4 | ForwardShadowCB | NumPointShadowCasters, PointShadowBias |

### Texture Slots (Basic.hlsl)

| Slot | Texture |
|------|---------|
| t0 | Albedo |
| t1 | Roughness |
| t2 | Normal map |
| t3 | Directional shadow map |
| t4 | Diffuse irradiance |
| t5–t6 | Point shadow cube maps |
| t7 | Metallic map |
| t9 | Emissive map |
| t10 | Prefiltered specular environment |
| t11 | BRDF integration LUT |
| t12 | Optional planar reflection |

## Coding Conventions

- **Naming**: Types/public functions `PascalCase`, locals/private `camelCase`, members `m_` prefix, constants `k_` prefix
- **Error handling**: `SE_HR(expr)` for HRESULT — logs + debug break. No exceptions.
- **COM pointers**: Always `ComPtr<T>`. Never raw `Release()`.
- **No singletons**: Subsystems owned by top-level `Engine` class, passed by reference.
- **Comments**: Only when the *why* is non-obvious.
- **HLSL**: One file per shader. Entry points: `VS_Main` / `PS_Main` / `CS_Main`.
- **Headers**: Public at `Engine/include/Engine/<Subsystem>/Name.h`, impl at `Engine/src/<Subsystem>/Name.cpp`.
- **Boundary**: Engine never includes Game headers. Game includes only `<Engine/...>`.

## Working Rules

1. **One milestone at a time.** Compile, run in test scene, commit before starting next.
2. **Test scene is canonical.** `Game/` is the live integration target. Wire everything into it.
3. **No premature abstraction.** Refactor only when duplication actually hurts (3+ occurrences).
4. **Commit format**: Follow Conventional Commits v1.0.0 (e.g., `feat(renderer): add SSAO pass`).
5. **Dependencies are additive.** Add via vcpkg or `ThirdParty/`. Never inline library source.
6. **Platform**: Windows 10+ x64 only. No cross-platform layers.
7. **Debug-first**: Default config is Debug. RelWithDebInfo for profiling.

## Design Decisions

### Forward-Only Rendering
No deferred pass. All geometry in a single HDR forward pass into `m_forwardHDR_RT` (R16G16B16A16_FLOAT). MRT output: RT0 = HDR color, RT1 = view-space normal + roughness (for SSAO and later temporal effects).

### PBR / IBL
- Cook-Torrance BRDF: GGX NDF, Smith-Schlick geometry, Schlick Fresnel
- IBL from equirectangular panorama: cosine-weighted irradiance, GGX-prefiltered specular, and BRDF LUT
- One authored horizontal planar reflector can supplement IBL without screen-space reflection artifacts
- Panorama loaded as R16G16B16A16_FLOAT and filtered on the GPU during level preparation

### Depth Buffer
Created as `R32_TYPELESS` with DSV view (`D32_FLOAT`) and SRV view (`R32_FLOAT`) for post-process reads.

### Alpha Handling
- `AlphaCutoff > 0` in MaterialCB triggers `clip()` in PS (hard-edge cutout)
- `AlphaMode::Transparent` enables src-alpha blend + no-cull raster
- Auto-detected from glTF metadata or Assimp opacity texture; scene JSON `alphaCutoff` override

### Post-Process SRV/RTV Hazard Prevention
When a pass needs to both read and write the same RT (e.g., SSAO apply), use a multiply blend state to avoid binding the same texture as both SRV and RTV simultaneously.

### GPU Particle System
- **Compute shaders** for emit/update/reset — fully GPU-driven, no CPU readback
- **Structured buffers**: particle pool (80 bytes), instance output (64 bytes, includes velocity), dead list, counters
- **Indirect draw** — GPU writes draw args, CPU never reads alive count
- **Flipbook atlas** — configurable columns/rows/frameCount/speed for animated sprites
- **Two shader modes**: 0 = textured billboard (flipbook), 1 = ember glow (procedural)
- **Per-emitter blend**: additive (fire/embers) or alpha blend (smoke)
- **Billboard controls**: fixed angle, optional random rotation, and per-emitter UV texture rotation
- **Tracer support**: velocity-aligned stretch for bullet/trail effects
- **Preset defaults**: load emitter presets from JSON (`Assets/Particles/Presets/`)
- **Soft particles**: depth-fade near geometry intersections
- **HDR colors**: values > 1.0 trigger bloom glow

### Development Asset Loading
- Levels and source assets are loaded directly during the current runtime/rendering phase.
- `LevelLoadOperation` prepares dependencies asynchronously, uploads GPU resources on the main thread, and reuses the in-memory `AssetManager` cache.
- Assimp remains the development mesh importer for FBX, glTF, and OBJ assets.
- Do not introduce native cooked asset formats, a disk-backed derived-data cache, incremental cooking/version invalidation, or publish-time asset packaging before the Editor/tooling phase.
- A cooked pipeline is deferred because it requires editor-facing import workflows and format/version management that are premature for the current project stage.

## Current Milestone Roadmap

### Completed (Phase 1–9 partial)
M01–M55: Foundation, renderer bootstrap, input, lighting, scene, physics, advanced rendering (PBR, IBL, shadows, bloom, HDR, SSAO, alpha blending; the original SSR experiment was retired)
M56: Spot lights (cone angle, inner/outer falloff, shadow map)
M57: Cascaded Shadow Maps (4-cascade)
M58: Emissive materials
M59: FXAA
M60: Volumetric fog / god rays
M67: GPU compute particle system
M68: Per-emitter blend mode, drag, flipbook fire, ember shader
M69: Smoke particles with expanding billboard puffs
M70: More particle effects (explosions, sparks/trails, dust)
M71: Particle asset serialization / default presets
M72: Material workflow correctness (metallic/roughness, packed MR, specular/glossiness; node-transform baking at import)

### Next Up

- M61: Decals
- M62: TAA

### Deferred
- Phase 10: Animation (implement when needed)
- Phase 11: Engine tooling & profiling
- Phase 12: Scripting (Lua)
- Phase 13: Editor
- Cooked asset pipeline and publishing: native runtime formats, disk-backed derived-data cache, incremental recooking/version invalidation, and final asset packaging (implement with the Editor/tooling phase)
