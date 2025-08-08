#include "PointLight.h"
#include "imgui.h"

PointLight::PointLight(Graphics& gfx)
{
	if (!lightPSCbuff)
	{
		lightPSCbuff = std::make_unique<PixelConstantBuffer<LightCBuff>>(gfx);
	}
	sphere = std::make_unique<SolidSphere>(gfx, 0.3f, 20, 20);
}

void PointLight::SpawnControlWindow()
{
	if(ImGui::Begin("Light"))
	{
		ImGui::Text("Position");
		ImGui::SliderFloat("X", &pos.x, -100.0f, 100.0f, "%.1f");
		ImGui::SliderFloat("Y", &pos.y, -100.0f, 100.0f, "%.1f");
		ImGui::SliderFloat("Z", &pos.z, -100.0f, 100.0f, "%.1f");

		ImGui::Text("Color");
		ImGui::SliderFloat("R", &color.x, 0.0f, 1.0f, "%.1f");
		ImGui::SliderFloat("G", &color.y, 0.0f, 1.0f, "%.1f");
		ImGui::SliderFloat("B", &color.z, 0.0f, 1.0f, "%.1f");
	}
	ImGui::End();
}

void PointLight::DrawSphere(Graphics& gfx)
{
	if (!sphere) return;

	sphere->SetPosition(pos);
	sphere->Draw(gfx);
}

void PointLight::Bind(Graphics& gfx)
{
	const LightCBuff buffer = { pos,0.0f, color, 0.0f };
	lightPSCbuff->Update(gfx, buffer);
	lightPSCbuff->Bind(gfx);
}