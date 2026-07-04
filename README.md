# FoxEngine

A from-scratch DirectX 11 rendering engine built on Windows with C++17. Features a physically-based forward renderer, real-time shadows, GPU particle system, post-processing pipeline, custom physics, and ImGui debug tooling.

## Features

### Rendering
- **PBR Forward Pipeline** — Cook-Torrance BRDF (GGX + Smith-Schlick + Fresnel)
- **Image-Based Lighting** — Equirectangular HDR panorama sampling for diffuse irradiance + specular reflections
- **Shadows** — Cascaded shadow maps (4-cascade), point light cube shadows, spot light shadow maps
- **Post-Processing** — HDR rendering, ACES/Reinhard tone mapping, FXAA
- **Screen-Space Effects** — SSR, SSAO (hemisphere sampling + bilateral blur)
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

### Engine Systems
- **Scene Management** — JSON scene descriptors with hot-reload, objects + particles + lights
- **Physics** — AABB/Sphere/OBB narrowphase, rigidbody dynamics, collision response, raycasting, character controller
- **Input** — Win32 raw input, XInput gamepad
- **Asset Pipeline** — DDS/WIC texture loading, Assimp mesh import
- **Lighting** — Directional + point + spot lights, all with shadow support

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

The build produces `build/Game/Debug/TestGame.exe`. The exe auto-detects the project root — just double-click it or run from any directory. Shaders and assets are loaded directly from source (`Engine/Shaders/`, `Assets/`).

**Shader/asset iteration**: Edit any `.hlsl` or asset file and re-run the exe — no rebuild needed. Only C++ changes require a build.

## Dependencies (via vcpkg)

| Library | Purpose |
|---------|---------|
| assimp | Mesh import (FBX, glTF, OBJ) |
| imgui | Debug UI (DX11 + Win32 backend) |
| directxtex | DDS/HDR texture loading, BC compression |
| nlohmann-json | Scene descriptor serialization |

## Project Structure

```
FoxEngine/
├── Engine/              # Static library (FoxEngine.lib)
│   ├── Shaders/         # HLSL shaders (loaded directly at runtime)
│   ├── include/Engine/  # Public headers (SE:: namespace)
│   │   ├── Core/        # Platform, logging, clock
│   │   ├── Renderer/    # Graphics pipeline, mesh, materials, post-processing
│   │   ├── Physics/     # Collision, dynamics, raycasting
│   │   ├── Input/       # Keyboard, mouse, gamepad
│   │   ├── Scene/       # Entity, components, camera, scene loading
│   │   └── Assets/      # Asset manager, resource cache
│   └── src/             # Implementation (mirrors include/)
├── Game/                # Test executable (integration target)
├── Assets/              # Runtime assets (textures, models, scenes)
│   ├── Scenes/          # JSON scene descriptors
│   └── Particles/       # Particle textures (flipbooks, smoke, etc.)
└── Tools/               # Build-time utilities (texture converter)
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
