#version 450

layout(push_constant) uniform LinePush
{
	mat4 ViewProjection;
} u_Push;

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec4 a_Color;

layout(location = 0) out vec4 v_Color;

void main()
{
	v_Color = a_Color;
	gl_Position = u_Push.ViewProjection * vec4(a_Position, 1.0);
}
