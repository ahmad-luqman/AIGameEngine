#version 450
#include "Common.glsl"
#include "Material.glsl"

layout(location = 0) in vec3 v_WorldPosition;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec4 v_Tangent;
layout(location = 3) in vec2 v_TexCoord;
layout(location = 4) flat in uint v_MaterialIndex;

// View-space normal for SSAO.
layout(location = 0) out vec4 o_Normal;

void main()
{
	MaterialData material = u_Materials.Materials[v_MaterialIndex];
	Surface surface = EvaluateSurface(material, v_Normal, v_Tangent, v_TexCoord, gl_FrontFacing);
	if (material.Params.z > 0.0 && surface.Alpha < material.Params.z)
		discard;
	o_Normal = vec4(normalize(mat3(u_View.View) * surface.Normal), 1.0);
}
