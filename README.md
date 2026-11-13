# FoxEngine

A from-scratch DirectX 11 rendering engine built on Windows with C++17. Features a physically-based forward renderer, real-time shadows, GPU particle system, post-processing pipeline, custom physics, Lua scripting, a level editor, and ImGui debug tooling. Games are scenes plus Lua scripts, run by a generic player executable and published as a standalone folder.

## Features

### Rendering
- **PBR Forward Pipeline** — Cook-Torrance BRDF (GGX + Smith-Schlick + Fresnel)
- **Image-Based Lighting** — GPU-generated diffuse irradiance, GGX-prefiltered specular reflections, and BRDF LUT
- **Stable Reflections** — Environment fallback everywhere plus an optional roughness-aware planar reflector
- **Shadows** — Cascaded shadow maps (4-cascade), point light cube shadows, spot light shadow maps
- **Post-Processing** — HDR rendering, ACES/Reinhard tone mapping, volumetric fog / god rays, FXAA
- **Screen-Space Effects** — SSAO with hemisphere sampling and bilateral blur
- **Bloom** — Dual Kawase blur chain with threshold
- **Emissive Materials** — Per-texture emissive with intensity/color
- **Alpha Support** — Alpha test for foliage, alpha blending for transparent materials
- **Render Queue** — Front-to-back opaque, back-to-front transparent, frustum culling

### GPU Particle System
- **Fully GPU-driven** — Compute shaders for emit/update, indirect draw (no CPU readback)
- **Flipbook atlas** — Animated sprite sheets (configurable grid + frame count)
- **Shader modes** — Textured billboard (flipbook) or procedural ember glow
- **Per-emitter blend** — Additive (fire/embers) or alpha blend (smoke)
- **Soft particles** — Depth-fade near geometry intersections
- **HDR particles** — Colors above 1.0 trigger bloom glow
- **Emission mode** — Continuous or single/manual burst trigger
- **Billboard orientation** — Fixed angle with optional random rotation
- **Texture rotation** — Per-emitter UV rotation to correct source texture orientation
- **Tracer support** — Velocity-aligned billboard stretch for bullet tracers
- **Volume emitters** — Sphere or box spawn regions for localized effects and atmospheric dust
- **Lit particles** — Optional directional/environment response for smoke and dust
- **Preset defaults** — Load emitter default presets from JSON in `Assets/Particles/Presets/`

### Engine Systems
- **Scene Management** — Asynchronous dependency preparation for JSON scenes, with objects + particles + lights
- **Physics** — AABB/Sphere/OBB narrowphase, rigidbody dynamics, collision response, raycasting, character controller
- **Input** — Win32 raw input, XInput gamepad
- **Asset Pipeline** — Concurrent CPU preparation, main-thread D3D11 uploads, in-memory caching, DDS/WIC texture loading, and Assimp mesh import
- **Lighting** — Directional + point + spot lights, all with shadow support
- **Scripting**: Lua 5.4 script components and autoloads with lifecycle callbacks, coroutines, hot reload, editor Play mode, headless unit tests, and a generated LuaLS API reference

## Requirements

- Windows 10+ (x64)
- Visual Studio 2022 (MSVC v143 toolchain)
- CMake 3.20+
- [vcpkg](https://github.com/microsoft/vcpkg) (refer microsoft docs on how to setup)

## Building

```powershell
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug
```

The build produces:

- `build/Player/Debug/FoxPlayer.exe`: runs the game described by `project.json` (`--scene bistro.json` overrides the startup scene)
- `build/Editor/Debug/FoxEditor.exe`: level editor; F5 plays the open level
- `build/Tools/FoxProject/Debug/FoxProject.exe`: project command line (scripts, tests, validation, publishing)

The executables auto-detect the project root, so you can double-click them or run from any directory. Shaders, scripts and assets are loaded directly from source (`Engine/Shaders/`, `Assets/`).

**Shader/asset iteration**: Edit any `.hlsl` or asset file and re-run the exe; no rebuild needed. Lua scripts hot reload while the game runs. Only C++ changes require a build.

## Scripting

Gameplay is written in Lua 5.4. A script returns a class with optional callbacks; the properties it declares are editable per entity in the editor:

```lua
local Spinner = fox.script {
    properties = {
        { name = "speed", type = "number", default = 90, tooltip = "Degrees per second." },
    },
}

function Spinner:on_update(dt)
    self.transform:rotate(vec3(0, self.speed * dt, 0))
end

return Spinner
```

Attach scripts to entities in the editor (Details → Add Script) or list global ones under `autoload` in `project.json`, which also names the startup scene, the published scenes, input actions and window settings. Press F5 in the editor to play the level; saving a script while the game runs hot-reloads it.

`FoxProject` covers the rest of the workflow:

```powershell
FoxProject new Door --dir Game            # Assets/Scripts/Game/Door.lua + Door_test.lua
FoxProject test                           # run the *_test.lua unit tests headless
FoxProject check                          # validate project.json, scripts and scenes
FoxProject eval "vec3(1, 2, 3):length()"  # evaluate Lua in a headless session
FoxProject api                            # regenerate the API reference after changing bindings
FoxProject publish                        # standalone game in out/Publish/<ProjectName>/
```

The API reference, `Engine/Scripts/types/fox.lua`, is generated from the C++ bindings; open the folder with the Lua language server (e.g. the VS Code "Lua" extension) for completion and type checks. `publish` copies the player as `<ProjectName>.exe` with its DLLs, shaders, scripts, and the published scenes with their cooked meshes and textures. Publish from a Release build to distribute; target machines need the Microsoft Visual C++ x64 redistributable. See `AGENTS.md` (Scripting) for the full model.

## Dependencies (via vcpkg)

| Library | Purpose |
|---------|---------|
| assimp | Mesh import (FBX, glTF, OBJ) |
| imgui | Debug UI (DX11 + Win32 backend) |
| directxtex | DDS/HDR texture loading, BC compression |
| nlohmann-json | Scene descriptor serialization |
| lua | Scripting runtime (Lua 5.4) |
| sol2 | C++ ↔ Lua bindings |

## Project Structure

```
FoxEngine/
├── Engine/              # Static library (FoxEngine.lib)
│   ├── Shaders/         # HLSL shaders (loaded directly at runtime)
│   ├── Scripts/         # Lua runtime prelude + generated API reference (types/fox.lua)
│   ├── include/Engine/  # Public headers (SE:: namespace)
│   │   ├── Core/        # Platform, logging, clock, project settings
│   │   ├── Renderer/    # Graphics pipeline, mesh, materials, post-processing
│   │   ├── Physics/     # Collision, dynamics, raycasting
│   │   ├── Input/       # Keyboard, mouse, gamepad
│   │   ├── Scene/       # Entity, components, camera, scene loading
│   │   ├── Scripting/   # Lua script system
│   │   └── Assets/      # Asset manager, resource cache
│   └── src/             # Implementation (mirrors include/; Lua bindings in Scripting/Bindings/)
├── Player/              # Generic game executable (FoxPlayer.exe)
├── Editor/              # Level editor (FoxEditor.exe)
├── Assets/              # Runtime assets (textures, models, scenes)
│   ├── Scenes/          # JSON scene descriptors
│   ├── Scripts/         # Lua game scripts and their tests
│   └── Particles/       # Particle textures (flipbooks, smoke, etc.)
├── Tools/               # FoxProject (project CLI), FoxCook (mesh cooker), texture converter
└── project.json         # Game settings: startup scene, scenes, autoloads, input, window
```

## Scene Format

Scenes are defined as JSON files in `Assets/Scenes/`. Example:

```json
{
  "name": "My Scene",
  "skybox": "Assets/Textures/sky.dds",
  "camera": { "eye": [0, 5, -10], "yaw": 0, "pitch": 0 },
  "sun": { "elevation": 60, "azimuth": -135, "intensity": 1.0 },
  "bloom": { "enabled": true, "threshold": 1.0, "intensity": 0.1 },
  "fog": {
    "enabled": true,
    "density": 0.03,
    "anisotropy": 0.8,
    "scatterIntensity": 1.2,
    "steps": 48,
    "maxDistance": 60.0,
    "ambientColor": [1.0, 1.0, 1.0],
    "ambientIntensity": 0.1,
    "heightFalloff": 0.0,
    "heightBase": 0.0,
    "noiseStrength": 0.35,
    "noiseScale": 0.035,
    "noiseSpeed": 0.0
  },
  "pointLights": [
    { "position": [5, 3, 0], "color": [1, 0.8, 0.5], "radius": 50, "castShadow": true }
  ],
  "particles": [
    {
      "position": [0, 0.5, 0],
      "emit_rate": 15,
      "max_particles": 60,
      "texture": "Assets/Particles/flipbook_fire.dds",
      "atlasColumns": 5,
      "atlasRows": 5
    }
  ]
}
```

## License

MIT License — see [LICENSE](LICENSE) for details.
