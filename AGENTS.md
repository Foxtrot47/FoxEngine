# Repository Guidelines

## Project Overview

A from-scratch DirectX 11 engine on Windows. Forward PBR renderer with HDR pipeline, custom physics, Lua scripting, and ImGui tooling including a level editor. A game is `project.json` + scenes + Lua scripts, run by the generic `FoxPlayer` executable and shipped with `FoxProject publish`; no game code is compiled. Built with C++17/MSVC, CMake, vcpkg dependencies.

## Build & Run

```powershell
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug
# Game:    build/Player/Debug/FoxPlayer.exe [--project <dir>] [--scene bistro.json]
# Editor:  build/Editor/Debug/FoxEditor.exe [--project <dir>]        (F5 plays the open level)
# Project: build/Tools/FoxProject/Debug/FoxProject.exe [--project <dir>] help
```

Source-tree default: `Projects/Showcase/project.json`; CMake `FOX_DEFAULT_PROJECT` can select another default. FoxPlayer, FoxEditor, FoxProject and FoxCook accept `--project <directory-or-project.json>` (or `--project=<path>`). Selection order: explicit path, project beside the executable, nearest `project.json` above cwd, configured default. Invalid explicit paths fail. The selected project becomes cwd; `Assets/...` and `Cache/Meshes` stay project-relative.

Engine resources are separate: `SE::EnginePath("Engine/...")` / `EnginePath("ThirdParty/...")` resolve from `GetEngineRoot()`, discovered above the executable then from the configured source root. ShaderLibrary resolves relative shader paths centrally, only on cache misses. API stubs always remain engine-owned. Published games include their own `Engine/` resources and select their adjacent `project.json` without relying on the checkout.

New `.cpp` files in `Engine/src/` are auto-discovered by `GLOB_RECURSE` — no need to edit CMakeLists, but you must re-run CMake configure for them to be picked up.

**Shader/asset/script iteration**: Edit shaders in `Engine/Shaders/` or assets/scripts in the selected project's `Assets/` and re-run the exe; no rebuild needed. Lua scripts hot reload while FoxPlayer or editor Play mode runs. Only C++ changes require a build.

## Architecture

- **Engine/** — Static library (`FoxEngine.lib`). All code in `namespace SE {}`.
- **Player/**: `FoxPlayer.exe`, the generic game executable: loads `project.json`, its startup scene and scripts. Holds no game logic; `FoxProject publish` ships it as `<ProjectName>.exe`.
- **Editor/**: `FoxEditor.exe` level editor with Play mode, Console and script property editing (see `Editor/README.md`).
- **Tools/FoxProject/**: Headless project CLI: API stubs, Lua tests, validation, REPL, scaffolding, publish (see Scripting).
- **Engine/Shaders/** — HLSL files loaded directly from source at runtime via `D3DCompileFromFile`.
- **Engine/Scripts/**: Lua runtime prelude (`fox/runtime.lua`) and the generated LuaLS API reference (`types/fox.lua`).
- **Projects/Showcase/**: Canonical sample game: `project.json`, `Assets/` (including game scripts/tests), and ignored `Cache/`.
- **Tests/Fixtures/**: Test-only scenes and importer assets, isolated from game content. LevelAssetTests runs sample-content checks in `FOX_DEFAULT_PROJECT`, then importer/async checks in the fixture directory.

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
| `ScriptSystem` | Lua session: sandboxed VM, script instances + lifecycle, coroutines, hot reload, REPL, tests, API stub generation |
| `Project` | `project.json` loading/validation, published scene list |

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

## Scripting

Lua 5.4 bound with sol2. Each session (a FoxPlayer run, an editor Play session, a headless `FoxProject` command) gets its own sandboxed VM. The C++ entry point is `SE::ScriptSystem` (`Engine/include/Engine/Scripting/ScriptSystem.h`).

### Script Model
- A script is a module under `Assets/Scripts/` returning a class made by `fox.script { properties = { ... } }`. `require("Game.CameraMath")` loads `Assets/Scripts/Game/CameraMath.lua`.
- **Script components** attach to level entities (mesh, camera, sun, point light, particle emitter, primitive, empty) in a `scripts` array beside `components`. Levels store only property overrides; defaults come from the script, so changing one updates every instance without an override:
  ```json
  { "id": "0000000000000007", "name": "Turntable",
    "components": { "transform": { "position": [0, 1, 0] } },
    "scripts": [ { "script": "Assets/Scripts/Examples/Spinner.lua", "properties": { "speed": 45 } } ] }
  ```
- **Autoloads** (`project.json` `autoload`) have no entity, live for the whole session across scene loads and receive `on_scene_loaded(scene)`. Global game logic goes there (`Game/PlayerController.lua`, `Game/PauseMenu.lua`).
- Callbacks are optional methods: `on_enable`, `on_start`, `on_update(dt)`, `on_fixed_update(dt)`, `on_late_update(dt)`, `on_gui`, `on_disable`, `on_destroy`, `on_reload`. Each frame runs coroutines → `on_update` → physics steps (`on_fixed_update`) → `on_late_update` → `on_gui`.
- Keep state on `self`: module-level locals reset on hot reload. `self:start_coroutine(fn)` with `fox.wait(seconds)` / `fox.wait_frames(n)` sequences work across frames.
- `fox.time.scale` scales dt, physics and particles; 0 pauses them while `on_update` (with dt 0) and `on_gui` keep running.
- Input goes through actions declared in `project.json` `input` (`"jump": ["Space", "Gamepad.A"]` → `fox.input.action_pressed("jump")`); key names are the `fox.Key` alias.
- `fox.ui` (ImGui) works only inside `on_gui`.
- Reference assets as literal `"Assets/..."` strings: publish scans script literals to ship them.

### Runtime Guarantees
- `fox.*` module tables are strict: unknown fields raise errors, so typos fail loudly.
- Sandbox: no `io`, `load`/`loadfile`/`dofile` or `string.dump`; `os` keeps only `clock`/`time`/`date`/`difftime`, `debug` only `traceback`.
- Script errors are logged as `file(line)` with a traceback and never reach C++; the failing instance stops until its file is saved again. A single callback running longer than 5 s is aborted.
- Hot reload (development builds): loaded files are polled twice a second; changed classes are patched in place, instances keep `self`, new properties get their defaults and `on_reload` runs.

### API Reference and New Bindings
- `Engine/Scripts/types/fox.lua` (LuaLS `---@meta`) documents every binding and is **generated** from `Engine/src/Scripting/Bindings/Bind*.cpp`: never edit it. Opening the repo root with the Lua language server gives completion and type checks.
- Add bindings in the matching `Bind<Area>.cpp` through the `ScriptApi` builders: `api.Module(name, doc)` with `.Function(name, "(path: string, volume?: number) -> boolean", doc, fn)`, `.Property`, `.ReadOnly`; `api.Class<T>(name, doc)` for usertypes. Raise Lua errors with `Raise(fmt, ...)`. The signature and doc strings become the reference, so it cannot drift. Then run `FoxProject api` and commit the stub; `FoxProject check` warns when it is stale.
- The Lua-side API (`fox.script`, `fox.Script` methods, `fox.wait`, `fox.test`, `fox.inspect`) lives in `Engine/Scripts/fox/runtime.lua`.

### FoxProject CLI
`build/Tools/FoxProject/<Config>/FoxProject.exe` accepts `--project` before or after the command; without it, the nearest project above cwd or configured sample is used. Diagnostics print as `path(line): error: message`; exit codes: 0 success, 1 failures, 2 usage error. `api` reads/writes the engine's stubs, never a project's copy. Explicit `publish --out` paths are invocation-relative.

| Command | Purpose |
|---------|---------|
| `new <Name> [--dir <subdir>]` | Create `Assets/Scripts[/<subdir>]/<Name>.lua` and `<Name>_test.lua` |
| `test [filter]` | Run `Assets/Scripts/**/*_test.lua` headless (`fox.test.case`, `expect`, `expect_eq`, `expect_near`) |
| `check` | Validate `project.json`, every script and every scene, including script property overrides |
| `eval <lua...>` | Evaluate Lua in a headless session (no window or scene) |
| `api [--check]` | Regenerate or verify `Engine/Scripts/types/fox.lua` |
| `publish [--out <dir>] [--clean]` | `check`, then write the standalone game |

Gameplay loop: edit the script → `FoxProject check` → `FoxProject test` → run FoxPlayer or editor Play mode, whose Console evaluates Lua in the running game.

### project.json and Publishing
- Keys: `name`, `startupScene`, `scenes` (published scenes; empty = every scene in `Assets/Scenes`), `autoload` (script paths, instantiated in order), `input` (action → key names), `window` (`title`, `width`, `height`, `fullscreen`). Comments are allowed. Unknown keys, bad key names and missing files are errors; FoxPlayer refuses to start on them.
- `FoxProject publish` writes the selected project's `out/Publish/<ProjectName>/`, incrementally unless `--clean`: FoxPlayer from FoxProject's own build configuration as `<ProjectName>.exe` with its DLLs, engine-owned `Engine/Shaders` and `Engine/Scripts` without `types/`, non-test game scripts, the listed scenes plus scenes named by `scene` script properties or script literals, their cooked meshes (`Cache/Meshes`), textures and particle presets, and `project.json` marked `"published": true` (no hot reload, no frame stats in the title). Output cannot overlap project assets/cache or engine sources.
- Every published scene must load: a scene mesh with neither a source nor a cooked entry aborts the publish. `scenes` lists the scenes whose assets are in the repository (`Assets/Sponza/` is not).
- Distribute a Release publish: a Debug player opens a log console and needs the Visual Studio debug runtimes (publish warns). Target machines need the Microsoft Visual C++ x64 redistributable.

## Coding Conventions

- **Naming**: Types/public functions `PascalCase`, locals/private `camelCase`, members `m_` prefix, constants `k_` prefix
- **Error handling**: `SE_HR(expr)` for HRESULT — logs + debug break. No exceptions.
- **COM pointers**: Always `ComPtr<T>`. Never raw `Release()`.
- **No singletons**: Subsystems owned by top-level `Engine` class, passed by reference.
- **Comments**: Only when the *why* is non-obvious.
- **HLSL**: One file per shader. Entry points: `VS_Main` / `PS_Main` / `CS_Main`.
- **Headers**: Public at `Engine/include/Engine/<Subsystem>/Name.h`, impl at `Engine/src/<Subsystem>/Name.cpp`.
- **Lua**: `snake_case` functions, fields and script properties; `PascalCase` script classes and their files (`Assets/Scripts/Game/PlayerController.lua`); one class per file, returned from the module; tests in `<Name>_test.lua` beside it.
- **Boundary**: Engine never includes Player, Editor or Tools headers; they include only `<Engine/...>`. Lua and sol2 stay inside `Engine/src/Scripting/`.

## Working Rules

1. **One milestone at a time.** Compile, run in test scene, commit before starting next.
2. **Test project is canonical.** `Projects/Showcase/project.json` + its `Assets/Scenes/showcase.json`, run by FoxPlayer and editor Play mode, are the live integration target. Wire everything into it; gameplay belongs in Lua scripts, not in `Player/`.
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
- Assimp is the source importer for FBX, glTF, and OBJ assets. `MeshCache` cooks imported meshes to `Cache/Meshes/*.fxmesh` (zstd, 256 KiB blocks) on first load or via `FoxCook` (`Tools/FoxCook`).
- Cooked entries invalidate when the source or any side file the importer read (`.bin`, `.mtl`, ...) changes, when the Assimp version changes, or when `k_meshCacheVersion` is bumped; a missing source loads its cooked entry.
- Textures still use offline BC DDS from `Tools/ConvertTextures.ps1`.

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
M62: TAA (jittered projection, per-object motion vectors, variance-clipped resolve) + geometric specular AA
M63: FSR 3.1 upscaling (vendored prebuilt DX11 SDK in `ThirdParty/fsr3/`; upscale-only — frame generation needs DX12/Vulkan; Native AA mode is the default AA path, mutually exclusive with TAA)
M73: Lua scripting (script components + autoloads, lifecycle, coroutines, hot reload; editor Play mode, Console and script properties; generic FoxPlayer; FoxProject CLI with tests, validation and publish)

### Next Up

- M61: Decals

### Deferred
- Phase 10: Animation (implement when needed)
- Phase 11: Engine tooling & profiling
- Phase 13: Editor
