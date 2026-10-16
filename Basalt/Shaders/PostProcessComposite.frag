#version 450
#include "RenderArea.glsl"
layout(set = 0, binding = 0) uniform texture2D u_HDR;
layout(set = 0, binding = 1) uniform sampler u_LinearClamp;
layout(set = 0, binding = 2) uniform texture2D u_Bloom;
layout(set = 0, binding = 3) uniform texture2D u_Exposure;
layout(push_constant) uniform CompositePush
{
	vec2 BloomSize;
	float BloomIntensity;
	uint AutoExposure;
}
u_Push;
layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;
void main()
{
	vec3 color = texelFetch(sampler2D(u_HDR, u_LinearClamp), ivec2(gl_FragCoord.xy), 0).rgb;
	if (u_Push.BloomIntensity > 0.0)
	{
		vec2 extent = vec2(textureSize(sampler2D(u_Bloom, u_LinearClamp), 0));
		vec2 scale = u_Push.BloomSize / extent;
		color += textureLod(sampler2D(u_Bloom, u_LinearClamp), ClampToRenderArea(v_TexCoord * scale, scale, ivec2(extent)), 0).rgb * u_Push.BloomIntensity;
	}
	if (u_Push.AutoExposure != 0u)
		color *= texelFetch(sampler2D(u_Exposure, u_LinearClamp), ivec2(0), 0).r;
	o_Color = vec4(clamp(color, vec3(0), vec3(65504)), 1);
}
