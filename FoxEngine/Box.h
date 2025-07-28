#pragma once
#include "Drawable.h"
class Box : public Drawable
{
public:
	Box(Graphics& gfx,
		std::mt19937& rng,
		std::uniform_real_distribution<float>& angularDistribution,
		std::uniform_real_distribution<float>& deltaDistribution,
		std::uniform_real_distribution<float>& orbitalDistribution,
		std::uniform_real_distribution<float>& radiusDistribution
	);
	void Update(float deltaTime) override;
	DirectX::XMMATRIX GetTransformXM() const override;
private:
	float radius;
	float roll = 0.0f;
	float pitch = 0.0f;
	float yaw = 0.0f;
	float deltaRoll;
	float deltaPitch;
	float deltaYaw;

	// orbital rotation
	float theta;
	float phi;
	float chi;
	float deltaTheta;
	float deltaPhi;
	float deltaChi;
};

