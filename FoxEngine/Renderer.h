#pragma once
#include "Graphics.h"
#include <unordered_map>
#include <vector>
#include <DirectXMath.h>
#include "FPVCamera.h"
#include "SceneManager.h"
#include "ConstantBuffer.h"

class Material;
class MeshNode;
class Mesh;

using Microsoft::WRL::ComPtr;

// Forward declarations for batching
using MeshTransformPair = std::pair<Mesh*, DirectX::XMMATRIX>;

class Renderer
{
private:
	struct ShadowConstants {
		DirectX::XMMATRIX modelLightViewProjection;
	};
	static const uint32_t SHADOW_MAP_SIZE = 8192;

	Graphics& gfx;
	FPVCamera& camera;
	ComPtr<ID3D11RenderTargetView> pBackBufferRTV = nullptr;
	ComPtr<ID3D11Texture2D> pBackBuffer = nullptr;
	ComPtr<ID3D11RasterizerState> pRasterizerState = nullptr;

	ComPtr<ID3D11DepthStencilState> pDepthStencilState = nullptr;
	ComPtr<ID3D11DepthStencilView> pDepthStencilView = nullptr;
	ComPtr<ID3D11Texture2D> pDepthStencilTexture = nullptr;

	ComPtr<ID3D11VertexShader> pPhongVS = nullptr;
	ComPtr<ID3D11PixelShader> pPhongPS = nullptr;
	ComPtr<ID3D11InputLayout> pInputLayout = nullptr;
	ComPtr<ID3D11SamplerState> linearSampler;

	// Shader cache for material overrides
	std::unordered_map<std::wstring, ComPtr<ID3D11VertexShader>> vertexShaderCache;
	std::unordered_map<std::wstring, ComPtr<ID3D11PixelShader>> pixelShaderCache;
	std::unordered_map<std::wstring, ComPtr<ID3D11InputLayout>> inputLayoutCache;

	// Currently bound shader tracking
	std::wstring currentVertexShaderPath;
	std::wstring currentPixelShaderPath;

	ComPtr<ID3D11Buffer> frameConstantBuffer = nullptr;
	ComPtr<ID3D11Buffer> objectConstantBuffer = nullptr;
	ComPtr<ID3D11Buffer> lightBuffer = nullptr;
	std::unique_ptr<VertexConstantBuffer<ShadowConstants>> shadowConstantBuffer = nullptr;

	ComPtr<ID3D11DepthStencilView> pDirectionalShadowDepthView;
	ComPtr<ID3D11ShaderResourceView> pDirectionalShadowSRV;
	ComPtr<ID3D11Texture2D> pDirectionalShadowTexture;
	std::array<ComPtr<ID3D11Texture2D>, 5> pPointShadowTexture;
	std::array<ComPtr<ID3D11DepthStencilView>, 5> pPointShadowDepthView;
	std::array<ComPtr<ID3D11ShaderResourceView>, 5> pointShadowMapSRVs;
	ComPtr<ID3D11RenderTargetView> pShadowRTV = nullptr;
	ComPtr<ID3D11SamplerState> pShadowSamplerState = nullptr;

	ComPtr<ID3D11VertexShader> pShadowVS = nullptr;
	ComPtr<ID3D11InputLayout> pShadowInputLayout = nullptr;
	ComPtr<ID3D11Buffer> pShadowConstantBuffer = nullptr;
	ComPtr<ID3D11SamplerState> pComparisonSampler = nullptr;

	ComPtr<ID3D11RasterizerState> pShadowRasterizerState = nullptr;
	ComPtr<ID3D11DepthStencilState> pShadowDepthStencilState = nullptr;

	Microsoft::WRL::ComPtr<ID3DBlob> pShaderBlob;

	D3D11_VIEWPORT renderViewPort = {};
	D3D11_VIEWPORT shadowViewPort = {};

	std::vector<MeshNode*> nodes;
	bool initialized = false;

	std::unordered_map<Material*, std::vector<MeshTransformPair>> materialBatches;

	std::wstring phongPS = L"PhongMultiLightsPS.cso";
	std::wstring phongVS = L"PhongVS.cso";
public:
	Renderer(
		Graphics& gfx,
		FPVCamera& camera,
		int windowWidth,
		int windowHeight
	);
	~Renderer() = default;
	Renderer() = delete;
	Renderer(const Renderer&) = delete;
	Renderer& operator= (const Renderer&) = delete;
	void CollectFrameData(SceneNode& node);

	void BeginFrame(float red, float green, float blue);
	void RenderScene();
	void EndFrame();
	void BindShaderForMaterial(Material* material);
private:
	void InitializeColorPass(int windowWidth, int windowHeight);
	void InitializeShadowPass();
	void TraverseAndCollect(SceneNode& node);
	void BuildMaterialBatches();
	void RenderBatch(Material* material, const std::vector<MeshTransformPair>& meshes);
	void RenderColorPass();
	void RendeShadowPass();
	ComPtr<ID3D11VertexShader> LoadVertexShader(const std::wstring& path, ComPtr<ID3DBlob>& outBlob);
	ComPtr<ID3D11PixelShader> LoadPixelShader(const std::wstring& path);
	ComPtr<ID3D11InputLayout> CreateInputLayout(ComPtr<ID3DBlob> vsBlob);
};
