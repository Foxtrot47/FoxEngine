#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <unordered_map>

namespace SE {

struct ShaderDefine
{
    std::string name;
    std::string value;
};

struct ShaderPermutation
{
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>  ps;
    Microsoft::WRL::ComPtr<ID3DBlob>           vsBlob;
};

class ShaderLibrary
{
public:
    void Init(ID3D11Device* device);

    // Compile and cache a VS+PS permutation keyed by (file + defines).
    // Returns nullptr on compilation failure.
    const ShaderPermutation* Get(const std::wstring& hlslFile,
                                  const std::vector<ShaderDefine>& defines = {});

    // Compile and cache a compute shader. Returns nullptr on failure.
    ID3D11ComputeShader* GetCS(const std::wstring& hlslFile,
                               const char* entryPoint = "CS_Main",
                               const std::vector<ShaderDefine>& defines = {});

    // Remove all cached permutations (e.g. for hot-reload).
    void Clear();

private:
    std::string MakeKey(const std::wstring& file,
                        const std::vector<ShaderDefine>& defines) const;

    ID3D11Device* m_device = nullptr;
    std::unordered_map<std::string, ShaderPermutation> m_cache;
    std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D11ComputeShader>> m_csCache;
};

} // namespace SE
