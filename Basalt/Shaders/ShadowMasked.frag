#version 450
#include "Common.glsl"
#include "Material.glsl"

layout(location = 0) in vec2 v_TexCoord;
layout(location = 1) flat in uint v_MaterialIndex;

void main()
{
	MaterialData material = u_Materials.Materials[v_MaterialIndex];
	float alpha = material.AlbedoColor.a;
	if ((uint(material.Params.w) & MATERIAL_HAS_ALBEDO_MAP) != 0u)
		alpha *= texture(sampler2D(u_AlbedoMap, u_MaterialSampler), v_TexCoord).a;
	if (alpha < material.Params.z)
		discard;
}
