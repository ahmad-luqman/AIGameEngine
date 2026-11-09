#version 450

layout(push_constant) uniform PushConstants
{
	vec2 Scale;
	vec2 Translate;
} u_Push;

layout(location = 0) in vec2 a_Position;
layout(location = 1) in vec2 a_TexCoord;
layout(location = 2) in vec4 a_Color;

layout(location = 0) out vec2 v_TexCoord;
layout(location = 1) out vec4 v_Color;

void main()
{
	v_TexCoord = a_TexCoord;
	v_Color = a_Color;
	// ImGui positions grow downwards; clip space is +Y up.
	vec2 position = a_Position * u_Push.Scale + u_Push.Translate;
	gl_Position = vec4(position.x, -position.y, 0.0, 1.0);
}
