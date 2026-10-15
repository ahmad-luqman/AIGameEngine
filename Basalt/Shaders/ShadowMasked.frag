#version 450
#include "Common.glsl"
#include "Material.glsl"

layout(location = 0) in vec2 v_TexCoord;
layout(location = 1) flat in uint v_MaterialIndex;

void main()
{
	MaterialData material = u_Materials.Materials[v_MaterialIndex];
	float alpha = MaterialAlpha(material, v_TexCoord);
	if (alpha < material.Params.z)
		discard;
}
