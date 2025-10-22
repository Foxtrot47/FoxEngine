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

    struct PointLight
    {
        DirectX::XMFLOAT3 position;
        float range;
        DirectX::XMFLOAT3 color;
        float intensity;
    };

    struct DirectionalLight
    {
        DirectX::XMFLOAT3 direction;
        float padding0;
        DirectX::XMFLOAT3 color;
        float intensity;
    };
    
    struct LightBuffer
    {
        DirectionalLight directionalLight;
        PointLight pointLights[MAX_LIGHTS];
        DirectX::XMFLOAT3 ambientLight;
        int activePointLightCount;
        float globalSpecularIntensity;
        int hasDirectionalLight;
        DirectX::XMFLOAT2 padding;
    };

    struct LightShadowMatrices
    {
        DirectX::XMMATRIX directionalLightViewProj;
        DirectX::XMMATRIX lightViewProj[5];
    };

    LightManager(Graphics& gfx);

    int AddPointLight(const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& color, float intensity = 1.0f, float range = 100.0f);
    void UpdatePointLight(int lightIndex, PointLight& lightData);
    void SetDirectionalLight(const DirectX::XMFLOAT3& direction, const DirectX::XMFLOAT3& color, float intensity = 1.0f);
    void UpdateDirectionLight(DirectionalLight& lightData);

    DirectX::XMMATRIX CalculateDirectionalLightMatrix();
    DirectX::XMMATRIX CalculatePointLightMatrix(int lightIndex);

    int GetActiveLights() const;
    bool HasDirectionalLight() const;
    void Bind(Graphics& gfx) const;
    void Update(Graphics& gfx);

private:
    LightBuffer lightBuffer;
    LightShadowMatrices lightMatrices;
    std::unique_ptr<PixelConstantBuffer<LightBuffer>> lightCBuff;
    std::unique_ptr<PixelConstantBuffer<LightShadowMatrices>> lightMatrixCBuff;
    bool isDirty;
};
