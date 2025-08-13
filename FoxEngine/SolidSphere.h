#pragma once
#include "DrawableBase.h"

class SolidSphere : public DrawableBase<SolidSphere>
{
public:
	SolidSphere(Graphics& gfx, float radius = 0.3f, int segU = 20, int segV = 20);

	void Update(float deltaTime) override;
	void SetPosition(const DirectX::XMFLOAT3& position);

private:
	DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
	float scale = 1.0f;
};

