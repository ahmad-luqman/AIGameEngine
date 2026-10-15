#version 450
#include "Common.glsl"
#include "Material.glsl"
layout(location = 3) in vec2 v_TexCoord;
layout(location = 4) flat in uint v_MaterialIndex;
layout(location = 0) out uint o_EntityId;
void main()
{
	MaterialData material = u_Materials.Materials[v_MaterialIndex];
	float alpha = material.AlbedoColor.a;
	if ((uint(material.Params.w) & 1u) != 0u)
		alpha *= texture(sampler2D(u_AlbedoMap, u_MaterialSampler), v_TexCoord).a;
	// Blended surfaces below 10% opacity do not intercept editor clicks.
	if (alpha < 0.1)
		discard;
	o_EntityId = u_Draws.Draws[u_Push.DrawIndex].Indices.y;
}
