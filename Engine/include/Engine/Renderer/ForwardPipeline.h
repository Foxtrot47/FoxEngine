#pragma once
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <vector>
#include "Engine/Assets/AssetManager.h"
#include "Engine/Renderer/VertexBuffer.h"
#include "Engine/Renderer/IndexBuffer.h"
#include "Engine/Renderer/ConstantBuffer.h"
#include "Engine/Renderer/SamplerState.h"
#include "Engine/Renderer/ShaderLibrary.h"
#include "Engine/Renderer/RenderQueue.h"
#include "Engine/Renderer/Frustum.h"
#include "Engine/Renderer/Mesh.h"

namespace SE {

class ForwardPipeline
{
public:
    enum class ViewShading { Lit, Unlit, Wireframe };
    ViewShading viewShading = ViewShading::Lit; // transient viewport override

    // SubMat::slots order (matches MaterialCB SlotST / SlotUVA / SlotUVB in Basic.hlsl).
    enum Slot : uint32_t
    {
        SlotAlbedo, SlotNormal, SlotRoughness, SlotEmissive,
        SlotOcclusion, SlotDetailAlbedo, SlotDetailNormal, SlotCount
    };

    // Per-texture UV set and tiling (uv * scale + offset).
    struct SlotTransform
    {
        uint8_t uvSet = 0;
        float   scale[2] = { 1.0f, 1.0f };
        float   offset[2] = { 0.0f, 0.0f };
    };

    struct SubMat
    {
        AssetHandle<Texture2D> albedo;
        AssetHandle<Texture2D> normal;
        AssetHandle<Texture2D> roughness;
        AssetHandle<Texture2D> metallic;   // t7; nullptr → default black (0 = dielectric)
        AssetHandle<Texture2D> emissive;   // t9; nullptr → default black (no emission)
        AssetHandle<Texture2D> occlusion;  // t13; nullptr → default white (no occlusion)
        AssetHandle<Texture2D> detailAlbedo; // t14 (sRGB); nullptr → not sampled
        AssetHandle<Texture2D> detailNormal; // t15; nullptr → not sampled
        SlotTransform slots[SlotCount];      // t7 (metallic) follows SlotRoughness
        float     occlusionStrength = 1.0f;
        uint8_t   occlusionChannel = 0;      // 0 = R, 1 = G
        DetailBlend detailAlbedoBlend = DetailBlend::Overlay;
        float     detailAlbedoStrength = 1.0f;
        float     detailNormalStrength = 1.0f;
        AlphaMode alphaMode  = AlphaMode::Opaque;
        float     alphaCutoff = 0.5f;
        MaterialWorkflow workflow = MaterialWorkflow::SeparateMR; // t1 interpretation
        float     roughnessScale = 1.0f;   // material roughness factor (scalar workflows)
        float     glossinessFactor = 1.0f; // spec/gloss: multiplier on t1.a
        bool      receiveShadows = true;
        DirectX::XMFLOAT3 tint = { 1.0f, 1.0f, 1.0f }; // multiplies the albedo (linear RGB)
        DirectX::XMFLOAT3 emissiveColor = { 1.0f, 1.0f, 1.0f }; // multiplies the emissive map (linear HDR)
        VertexTint vertexTint = VertexTint::None;
    };

    bool Init(ID3D11Device* device, AssetManager& assets, ShaderLibrary& shaders);

    // Bind shaders + shared pipeline state; cache view/proj for this frame.
    // Also binds a default ForwardShadowCB (b4) with zero point shadow casters.
    void Begin(ID3D11DeviceContext* ctx, DirectX::XMMATRIX view, DirectX::XMMATRIX proj);

    // Matrices for motion vector output (TAA). Call after Begin(). Both must be
    // jitter-free; without this call velocity resolves to zero.
    void SetTemporalMatrices(DirectX::XMMATRIX prevViewProj,
                             DirectX::XMMATRIX viewProjNoJitter);

    // Bind equirectangular HDR panorama for IBL (t4). Pass nullptr to unbind.
    void BindEnvironment(ID3D11DeviceContext* ctx,
                         ID3D11ShaderResourceView* irradianceSRV,
                         ID3D11ShaderResourceView* specularSRV,
                         ID3D11ShaderResourceView* brdfLutSRV);
    void SetReflectionPass(ID3D11DeviceContext* ctx, bool enabled, float planeY = 0.0f);
    void BindPlanarReflection(ID3D11DeviceContext* ctx,
                              ID3D11ShaderResourceView* reflectionSRV,
                              DirectX::XMMATRIX reflectionViewProj,
                              float planeY, float strength);
    void ClearPlanarReflection(ID3D11DeviceContext* ctx);

    // Bind point shadow cube maps (t5-t6) + ForwardShadowCB (b4). Pass nullptr SRVs if unused.
    void BindPointShadows(ID3D11DeviceContext* ctx,
                          ID3D11ShaderResourceView* psrv0,
                          ID3D11ShaderResourceView* psrv1,
                          int numCasters, float bias);

    // Upload and bind MaterialParamsCB (b3).
    void SetMaterialParams(ID3D11DeviceContext* ctx,
                           DirectX::XMFLOAT3 tint, float roughnessScale, float metallic,
                           float debugShadow = 0.0f, float emissiveIntensity = 1.0f,
                           DirectX::XMFLOAT3 emissiveColor = { 1.f, 1.f, 1.f });

    // Global overrides applied on top of per-submesh material values during Flush().
    // tint replaces AlbedoTint, roughnessScale/metallic multiply texture values,
    // debugShadow enables the shadow-factor debug view.
    void SetGlobalMaterialParams(DirectX::XMFLOAT3 tint, float roughnessScale,
                                 float metallic, float debugShadow);

    // --- Queued rendering (M42) ---
    // Submit a mesh for sorted draw. Call Flush() after all submits to actually draw.
    // prevModel: last frame's model matrix for per-object motion vectors;
    // nullptr means the object did not move (velocity from camera only).
    void SubmitMesh(const Mesh& mesh, DirectX::XMMATRIX model,
                    const std::vector<SubMat>& mats, bool transparent = false,
                    const DirectX::XMMATRIX* prevModel = nullptr);
    void Flush(ID3D11DeviceContext* ctx);

    // --- Immediate rendering ---
    void DrawSphere(ID3D11DeviceContext* ctx,
                    DirectX::XMFLOAT3 position, float radius, DirectX::XMFLOAT3 tint,
                    const DirectX::XMMATRIX* prevModel = nullptr);

    // Debug wire draws — unlit flat color, LINELIST topology.
    void DrawWireSphere(ID3D11DeviceContext* ctx,
                        DirectX::XMFLOAT3 position, float radius, DirectX::XMFLOAT3 color);
    void DrawWireAABB(ID3D11DeviceContext* ctx,
                      DirectX::XMFLOAT3 min, DirectX::XMFLOAT3 max, DirectX::XMFLOAT3 color);
    void DrawWireDisc(ID3D11DeviceContext* ctx,
                      DirectX::XMFLOAT3 center, float radius, DirectX::XMFLOAT3 color);
    void DrawWireBox(ID3D11DeviceContext* ctx,
                     DirectX::XMMATRIX world, DirectX::XMFLOAT3 color);
    void DrawLine(ID3D11DeviceContext* ctx,
                  DirectX::XMFLOAT3 from, DirectX::XMFLOAT3 to, DirectX::XMFLOAT3 color);
    // Triangle edges of the mesh's opaque submeshes (what a static mesh
    // collider is built from), drawn over the shaded surface.
    void DrawWireMesh(ID3D11DeviceContext* ctx, const Mesh& mesh,
                      DirectX::XMMATRIX world, DirectX::XMFLOAT3 color);

    // Draw a sphere with explicit PBR textures and a metallic override scalar.
    // Uses the SubMat's albedo/normal/roughness textures. Set metallic per-sphere via SetMaterialParams.
    void DrawPBRSphere(ID3D11DeviceContext* ctx,
                       DirectX::XMFLOAT3 position, float radius,
                       const SubMat& mat, float metallic, float roughnessScale = 1.0f,
                       DirectX::XMFLOAT3 tint = { 1.f, 1.f, 1.f },
                       float emissiveIntensity = 1.0f,
                       DirectX::XMFLOAT3 emissiveColor = { 1.f, 1.f, 1.f },
                       const DirectX::XMMATRIX* prevModel = nullptr);

    // Draw a horizontal unit plane scaled to the given half-extents, with PBR textures.
    void DrawPBRPlane(ID3D11DeviceContext* ctx,
                      DirectX::XMFLOAT3 center, float halfSizeX, float halfSizeZ,
                      const SubMat& mat, float metallic, float roughnessScale = 1.0f,
                      DirectX::XMFLOAT3 tint = { 1.f, 1.f, 1.f },
                      float emissiveIntensity = 1.0f,
                      DirectX::XMFLOAT3 emissiveColor = { 1.f, 1.f, 1.f },
                      const DirectX::XMMATRIX* prevModel = nullptr);

    uint32_t GetLastDrawCalls() const { return m_lastDrawCalls; }
    uint32_t GetLastCulledCount() const { return m_lastCulled; }

private:
    struct ForwardShadowCBData
    {
        int   numPointShadowCasters;
        float pointShadowBias;
        float _pad[2];
    };

    struct TransformCBData
    {
        DirectX::XMFLOAT4X4 model;
        DirectX::XMFLOAT4X4 view;
        DirectX::XMFLOAT4X4 projection;
        DirectX::XMFLOAT4X4 prevModel;         // last frame's model matrix
        DirectX::XMFLOAT4X4 viewProjNoJitter;  // current frame, no TAA jitter
        DirectX::XMFLOAT4X4 prevViewProj;      // last frame, no TAA jitter
    };
    // Field-for-field mirror of Basic.hlsl MaterialCB (b3). Defaults are the neutral
    // material: identity slot transforms on UV0, no occlusion or detail maps.
    struct MaterialParamsCBData
    {
        DirectX::XMFLOAT3 albedoTint; float roughnessScale;
        float metallic; float unlit; float debugShadow; float alphaCutoff;
        float emissiveIntensity; DirectX::XMFLOAT3 emissiveColor;
        float workflow; float glossinessFactor; float receiveShadows; float occlusionStrength = 1.0f;
        DirectX::XMFLOAT4 slotST[SlotCount] = { // xy scale, zw offset
            { 1, 1, 0, 0 }, { 1, 1, 0, 0 }, { 1, 1, 0, 0 }, { 1, 1, 0, 0 },
            { 1, 1, 0, 0 }, { 1, 1, 0, 0 }, { 1, 1, 0, 0 } };
        DirectX::XMFLOAT4 slotUVA;   // UV set of slots 0-3
        DirectX::XMFLOAT4 slotUVB;   // UV set of slots 4-6, w = detail normal strength
        DirectX::XMFLOAT4 slotFlags; // x hasOcclusion, y hasDetailAlbedo, z hasDetailNormal, w occlusionChannel
        DirectX::XMFLOAT4 tintParams = { 0.0f, 0.0f, 1.0f, 0.0f }; // x = VertexTint (0..2), y = DetailBlend (0..2), z = detailAlbedoStrength
    };
    static_assert(sizeof(MaterialParamsCBData) == 240, "MaterialParamsCBData must match Basic.hlsl MaterialCB");
    struct PlanarReflectionCBData
    {
        DirectX::XMFLOAT4X4 reflectionViewProj;
        float planeY;
        float planarStrength;
        float planarEnabled;
        float clipEnabled;
    };

    // Stored per-submit for Flush() to reference.
    struct QueuedDraw
    {
        const Mesh*              mesh;
        const std::vector<SubMat>* mats;
    };

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>  m_ps;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>  m_layout;
    Microsoft::WRL::ComPtr<ID3D11Buffer>       m_lineBuffer;

    ConstantBuffer<TransformCBData>           m_transformCB;
    void ApplyAlphaModeState(ID3D11DeviceContext* ctx, AlphaMode mode);
    void BindSubMaterial(ID3D11DeviceContext* ctx, const SubMat& mat);
    void UploadTransformCB(ID3D11DeviceContext* ctx, DirectX::XMMATRIX model);
    void UploadTransformCB(ID3D11DeviceContext* ctx, DirectX::XMMATRIX model,
                           DirectX::XMMATRIX prevModel);

    ConstantBuffer<MaterialParamsCBData>      m_materialCB;
    DirectX::XMFLOAT3 m_globalTint = { 1.0f, 1.0f, 1.0f };
    float m_globalRoughnessScale = 1.0f;
    float m_globalMetallic = 1.0f;
    float m_globalDebugShadow = 0.0f;
    ConstantBuffer<ForwardShadowCBData>       m_shadowCB;
    ConstantBuffer<PlanarReflectionCBData>    m_planarCB;
    SamplerState                              m_sampler;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_cubeSampler;
    VertexBuffer                         m_sphereVB;
    IndexBuffer                          m_sphereIB;
    VertexBuffer                         m_wireSphereVB;
    IndexBuffer                          m_wireSphereIB;
    VertexBuffer                         m_wireAABBVB;
    IndexBuffer                          m_wireAABBIB;
    VertexBuffer                         m_planeVB;
    IndexBuffer                          m_planeIB;

    AssetHandle<Texture2D> m_defaultWhite;
    AssetHandle<Texture2D> m_defaultBlack;
    AssetHandle<Texture2D> m_defaultNormal;

    Microsoft::WRL::ComPtr<ID3D11BlendState>      m_alphaBlend;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>  m_noCullRS;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>  m_wireframeRS;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>  m_wireOverlayRS;

    DirectX::XMMATRIX m_view = {};
    DirectX::XMMATRIX m_proj = {};
    DirectX::XMMATRIX m_viewProjNoJitter = DirectX::XMMatrixIdentity();
    DirectX::XMMATRIX m_prevViewProj = DirectX::XMMatrixIdentity();
    Frustum                  m_frustum;

    RenderQueue              m_queue;
    std::vector<QueuedDraw>  m_queuedDraws;
    uint32_t                 m_lastDrawCalls = 0;
    uint32_t                 m_lastCulled    = 0;
};

} // namespace SE
