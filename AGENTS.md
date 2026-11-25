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
- **Editor/**: `FoxEditor.exe` level editor with Play mode, Console, script property editing and an MCP server on `127.0.0.1:47800/mcp` (`.mcp.json`; inspect, edit, screenshot, play and drive input from an agent; `--mcp-port`/`--no-mcp`; see `Editor/README.md`).
- **Tools/FoxProject/**: Headless project CLI: API stubs, Lua tests, validation, REPL, scaffolding, publish (see Scripting).
- **Engine/Shaders/** — HLSL files loaded directly from source at runtime via `D3DCompileFromFile`.
- **Engine/Scripts/**: Lua runtime prelude (`fox/runtime.lua`) and the generated LuaLS API reference (`types/fox.lua`).
- **Projects/Showcase/**: Canonical sample game: `project.json`, `Assets/` (including game scripts/tests), and ignored `Cache/`.
- **Tests/Fixtures/**: Test-only scenes and importer assets, isolated from game content. LevelAssetTests runs sample-content checks in `FOX_DEFAULT_PROJECT`, then importer/async checks in the fixture directory.

### Renderer Pipeline (forward-only)

1. `ForwardPipeline::Begin()` → caches view/proj, extracts frustum
2. `SubmitMesh()` → frustum cull, push to render queue
3. `Flush()` → sort queue (opaque front-to-back, transparent back-to-front), bind per-draw state, draw
4. Post-process chain: **SSAO → Volumetric Fog → TAA/FSR → Motion Blur → Bloom → FXAA → ToneMap → Present**

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
| `MotionBlur` | Velocity-buffer motion blur (tile/neighbour max, depth-aware gather); level `settings.motionBlur`, Lua `fox.graphics.motion_blur` |
| `Bloom` | Dual Kawase blur chain |
| `ToneMap` | Reinhard/ACES, auto-exposure |
| `EnvironmentLighting` | Irradiance, GGX-prefiltered specular IBL, BRDF LUT |
| `ReflectionProbes` | Box-projected reflection probes from level `settings.reflectionProbes` (max 8): 128² HDR cube capture at load or on editor Recapture, equirect + GGX prefilter into a probe array (b9, t28) |
| `SSAO` | Hemisphere sampling, bilateral blur, multiply composite |
| `FXAA` | Post-process anti-aliasing |
| `ParticleSystem` | GPU compute emit/update, billboard render, flipbook atlas, soft particles |
| `ShaderLibrary` | Compile + cache shader permutations |
| `RenderStateCache` | Deduplicate blend/raster/depth-stencil states |
| `AssetManager` | Path-keyed cache, ref-counted handles |
| `ScriptSystem` | Lua session: sandboxed VM, script instances + lifecycle, coroutines, hot reload, REPL, tests, API stub generation |
| `Vehicle` | Wheeled vehicle on a Jolt chassis body: suspension, Pacejka tires, ABS/TC, engine + gearbox, LSD, aero (`Engine/Physics/Vehicle/`) |
| `Project` | `project.json` loading/validation, published scene list |

### Constant Buffer Layout (Basic.hlsl)

| Slot | Name | Contents |
|------|------|----------|
| b0 | TransformCB | Model, View, Projection (row_major) |
| b1 | LightCB | LightDir, IBLIntensity, LightColor, CameraPos, LightViewProj |
| b2 | PointLightCB | PointLight[8] (pos, radius, color), NumPointLights |
| b3 | MaterialCB | AlbedoTint, RoughnessScale, Metallic, Unlit, DebugShadow, AlphaCutoff, emissive, workflow, OcclusionStrength, per-slot UV set/scale/offset (`SlotST`, `SlotUVA/B`), slot flags, tint/detail params, terrain layer scales and params (`TerrainScale`, `TerrainParams`, `TerrainParams2`, `TerrainST`), clearcoat params (`Clearcoat`, `ClearcoatST`, `ClearcoatUV`) (`MaterialParamsCBData` mirrors it, size asserted) |
| b4 | ForwardShadowCB | NumPointShadowCasters, PointShadowBias, FrameIndex, CutoutDitherWidth (temporal cutout dither) |
| b9 | ReflectionProbeCB | ProbeCenter[8], ProbeExtents[8] (xyz half extents, w blendDistance), ProbeInfo (x count, y mip count) |

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
| t13 | Occlusion (glTF `occlusionTexture`; multiplies IBL ambient only) |
| t14 | Detail albedo (`extras.fox.detailAlbedo`, ×4.59 around sRGB mid-grey) |
| t15 | Detail normal (`extras.fox.detailNormal`, UDN blend) |
| t16–t19 | Terrain layer albedo A–D (`FOX_TERRAIN` permutation; alpha = roughness when `albedoAlphaIsRoughness`) |
| t20 | Terrain splat (layers B, C, D lerp over A by R, G, B) |
| t21 | Terrain overlay (colour multiply, weighted by strength × alpha) |
| t22–t25 | Terrain layer normal A–D |
| t26 | Clearcoat factor (`KHR_materials_clearcoat` `clearcoatTexture`, R channel) |
| t27 | Clearcoat roughness (`KHR_materials_clearcoat` `clearcoatRoughnessTexture`, G channel) |
| t28 | Reflection probe array (Texture2DArray, 8 slices of 256×128 equirect, 8 GGX mips; box-projected, weighted over t10) |

## Scripting

Lua 5.4 bound with sol2. Each session (a FoxPlayer run, an editor Play session, a headless `FoxProject` command) gets its own sandboxed VM. The C++ entry point is `SE::ScriptSystem` (`Engine/include/Engine/Scripting/ScriptSystem.h`).

### Script Model
- A script is a module under `Assets/Scripts/` returning a class. `require("Game.CameraMath")` loads `Assets/Scripts/Game/CameraMath.lua`.
- **Actors** are the only level objects with behaviour. An actor class is `fox.actor { base = ..., components = { ... }, properties = { ... } }`: each component (`mesh`, `point_light`, `particle_emitter`, `box`, `sphere`) becomes a child entity exposed as `self.<name>` (`fox.ActorComponent`). Static entities (meshes, lights, primitives, empties) take no `scripts` or `properties`; the loader rejects them.
- `box` and `sphere` are rendered shapes and colliders: `color` (albedo 0..1, default 0.8 grey), `emissive` (linear HDR glow, default 0; above 1 blooms) and `cast_shadows` style them, `visible = false` keeps only the collider (triggers, invisible walls) and `collision = false` keeps only the shape (view models, decoration; a `dynamic` body needs collision). They render through the mesh path, so shadows, reflections, motion vectors and editor picking apply.
- A placement names its class in `actor`; its `transform` is the actor root, other `components` keys override per-component fields by name (any key except `name`/`type`), `properties` override class properties, and `scripts` adds `fox.script { properties = { ... } }` components that run after the class instance. Levels store only overrides; defaults come from the class, so changing one updates every placement without an override:
  ```json
  { "id": "0000000000000005", "name": "Brazier Right",
    "actor": "Assets/Scripts/Actors/Brazier.lua",
    "components": { "transform": { "position": [3.5, 0, 2] }, "light": { "color": [0.6, 1.4, 2.8] } },
    "properties": { "brightness": 1.5 },
    "scripts": [ { "script": "Assets/Scripts/Examples/Bobber.lua", "properties": { "amplitude": 0.6 } } ] }
  ```
- Scripts create actors at runtime with `fox.world.spawn(class_or_path, { position = ..., properties = ..., components = ... })`; spawned actors are not saved. `fox.world.actors(class)` and `fox.world.destroy(actor)` complete the set. `Projects/Showcase/Assets/Scenes/actors.json` is the reference scene.
- Level transforms use `rotationDegrees` (the Lua side spells it `rotation`); the loader warns about any other transform key.
- `Projects/Showcase/Assets/Scenes/shooting_range.json` is the gameplay reference: `Actors/FirstPersonPlayer.lua` (walk/sprint/jump on `fox.character`, mouse and gamepad look, a render-only box view model, ballistic-aimed `Actors/Projectile.lua` shots, crosshair HUD) among dynamic `Crate`s, `Block` geometry and `Target` plates (`on_hit` from a `Projectile`). An actor class that sets `controls_camera = true` owns the camera; `Game/PlayerController.lua` stands down while one exists.
- Actor classes are Lua, so `SceneRuntime` resolves every placed class on the main thread (`actorClassResolver`) before the level load worker starts; the worker then preloads their meshes. Unresolved classes log a level warning and the actor has no components.
- **Autoloads** (`project.json` `autoload`) have no entity, live for the whole session across scene loads and receive `on_scene_loaded(scene)`. Global game logic goes there (`Game/PlayerController.lua`, `Game/PauseMenu.lua`).
- Callbacks are optional methods: `on_enable`, `on_start`, `on_update(dt)`, `on_fixed_update(dt)`, `on_hit(hit)`, `on_late_update(dt)`, `on_gui`, `on_disable`, `on_destroy`, `on_reload`. Each frame runs coroutines → `on_update` → physics steps (`on_fixed_update`) → `on_hit` → `on_late_update` → `on_gui`.
- `on_hit(hit)` (`fox.Hit`: `entity`, `actor`, `part`, `other_part`, `position`, `normal`, `relative_velocity`, `speed`) runs on every script of the entity whose body starts touching another, once per pair until they separate. `PhysicsWorld` collects Jolt `OnContactAdded` on job threads and reports `PhysicsContact`s to `PhysicsStepListener::OnPhysicsContact` after `Step`. Resting pairs report again when they wake (filter on `hit.speed`); only pairs with a dynamic body touch, and `fox.character` is not a body, so it never reports.
- Keep state on `self`: module-level locals reset on hot reload. `self:start_coroutine(fn)` with `fox.wait(seconds)` / `fox.wait_frames(n)` sequences work across frames.
- `fox.time.scale` scales dt, physics and particles; 0 pauses them while `on_update` (with dt 0) and `on_gui` keep running.
- Input goes through actions declared in `project.json` `input` (`"jump": ["Space", "Gamepad.A"]` → `fox.input.action_pressed("jump")`); key names are the `fox.Key` alias.
- `fox.ui` (ImGui) works only inside `on_gui`.
- Reference assets as literal `"Assets/..."` strings: publish scans script literals to ship them.

### Vehicles
A vehicle definition (`Assets/Vehicles/<Car>/vehicle.json`) holds the physics (chassis, axles, engine, gearbox, differential), a `layout` file with wheel positions and the collision hull, and a `visual` block naming the body, four wheel meshes (spin with the wheel) and optional four `spindles` (brake calipers: steer and follow the suspension, no spin), all modelled in vehicle space with wheel parts around their wheel center. Vehicles are runtime state, not level objects: `fox.vehicle.spawn(path, position, yaw)` starts the async mesh load and returns a `fox.Vehicle` handle whose `ready` turns true when it can be driven. The handle exposes `set_input(throttle, brake, steer, handbrake)`, gearbox/aid controls, speed/rpm/gear telemetry and reset; the next scene load removes every car. `SceneRuntime` renders the chassis, wheels and spindles in the shadow, reflection and lit passes with motion vectors. `Projects/Showcase/Assets/Scripts/Actors/CarSpawn.lua` is the reference: a placement that spawns a car, drives it with the `drive_*` actions and owns a chase camera/HUD (`Assets/Scenes/monza.json`); publish ships a definition's layout and visual meshes when a script literal names its JSON.

`Assets/Vehicles/Porsche911GT3R/` (LMU 992 GT3 R 2026, Manthey #92 livery) is generated by `Tools/External/LMU/import_lmu_vehicle.py <raw_dir> <livery.veh>` from files extracted from Le Mans Ultimate `.mas` archives: it reads the `.gen` instance tree and GMT meshes directly (not the GLB export), bakes LMU materials and the livery to glTF 2.0 metallic-roughness with one primitive per material, writes BC-compressed DDS textures with mip chains and `layout.json`. `vehicle.json` physics are hand-authored from the car's `.hdv`/engine files (see its `_notes`).

`Assets/Vehicles/Ferrari488GTEEvo/` (LMU Ferrari 488 GTE Evo 2023, AF Corse #21 Le Mans livery `21_AFCorsefdf53f44.veh`) comes from the same importer with `--out`; it is `CarSpawn.lua`'s default car. The Porsche stays the `VehicleTests` reference.

### Tracks
`Assets/Tracks/RoadAtlanta/` (LMU Road Atlanta 2026) and `Assets/Scenes/roadatlanta.json` are generated by `Tools/External/LMU/import_lmu_track.py <raw_dir> --shared <commonmaps_dir>` from the track's extracted `.mas` files (`.scn`, `.AIW`, GMTs, material JSON, DDS). GMT world space is engine space, so `.scn`/`.AIW` coordinates are used as-is; `Moveable` instances (ground ad and speed-limit signs) are modelled around the origin and placed at their GMT header pivot. Geometry is binned into 250 m cells per kind: `surface_*` (CollTarget asphalt, kerbs, grass, gravel: static mesh colliders), `scenery_*` (shadow casters), `ground_*` (terrain, lines, decals), `*_wide` (meshes over 1 km wide, kept whole) and `collision.gltf` (the `.scn`'s invisible wall/armco/tyre proxies). Timing triggers, shadow-only proxies and animated instances (marshals, birds) are skipped. Track shaders are baked to one UV set: the albedo stage's UV scale or WorldPlanar mapping goes into the UVs, `_ALBR` alpha becomes roughness, cutout `_ALBA`/`_DCA` alpha becomes MASK (not clearcoat masks or `BLENDMASK=AlbedoA_A` palette masks). Parked vehicles, crowds, flags, tents and pit equipment use the game's shared `Core/Locations/Commonmaps.mas`, whose entries are encrypted with a cipher the extractor does not implement: lmu-gamerip `tools/dump_mas_entries.py` dumps them through the running game's own decrypt function, and `--shared` points the importer at that dump (without it those materials fall back to grey). The scene puts a `CarSpawn` on pole position (AIW grid slot 0, heading along the grid). Both importers share `Tools/External/LMU/lmu_assets.py` (GMT reader, LMU materials, texture and glTF writers).

`Assets/Tracks/` is gitignored (extracted LMU content is proprietary and about 1 GB): a fresh clone has `roadatlanta.json` listed in `project.json` but no track meshes, so `FoxProject check`, `publish` and the level tests that load every listed scene fail until the importer has been run locally.

Terrain grass (LMU `L2IBLTERRAIN`) blends four world-tiled layers per pixel by a circuit-wide splat map (UV_B, an affine map of world X/Z) and multiplies in a colour map (UV_C, a ribbon along the track). The importer writes each such material as a `<material>.foxmat` terrain sidecar (layers A–D with `worldScale`, splat on UV set 1, overlay with its UV set, `uvScale` and strength) referenced from `extras.fox.material`, and the engine blends it per pixel in the `FOX_TERRAIN` permutation of `Basic.hlsl` (B ← splat.r, C ← splat.g, D ← splat.b; the order is inferred, not decoded from the shader). Layer normals (`normalA` → layers A–B, `normalB` → C–D) are inferred too; pattern/mix maps and vertex colour are not reproduced.

Parked vehicles, trucks, trailers, fuel rigs, pit stands and awnings are painted per vertex from a palette texture: each vertex's UV_B picks a swatch, and the albedo's alpha masks where it multiplies in (`PaletteTint`: `L2IBLSTANDARDBLEND` `albedoAltMap` with `BLENDMASK=AlbedoA_A`, or `L2IBLCLEARCOAT` `detailMap` with `DETAILMASKING=AlbedoAlpha`). The importer samples the swatch per vertex into `COLOR_0` and marks the material `extras.fox.vertexTint = "albedoAlphaMask"`; the engine multiplies it in where the albedo alpha masks.

A level `meshRenderer` with `"collision": true` builds a static mesh collider from its opaque submeshes; `"visible": false` keeps only that collider (invisible walls). The editor's Show Colliders draws every mesh collider as wireframe (level meshes orange, at the pose they were added at load; actor meshes cyan) and makes collider-only meshes pickable.

### Runtime Guarantees
- `fox.*` module tables are strict: unknown fields raise errors, so typos fail loudly.
- Sandbox: no `io`, `load`/`loadfile`/`dofile` or `string.dump`; `os` keeps only `clock`/`time`/`date`/`difftime`, `debug` only `traceback`.
- Script errors are logged as `file(line)` with a traceback and never reach C++; the failing instance stops until its file is saved again. A single callback running longer than 5 s is aborted.
- Hot reload (development builds): loaded files are polled twice a second; changed classes are patched in place, instances keep `self`, new properties get their defaults and `on_reload` runs. When an actor class's component list changes, its live actors rebuild their parts in place (root, transform and `self` kept; `self.<name>` points at the new parts).

### API Reference and New Bindings
- `Engine/Scripts/types/fox.lua` (LuaLS `---@meta`) documents every binding and is **generated** from `Engine/src/Scripting/Bindings/Bind*.cpp`: never edit it. Opening the repo root with the Lua language server gives completion and type checks.
- Add bindings in the matching `Bind<Area>.cpp` through the `ScriptApi` builders: `api.Module(name, doc)` with `.Function(name, "(path: string, volume?: number) -> boolean", doc, fn)`, `.Property`, `.ReadOnly`; `api.Class<T>(name, doc)` for usertypes. Raise Lua errors with `Raise(fmt, ...)`. The signature and doc strings become the reference, so it cannot drift. Then run `FoxProject api` and commit the stub; `FoxProject check` warns when it is stale.
- The Lua-side API (`fox.script`, `fox.actor`, `fox.Script`/`fox.Actor` methods, `fox.wait`, `fox.test`, `fox.inspect`) lives in `Engine/Scripts/fox/runtime.lua`. CTest runs `FoxProject api --check`, so a binding change without a regenerated stub fails the suite.

### FoxProject CLI
`build/Tools/FoxProject/<Config>/FoxProject.exe` accepts `--project` before or after the command; without it, the nearest project above cwd or configured sample is used. Diagnostics print as `path(line): error: message`; exit codes: 0 success, 1 failures, 2 usage error. `api` reads/writes the engine's stubs, never a project's copy. Explicit `publish --out` paths are invocation-relative.

| Command | Purpose |
|---------|---------|
| `new <Name> [--dir <subdir>] [--actor]` | Create `Assets/Scripts[/<subdir>]/<Name>.lua` and `<Name>_test.lua`; `--actor` scaffolds a placeable `fox.actor` class instead of a `fox.script` |
| `test [filter]` | Run `Assets/Scripts/**/*_test.lua` headless (`fox.test.case`, `expect`, `expect_eq`, `expect_near`) |
| `check` | Validate `project.json`, every script and every scene, including actor classes, component overrides and property overrides of placements and script components |
| `eval <lua...>` | Evaluate Lua in a headless session (no window or scene) |
| `api [--check]` | Regenerate or verify `Engine/Scripts/types/fox.lua` |
| `publish [--out <dir>] [--clean]` | `check`, then write the standalone game |
| `upgrade [scene...]` | Rewrite legacy unversioned scenes (default: all in `Assets/Scenes`) as FoxLevel version 2 |

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
- Reflection probes (`settings.reflectionProbes`: `[{ "position": [x,y,z], "extents": [hx,hy,hz], "blendDistance": 5 }]`, first 8 used): captured once after a level loads (sun CSM refit per face, current point/spot shadows, global IBL only, no probe recursion) and blended in `IBL()` specular by `saturate(distance inside the box / blendDistance)`, the remainder from the global map; diffuse irradiance stays global and the planar reflector still overrides on its plane
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
- glTF (`.gltf`/`.glb`) is read natively with cgltf (`Renderer/GltfImport.cpp`; MikkTSpace tangents when the file has none; Draco/meshopt compression is rejected); Assimp imports FBX, OBJ and everything else. `Mesh::Import` takes an optional `MeshImporter` to force either path; both produce the same left-handed output (Z mirrored, node transforms baked, one submesh per primitive instance in mesh order). `MeshCache` cooks imported meshes to `Cache/Meshes/*.fxmesh` (zstd, 256 KiB blocks) on first load or via `FoxCook` (`Tools/FoxCook`).
- Cooked entries invalidate when the source or any side file the importer read (`.bin`, `.mtl`, `.foxmat`, ...) changes, when the importer (cgltf or Assimp) or its version changes, or when `k_meshCacheVersion` is bumped; a missing source loads its cooked entry.
- Layered terrain: a glTF material whose `extras.fox.material` names a `.foxmat` sidecar (JSON, path relative to the glTF; `format` `"FoxMaterial"`, `version` 1, `type` `"terrain"`, 1–4 `layers` of `albedo`/optional `normal`/`worldScale`, `albedoAlphaIsRoughness`, `splat {texture, uvSet, uvScale}`, optional `overlay {texture, uvSet, uvScale, strength}`, `roughnessScale`; `uvScale` is an optional `[sx, sy]`; texture paths relative to the sidecar) becomes `MaterialKind::Terrain`. Layers tile world X/Z × `worldScale` and are drawn by `Basic.hlsl` compiled with `FOX_TERRAIN=1`; an invalid sidecar logs `foxmat '<path>': <reason>` and keeps the glTF's own material.
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
M74: Actors (`fox.actor` classes from mesh/light/emitter/collider components, per-placement overrides, `fox.world` spawning, kinematic bodies; editor placement and editing)
M75: Vehicles (Jolt chassis + suspension/tire/drivetrain simulation, async runtime spawn, `fox.vehicle` scripting, chase-camera demo scene, publish support)
M76: Tracks (LMU track importer, Road Atlanta level on static mesh colliders, collider-only level meshes)

### Next Up

- M61: Decals

### Deferred
- Phase 10: Animation (implement when needed)
- Phase 11: Engine tooling & profiling
- Phase 13: Editor
