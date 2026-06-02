# FoxEngine

A simple DirectX 11 rendering engine built on Windows with C++17. Features a physically-based forward renderer, real-time shadows, post-processing pipeline, custom physics, and ImGui debug tooling.

## Features

### Rendering
- **PBR Forward Pipeline** — Cook-Torrance BRDF
- **Image-Based Lighting** — Equirectangular HDR panorama sampling for diffuse irradiance + specular reflections
- **Shadows** — Directional light shadow maps with PCF, point light cube shadows
- **Post-Processing** — HDR rendering, ACES/Reinhard tone mapping
- **Screen-Space Effects** — SSR , SSAO
- **Alpha Support** — Alpha test for foliage, alpha blending for transparent materials
- **Render Queue** — Front-to-back opaque, back-to-front transparent, frustum culling

### Engine Systems
- **Scene Management** — Entity/component system, scene graph with parent-child transforms, JSON scene descriptors
- **Physics** — AABB/Sphere/OBB narrowphase, rigidbody dynamics, collision response, raycasting, character controller
- **Input** — Win32 raw input, XInput gamepad
- **Asset Pipeline** — DDS/WIC texture loading, Assimp mesh import

## Requirements

- Windows 10+ (x64)
- Visual Studio 2022 (MSVC v143 toolchain)
- CMake 3.20+
- [vcpkg](https://github.com/microsoft/vcpkg) (refer microsoft docs on how to setup)

## Building

cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug
```

The build produces `build/Game/Debug/TestGame.exe`. Run from the build directory — shaders and assets are copied automatically.

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
│   ├── Shaders/         # HLSL shaders (copied to build at compile time)
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
│   └── Scenes/          # JSON scene descriptors
└── Tools/               # Build-time utilities (texture converter)
```

## Scene Format

Scenes are defined as JSON files in `Assets/Scenes/`. Example:

```json
{
  "name": "My Scene",
  "mesh": {
    "path": "Assets/Models/scene.fbx",
    "alphaCutoff": 0.5,
    "transform": { "position": [0, 0, 0], "rotation": [0, 0, 0], "scale": 1.0 }
  },
  "skybox": "Assets/Textures/sky.dds",
  "camera": { "eye": [0, 5, -10], "yaw": 0, "pitch": 0 },
  "sun": { "elevation": 60, "azimuth": -135, "intensity": 1.0 },
  "bloom": { "enabled": true, "threshold": 1.0, "intensity": 0.1 },
  "pointLights": [
    { "position": [5, 3, 0], "color": [1, 0.8, 0.5], "radius": 50, "castShadow": true }
  ]
}
```

## License

MIT License — see [LICENSE](LICENSE) for details.
