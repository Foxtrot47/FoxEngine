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

    // Layered terrain inputs (MaterialKind::Terrain; Basic.hlsl FOX_TERRAIN permutation).
    struct TerrainMat
    {
        AssetHandle<Texture2D> layerAlbedo[k_maxTerrainLayers]; // t16-t19 (sRGB); nullptr → default white
        AssetHandle<Texture2D> layerNormal[k_maxTerrainLayers]; // t22-t25; nullptr → flat
        AssetHandle<Texture2D> splat;    // t20; nullptr → default black (layer A only)
        AssetHandle<Texture2D> overlay;  // t21 (sRGB); nullptr → no overlay
        float   layerScale[k_maxTerrainLayers] = { 1.0f, 1.0f, 1.0f, 1.0f };
        uint8_t layerCount = 1;
        bool    albedoAlphaIsRoughness = false;
        uint8_t splatUVSet = 0;
        uint8_t overlayUVSet = 0;
        float   splatUVScale[2] = { 1.0f, 1.0f };
        float   overlayUVScale[2] = { 1.0f, 1.0f };
        float   overlayStrength = 0.0f;
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
        float     detailAlbedoNeutral = k_defaultDetailAlbedoNeutral;
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
        MaterialKind kind = MaterialKind::Standard;
        TerrainMat terrain; // kind == Terrain: replaces albedo/roughness/normal; roughnessScale applies
        AssetHandle<Texture2D> clearcoatTex;          // t26; nullptr → not sampled
        AssetHandle<Texture2D> clearcoatRoughnessTex; // t27; nullptr → not sampled
        SlotTransform clearcoatSlot;
        SlotTransform clearcoatRoughnessSlot;
        float     clearcoat = 0.0f;
        float     clearcoatRoughness = 0.0f;
        bool      clearcoatMaskFromAlbedoAlpha = false;
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

    // Staggered cutout coverage (b4): alpha-tested pixels blend the threshold
    // with interleaved gradient noise by up to width, animated by frameIndex.
    // Use a non-zero width only while a temporal resolve (TAA or FSR) is active.
    void SetCutoutDither(uint32_t frameIndex, float width);

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
    // Culls the whole mesh, then each submesh, against the view frustum; each
    // submesh sorts by the view depth of its own bounds centre.
    void SubmitMesh(const Mesh& mesh, DirectX::XMMATRIX model,
                    const std::vector<SubMat>& mats, bool transparent = false,
                    const DirectX::XMMATRIX* prevModel = nullptr);
    // Opaque and cutout submeshes submitted at least k_minInstanceGroup times with
    // identical GPU bindings are drawn first as one DrawIndexedInstanced each;
    // the rest follow in sorted order.
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

    // Draws issued since the last Begin() (one per instanced group).
    uint32_t GetLastDrawCalls() const { return m_lastDrawCalls; }
    uint32_t GetLastCulledCount() const { return m_lastCulled; }
    // Instanced draws issued since the last Begin() and the submeshes they covered.
    uint32_t GetLastInstancedDraws() const { return m_lastInstancedDraws; }
    uint32_t GetLastInstancedItems() const { return m_lastInstancedItems; }

    static constexpr uint32_t k_minInstanceGroup = 4;

private:
    struct ForwardShadowCBData
    {
        int   numPointShadowCasters;
        float pointShadowBias;
        float frameIndex;        // dither animation step for cutout coverage
        float cutoutDitherWidth; // 0 disables the dithered cutout threshold
    };

    struct TransformCBData
    {
        DirectX::XMFLOAT4X4 model;
        DirectX::XMFLOAT4X4 view;
        DirectX::XMFLOAT4X4 projection;
        DirectX::XMFLOAT4X4 prevModel;         // last frame's model matrix
        DirectX::XMFLOAT4X4 viewProjNoJitter;  // current frame, no TAA jitter
        DirectX::XMFLOAT4X4 prevViewProj;      // last frame, no TAA jitter
        uint32_t instanceBase;  // first InstanceData of an instanced draw
        uint32_t useInstances;  // 1: Model/PrevModel come from the t29 instance buffer
        uint32_t pad[2];
    };
    // Element of the VS t29 instance buffer (Basic.hlsl InstanceData).
    struct InstanceData
    {
        DirectX::XMFLOAT4X4 model;
        DirectX::XMFLOAT4X4 prevModel;
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
        DirectX::XMFLOAT4 tintParams = { 0.0f, 0.0f, 1.0f, 4.59479f }; // x = VertexTint (0..2), y = DetailBlend (0..2), z = detailAlbedoStrength, w = 1 / detailAlbedoNeutral
        DirectX::XMFLOAT4 terrainScale = { 1.0f, 1.0f, 1.0f, 1.0f }; // world X/Z scale of layers A-D
        DirectX::XMFLOAT4 terrainParams;  // x layerCount, y albedoAlphaIsRoughness, z overlayStrength, w overlay uvSet
        DirectX::XMFLOAT4 terrainParams2; // x splat uvSet
        DirectX::XMFLOAT4 terrainST = { 1.0f, 1.0f, 1.0f, 1.0f }; // xy splat uv scale, zw overlay uv scale
        DirectX::XMFLOAT4 clearcoat = { 0.0f, 0.0f, 0.0f, 0.0f }; // x factor, y roughness, z maskFromAlbedoAlpha, w hasTextures (1 coat, 2 rough)
        DirectX::XMFLOAT4 clearcoatST[2] = { { 1, 1, 0, 0 }, { 1, 1, 0, 0 } }; // 0 coat, 1 coat roughness: xy scale, zw offset
        DirectX::XMFLOAT4 clearcoatUV = { 0.0f, 0.0f, 0.0f, 0.0f }; // x coat uv set, y coat-roughness uv set
    };
    static_assert(sizeof(MaterialParamsCBData) == 368, "MaterialParamsCBData must match Basic.hlsl MaterialCB");
    struct PlanarReflectionCBData
    {
        DirectX::XMFLOAT4X4 reflectionViewProj;
        float planeY;
        float planarStrength;
        float planarEnabled;
        float clipEnabled;
    };

    // Everything BindSubMaterial sends to the GPU for one submesh: the MaterialCB
    // contents, the texture at each material register (null = left unbound) and the
    // alpha mode and material kind that select blend/raster state and the shader
    // permutation. Instancing groups draws whose bindings compare equal.
    static constexpr uint32_t k_materialTextureCount = 20;
    struct MaterialBinding
    {
        MaterialParamsCBData cb;
        const Texture2D*     textures[k_materialTextureCount];
        AlphaMode            alphaMode;
        MaterialKind         kind;
        bool operator==(const MaterialBinding& other) const;
    };

    // Stored per-submit for Flush() to reference.
    struct QueuedDraw
    {
        const Mesh*              mesh;
        const std::vector<SubMat>* mats;
    };

    // Flush() scratch: an opaque/cutout queue item and the index of its binding.
    struct InstanceCandidate
    {
        const Mesh*    mesh;
        const SubMat*  mat;
        uint32_t       subMesh;
        uint32_t       item;     // index into the sorted queue
        uint32_t       binding;  // index into m_bindings
    };
    struct InstanceGroup
    {
        uint32_t firstInstance; // into m_instanceData
        uint32_t count;
        uint32_t item;          // a member, for mesh and submesh
        uint32_t binding;       // into m_bindings
        float    depth;         // nearest member's sort depth
    };

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>  m_ps;
    ShaderPermutation                          m_terrainPerm; // Basic.hlsl with FOX_TERRAIN=1
    Microsoft::WRL::ComPtr<ID3D11InputLayout>  m_layout;
    Microsoft::WRL::ComPtr<ID3D11Buffer>       m_lineBuffer;

    ConstantBuffer<TransformCBData>           m_transformCB;
    void ApplyAlphaModeState(ID3D11DeviceContext* ctx, AlphaMode mode);
    void BuildMaterialBinding(const SubMat& mat, MaterialBinding& out) const;
    void BindMaterial(ID3D11DeviceContext* ctx, const MaterialBinding& binding);
    void BindSubMaterial(ID3D11DeviceContext* ctx, const SubMat& mat);
    void UploadTransformCB(ID3D11DeviceContext* ctx, DirectX::XMMATRIX model);
    void UploadTransformCB(ID3D11DeviceContext* ctx, DirectX::XMMATRIX model,
                           DirectX::XMMATRIX prevModel);
    void UploadInstancedTransformCB(ID3D11DeviceContext* ctx, uint32_t instanceBase);
    // Groups the sorted queue's repeated opaque/cutout submeshes and draws each
    // group instanced; marks their items in m_instanced.
    void DrawInstancedGroups(ID3D11DeviceContext* ctx, AlphaMode& boundMode, MaterialKind& boundKind);
    void ApplyMaterialState(ID3D11DeviceContext* ctx, AlphaMode mode, MaterialKind kind,
                            AlphaMode& boundMode, MaterialKind& boundKind);
    bool UploadInstances(ID3D11DeviceContext* ctx);

    ConstantBuffer<MaterialParamsCBData>      m_materialCB;
    DirectX::XMFLOAT3 m_globalTint = { 1.0f, 1.0f, 1.0f };
    float m_globalRoughnessScale = 1.0f;
    float m_globalMetallic = 1.0f;
    float m_globalDebugShadow = 0.0f;
    ConstantBuffer<ForwardShadowCBData>       m_shadowCB;
    uint32_t                                  m_cutoutFrameIndex  = 0;
    float                                     m_cutoutDitherWidth = 0.0f;
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
    uint32_t                 m_lastInstancedDraws = 0;
    uint32_t                 m_lastInstancedItems = 0;

    // Instancing (Flush scratch, reused across frames) and the VS t29 buffer.
    std::vector<InstanceCandidate>  m_candidates;
    std::vector<MaterialBinding>    m_bindings;
    std::vector<InstanceGroup>      m_groups;
    std::vector<InstanceData>       m_instanceData;
    std::vector<uint8_t>            m_instanced; // per sorted queue item
    Microsoft::WRL::ComPtr<ID3D11Buffer>             m_instanceBuffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_instanceSRV;
    uint32_t                                         m_instanceCapacity = 0;
};

} // namespace SE
