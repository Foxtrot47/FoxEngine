#include "PointLight.h"
#include "imgui.h"

PointLight::PointLight(Graphics& gfx) :	valuesChanged(true)
{
	if (!plightPSCbuff)
	{
		plightPSCbuff = std::make_unique<PixelConstantBuffer<LightCBuff>>(gfx);
	}
	Reset();
	sphere = std::make_unique<SolidSphere>(gfx, 0.3f, 20, 20);
}

void PointLight::SpawnControlWindow()
{
	if(ImGui::Begin("Light"))
	{
		if (ImGui::ColorEdit3("Light Color", reinterpret_cast<float*>(&lightCbuff.lightColor)))
			valuesChanged = true;
        
		if (ImGui::ColorEdit3("Ambient Light", reinterpret_cast<float*>(&lightCbuff.ambientLight)))
			valuesChanged = true;
		
		if (ImGui::SliderFloat("Ambient Strength", &lightCbuff.ambientStrength, 0.0f, 1.0f))
			valuesChanged = true;
        
		if (ImGui::SliderFloat("Specular Intensity", &lightCbuff.specularIntensity, 0.0f, 1.0f))
			valuesChanged = true;
        
		if (ImGui::SliderFloat("Specular Power", &lightCbuff.specularPower, 1.0f, 128.0f))
			valuesChanged = true;

		if( ImGui::Button( "Reset" ) )
		{
			Reset();
		}
	}
	ImGui::End();
}

void PointLight::DrawSphere(Graphics& gfx) const
{
	if (!sphere) return;

	sphere->SetPosition(lightCbuff.lightPos);
	sphere->Draw(gfx);
}

void PointLight::Bind(Graphics& gfx)
{
	if (valuesChanged)
	{
		plightPSCbuff->Update(gfx, lightCbuff);
		plightPSCbuff->Bind(gfx);
		valuesChanged = false;
	}
}

void PointLight::Reset()
{
	const LightCBuff buff = {
		{ 0.0f, 0.0f, 0.0f },
		0.0f,
		{ 1.0f, 1.0f, 1.0f },
		0.1f,
		{ 1.0f, 1.0f, 1.0f },
		1.0f,
		10.0f,
		{ 0.0f, 0.0f, 0.0f }
	};
	lightCbuff = buff;
	valuesChanged = true;
}