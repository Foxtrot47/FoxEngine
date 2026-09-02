# FSR 3.1.5 upscaler — DirectX 11 backend

Prebuilt static libraries and public headers for AMD FidelityFX Super
Resolution 3.1.5 upscaling with a community DirectX 11 backend.

- Source: https://github.com/alandtse/FidelityFX-SDK-DX11 (MIT), a fork of
  https://github.com/metarutaiga/FidelityFX-SDK-DX11 adding an automated
  DX11 build to AMD's FidelityFX SDK.
- AMD's official FSR 3.1 supports only DX12/Vulkan; the DX11 backend here is
  community-maintained (also used by OptiScaler).
- Upscaling only. Frame generation is not available on D3D11.

## Contents

- `include/` — public FidelityFX host headers (`sdk/include` from the fork)
- `lib/ffx_backend_dx11_x64(.d).lib` — DX11 backend + embedded shader blobs
- `lib/ffx_fsr3upscaler_x64(.d).lib` — FSR 3.1 upscaler host component
- `LICENSE.txt` — MIT

`*d.lib` variants are Debug (/MDd); the others are Release (/MD).

## Rebuilding

```
git clone https://github.com/alandtse/FidelityFX-SDK-DX11 fsr3-dx11
cd fsr3-dx11/sdk && mkdir build && cd build
cmake -A x64 .. -DFFX_API_CUSTOM=OFF -DFFX_API_VK=OFF -DFFX_API_DX12=OFF ^
  -DFFX_ALL=OFF -DFFX_API_DX11=ON -DFFX_FSR=ON -DFFX_FSR3UPSCALER=ON ^
  -DFFX_FSR1=OFF -DFFX_FSR2=OFF -DFFX_FI=OFF -DFFX_OF=OFF ^
  -DFFX_AUTO_COMPILE_SHADERS=1
cmake --build . --config Release --parallel 4
cmake --build . --config Debug --parallel 4
```

Libs land in `sdk/bin/ffx_sdk/`; copy them plus `sdk/include` here.
Note: `-DFFX_FSR3UPSCALER=ON` is required — the stock
`BuildFidelityFXDX11.bat` only sets `FFX_FSR` and omits the upscaler
component library.

## Local patches

- `include/FidelityFX/host/backends/dx11/ffx_dx11.h`:
  `ffxGetResourceDX11_Fsr31` is declared with C++ linkage and a non-const
  `ID3D11Resource*` to match the mangled symbol actually present in the
  prebuilt libraries (upstream header/impl mismatch).
