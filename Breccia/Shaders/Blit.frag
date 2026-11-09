#version 450

layout(set = 0, binding = 0) uniform texture2D u_Source;
layout(set = 0, binding = 1) uniform sampler u_LinearClamp;

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

void main()
{
	o_Color = vec4(textureLod(sampler2D(u_Source, u_LinearClamp), v_TexCoord, 0.0).rgb, 1.0);
}
