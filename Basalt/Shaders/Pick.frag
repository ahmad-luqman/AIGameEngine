#version 450
#include "Common.glsl"
#include "Material.glsl"
layout(location = 3) in vec2 v_TexCoord;
layout(location = 4) flat in uvec2 v_InstanceIndices;
layout(location = 0) out uint o_EntityId;
const float MinimumPickOpacity = 0.1;
layout(location = 5) in vec2 v_TexCoord1;

void main()
{
	MaterialData material = u_Materials.Materials[v_InstanceIndices.x];
	float alpha = MaterialAlpha(material, v_TexCoord, v_TexCoord1);
	// Blended surfaces below 10% opacity do not intercept editor clicks.
	if (alpha < MinimumPickOpacity)
		discard;
	o_EntityId = v_InstanceIndices.y;
}
