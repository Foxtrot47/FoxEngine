# Copilot Instructions — FoxEngine

## Project Overview

A from-scratch DirectX 11 engine on Windows. Forward PBR renderer with HDR pipeline, custom physics, and ImGui tooling. Built with C++17/MSVC, CMake, vcpkg dependencies.

## Build & Run

```powershell
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug
# Run: build/Game/Debug/TestGame.exe
```

New `.cpp` files in `Engine/src/` are auto-discovered by `GLOB_RECURSE` — no need to edit CMakeLists, but you must re-run CMake configure for them to be picked up (the build system does this automatically when CMakeLists.txt changes).

## Architecture

- **Engine/** — Static library (`FoxEngine.lib`). All code in `namespace SE {}`.
- **Game/** — Test executable. Links `FoxEngine`. Integration target for all features.
- **Engine/Shaders/** — HLSL files copied to build dir at compile time. Compiled at runtime via `D3DCompileFromFile`.

### Renderer Pipeline (forward-only)

1. `ForwardPipeline::Begin()` → caches view/proj, extracts frustum
2. `SubmitMesh()` → frustum cull, push to render queue
3. `Flush()` → sort queue (opaque front-to-back, transparent back-to-front), bind per-draw state, draw
4. Post-process chain: **SSAO → SSR → Bloom → ToneMap → Present**

### Key Classes

| Class | Responsibility |
|-------|---------------|
| `Renderer` | D3D11 device, swap chain, surface management |
| `ForwardPipeline` | PBR shading, vertex layout, transform/material CBs, render queue |
| `LightEnvironment` | Directional + point light state (b1, b2 cbuffers) |
| `ShadowMap` | Directional light depth pass |
| `PointShadowMap` | Cube depth map (6-face) for point lights |
| `Bloom` | Dual Kawase blur chain |
| `ToneMap` | Reinhard/ACES, auto-exposure |
| `SSR` | View-space ray march, MRT normals |
| `SSAO` | Hemisphere sampling, bilateral blur, multiply composite |
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
| t4 | Sky panorama (IBL) |
| t5–t6 | Point shadow cube maps |
| t7 | Metallic map |

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
4. **Commit format**: `M##: <short description>` — e.g. `M54: Screen-space ambient occlusion`.
5. **Dependencies are additive.** Add via vcpkg or `ThirdParty/`. Never inline library source.
6. **Platform**: Windows 10+ x64 only. No cross-platform layers.
7. **Debug-first**: Default config is Debug. RelWithDebInfo for profiling.

## Design Decisions

### Forward-Only Rendering
No deferred pass. All geometry in a single HDR forward pass into `m_forwardHDR_RT` (R16G16B16A16_FLOAT). MRT output: RT0 = HDR color, RT1 = view-space normal + roughness (for SSR/SSAO).

### PBR / IBL
- Cook-Torrance BRDF: GGX NDF, Smith-Schlick geometry, Schlick Fresnel
- IBL from equirectangular panorama: diffuse at max mip, specular at roughness-scaled mip
- Panorama loaded as R16G16B16A16_FLOAT with full mip chain

### Depth Buffer
Created as `R32_TYPELESS` with DSV view (`D32_FLOAT`) and SRV view (`R32_FLOAT`) for post-process reads.

### Alpha Handling
- `AlphaCutoff > 0` in MaterialCB triggers `clip()` in PS (hard-edge cutout)
- `AlphaMode::Transparent` enables src-alpha blend + no-cull raster
- Auto-detected from glTF metadata or Assimp opacity texture; scene JSON `alphaCutoff` override

### Post-Process SRV/RTV Hazard Prevention
When a pass needs to both read and write the same RT (e.g., SSAO apply), use a multiply blend state to avoid binding the same texture as both SRV and RTV simultaneously.

## Current Milestone Roadmap

### Completed (Phase 1–8 partial)
M01–M55: Foundation, renderer bootstrap, input, lighting, scene, physics, advanced rendering (PBR, IBL, shadows, bloom, HDR, SSR, SSAO, alpha blending)

### Next Up
- M56: Spot lights (cone angle, inner/outer falloff, shadow map)
- M57: Cascaded Shadow Maps
- M58: Emissive materials
- M59: FXAA
- M60: Volumetric fog / god rays
- M61: Decals
- M62: TAA

### Phase 9 — Particle Systems
M63–M71: CPU emitter → billboard render → affectors → flipbook → GPU compute → soft particles → lit particles → trails → asset serialization

### Deferred
- Phase 10: Animation (implement when needed)
- Phase 11: Engine tooling & profiling
- Phase 12: Scripting (Lua)
- Phase 13: Editor
