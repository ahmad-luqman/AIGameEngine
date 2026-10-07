#version 450
#include "Common.glsl"

layout(location = 0) in vec3 a_Position;

void main()
{
	DrawData draw = u_Draws.Draws[u_Push.DrawIndex];
	gl_Position = u_View.CascadeViewProjection[u_Push.CascadeIndex] * draw.Model * vec4(a_Position, 1.0);
}
