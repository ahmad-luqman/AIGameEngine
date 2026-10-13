#version 450
#include "Common.glsl"

layout(location = 0) in vec3 a_Position;

void main()
{
	DrawData draw = u_Draws.Draws[u_Push.DrawIndex];
	gl_Position = ShadowViewProjection(u_Push.ShadowViewIndex) * draw.Model * vec4(a_Position, 1.0);
}
