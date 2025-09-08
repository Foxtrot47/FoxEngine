#pragma once
#include "ConstantBuffer.h"
#include "Graphics.h"

class LightManager
{
public:
    static const unsigned int MAX_LIGHTS = 5;
    enum class LightType : int
    {
        POINT = 0,
        DIRECTIONAL = 1
    };

    struct Light
    {
        DirectX::XMFLOAT3 position;
        int type;
        DirectX::XMFLOAT3 direction;
        float range;
        DirectX::XMFLOAT3 color;
        float intensity;
    };
    
    struct LightBuffer
    {
        Light lights[MAX_LIGHTS];
        DirectX::XMFLOAT3 ambientLight;
        int activeLightCount;
        float globalSpecularIntensity;
        DirectX::XMFLOAT3 padding;
    };

    struct LightShadowMatrices
    {
        DirectX::XMMATRIX lightViewProj;
    };

    LightManager(Graphics& gfx);

    int AddPointLight(const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& color, float intensity = 1.0f, float range = 100.0f);
    int AddDirectionalLight(const DirectX::XMFLOAT3& direction, const DirectX::XMFLOAT3& color, float intensity = 1.0f);
    void UpdateLight(int lightIndex, Light lightData);

    DirectX::XMMATRIX CalculateLightMatrix(int lightIndex);

    void Bind(Graphics& gfx) const;
    void Update(Graphics& gfx);

private:
    LightBuffer lightBuffer;
    LightShadowMatrices lightMatrices;
    std::unique_ptr<PixelConstantBuffer<LightBuffer>> lightCBuff;
    std::unique_ptr<PixelConstantBuffer<LightShadowMatrices>> lightMatrixCBuff;
    bool isDirty;
};
