#include "Topology.h"

Topology::Topology(Graphics& gfx, D3D11_PRIMITIVE_TOPOLOGY _topologyType)
	:
	topologyType(_topologyType)
{}

void Topology::Bind(Graphics& gfx)
{
	GetContext(gfx)->IASetPrimitiveTopology(topologyType);
}

