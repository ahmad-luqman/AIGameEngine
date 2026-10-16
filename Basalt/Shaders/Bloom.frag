#version 450
#include "RenderArea.glsl"
layout(set = 0, binding = 0) uniform texture2D u_Source;
layout(set = 0, binding = 1) uniform sampler u_LinearClamp;
layout(set = 0, binding = 2) uniform texture2D u_Base;
layout(push_constant) uniform BloomPush
{
	vec2 SourceSize;
	vec2 BaseSize;
	float Threshold;
	float Scatter;
	uint Mode;
}
u_Push;
layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;
vec3 Sample(vec2 uv)
{
	return SampleRenderArea(u_Source, u_LinearClamp, uv, u_Push.SourceSize).rgb;
}
void main()
{
	vec2 texel = 1.0 / u_Push.SourceSize;
	vec3 c = vec3(0);
	if (u_Push.Mode == 2u)
	{
		// Normalized tent upsample, combined with this level's original downsample.
		for (int y = -1; y <= 1; ++y)
			for (int x = -1; x <= 1; ++x)
				c += Sample(v_TexCoord + vec2(x, y) * texel) * float((x == 0 ? 2 : 1) * (y == 0 ? 2 : 1)) / 16.0;
		vec3 base = SampleRenderArea(u_Base, u_LinearClamp, v_TexCoord, u_Push.BaseSize).rgb;
		c = mix(base, c, u_Push.Scatter);
	}
	else
	{
		for (int y = -1; y <= 1; y += 2)
			for (int x = -1; x <= 1; x += 2)
			{
				vec3 tap = max(Sample(v_TexCoord + vec2(x, y) * texel * 0.5), vec3(0));
				if (u_Push.Mode == 0u)
				{
					float brightness = max(tap.r, max(tap.g, tap.b));
					tap *= max(brightness - u_Push.Threshold, 0.0) / max(brightness, 1e-5);
				}
				c += tap * 0.25;
			}
	}
	o_Color = vec4(min(c, vec3(65504)), 1);
}
