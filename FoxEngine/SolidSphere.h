#pragma once
#include "Graphics.h"
#include "BindableBase.h"

class SolidSphere
{
public:
	SolidSphere(Graphics& gfx, float radius = 0.3f, int segU = 20, int segV = 20);
	SolidSphere(const SolidSphere&) = delete;
	void SetPosition(const DirectX::XMFLOAT3& position);
	void Draw(Graphics& gfx, DirectX::XMMATRIX transform) const;
private:
	DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
	float scale = 1.0f;

	class IndexBuffer* pIndexBuffer = nullptr;
	class VertexBuffer* pVertexBuffer = nullptr;
	class Material* pMaterial = nullptr;
	class Topology* pTopology = nullptr;
	class TransformConstantBuffer* pTransformCB = nullptr;
};

