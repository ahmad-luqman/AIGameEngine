#version 450
#include "Common.glsl"

layout(location = 0) in vec3 a_Position;

void main()
{
	DrawData draw = u_Draws.Draws[u_Push.DrawIndex];
	mat4 lightMatrix = u_Push.CascadeIndex < MAX_CASCADES ? u_View.CascadeViewProjection[u_Push.CascadeIndex] :
		u_LocalShadows.Maps[u_Push.CascadeIndex - MAX_CASCADES].ViewProjection;
	gl_Position = lightMatrix * draw.Model * vec4(a_Position, 1.0);
}
