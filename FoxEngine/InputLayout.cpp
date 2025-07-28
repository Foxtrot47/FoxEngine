#include "InputLayout.h"

InputLayout::InputLayout(Graphics& gfx,
	const std::vector<D3D11_INPUT_ELEMENT_DESC>& layout,
	ID3DBlob* pVertexShaderByteCode)
{
	GetDevice(gfx)->CreateInputLayout(
		layout.data(),								// Input element description
		(UINT)layout.size(),						// Number of elements in the array
		pVertexShaderByteCode->GetBufferPointer(),	// Pointer to the compiled shader bytecode
		pVertexShaderByteCode->GetBufferSize(),		// Size of the compiled shader bytecode
		&pInputLayout								// Output pointer to the input layout
	);
}

void InputLayout::Bind(Graphics& gfx)
{
	GetContext(gfx)->IASetInputLayout(pInputLayout.Get());
}
