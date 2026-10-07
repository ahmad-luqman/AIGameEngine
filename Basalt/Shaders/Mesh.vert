#version 450
#include "Common.glsl"

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec4 a_Tangent;
layout(location = 3) in vec2 a_TexCoord;

layout(location = 0) out vec3 v_WorldPosition;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec4 v_Tangent;
layout(location = 3) out vec2 v_TexCoord;
layout(location = 4) flat out uint v_MaterialIndex;

// The depth prepass and the lighting pass must produce bit-identical depth.
invariant gl_Position;

void main()
{
	DrawData draw = u_Draws.Draws[u_Push.DrawIndex];
	vec4 world = draw.Model * vec4(a_Position, 1.0);
	v_WorldPosition = world.xyz;
	v_Normal = normalize(mat3(draw.Normal) * a_Normal);
	v_Tangent = vec4(normalize(mat3(draw.Model) * a_Tangent.xyz), a_Tangent.w);
	v_TexCoord = a_TexCoord;
	v_MaterialIndex = draw.Indices.x;
	gl_Position = u_View.ViewProjection * world;
}
