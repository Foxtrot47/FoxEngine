#pragma once
#include "DirectXMath.h"
#include <algorithm>

DirectX::XMFLOAT3 ConvertQuaternionToEuler(DirectX::XMFLOAT4 quat)
{
	auto quatVec = DirectX::XMLoadFloat4(&quat);
	quatVec = DirectX::XMQuaternionNormalize(quatVec);
	DirectX::XMStoreFloat4(&quat, quatVec);

	// Given a quaternion q = (x, y, z, w)
	// Pitch asin(-2(xz - wy))
	// Yaw atan2(2(yz + wx), w^2 - x^2 - y^2 + z^2)
	// Roll atan2(2(xy + wz), w^2 + x^2 - y^2 - z^2)
	// Convert radians to degrees (deg = rad * 180  / pi )
	
	// Clamp the pitch calculation to avoid NaN from asin
	float pitchSin = -2.0f * (quat.x * quat.z - quat.w * quat.y);
	pitchSin = std::max(-1.0f, std::min(1.0f, pitchSin)); // Clamp to [-1, 1]
    
	const float pitch = asin(pitchSin);
	const float pitch_deg = pitch * 180.0f / DirectX::XM_PI;
    
	const float yaw = atan2(2.0f * (quat.y * quat.z + quat.w * quat.x),
		pow(quat.w, 2) - pow(quat.x, 2) - pow(quat.y, 2) + pow(quat.z, 2));
	const float yaw_deg = yaw * 180.0f / DirectX::XM_PI;
    
	const float roll = atan2(2.0f * (quat.x * quat.y + quat.w * quat.z),
		pow(quat.w, 2) + pow(quat.x, 2) - pow(quat.y, 2) - pow(quat.z, 2));
	const float roll_deg = roll * 180.0f / DirectX::XM_PI;

	// clamp pitch to avoid gimbal lock
	return DirectX::XMFLOAT3(
		std::max(std::min(pitch_deg, 89.0f), -89.0f),
		yaw_deg,
		roll_deg
	);
}