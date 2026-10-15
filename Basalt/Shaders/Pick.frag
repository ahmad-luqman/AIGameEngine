#version 450
#include "Common.glsl"
#include "Material.glsl"
layout(location = 3) in vec2 v_TexCoord;
layout(location = 4) flat in uint v_MaterialIndex;
layout(location = 0) out uint o_EntityId;
const float MinimumPickOpacity = 0.1;
void main()
{
	MaterialData material = u_Materials.Materials[v_MaterialIndex];
	float alpha = MaterialAlpha(material, v_TexCoord);
	// Blended surfaces below 10% opacity do not intercept editor clicks.
	if (alpha < MinimumPickOpacity)
		discard;
	o_EntityId = u_Draws.Draws[u_Push.DrawIndex].Indices.y;
}
