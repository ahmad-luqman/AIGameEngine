#version 450
#include "Common.glsl"

// Shadow pass for alpha-tested materials: passes UVs so the fragment shader can discard.
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;
layout(location = 1) flat out uint v_MaterialIndex;

void main()
{
	DrawData draw = u_Draws.Draws[u_Push.DrawIndex];
	v_TexCoord = a_TexCoord;
	v_MaterialIndex = draw.Indices.x;
	mat4 lightMatrix = u_Push.CascadeIndex < MAX_CASCADES ? u_View.CascadeViewProjection[u_Push.CascadeIndex] :
		u_LocalShadows.Maps[u_Push.CascadeIndex - MAX_CASCADES].ViewProjection;
	gl_Position = lightMatrix * draw.Model * vec4(a_Position, 1.0);
}
