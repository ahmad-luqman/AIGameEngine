#version 450
#include "RenderArea.glsl"
#include "Dither.glsl"
layout(set = 0, binding = 0) uniform texture2D u_Color;
layout(set = 0, binding = 1) uniform sampler u_LinearClamp;
layout(push_constant) uniform FxaaPush
{
	vec2 RenderSize;
	uint Dither;
}
u_Push;
layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;
vec3 Sample(vec2 uv)
{
	vec2 extent = vec2(textureSize(sampler2D(u_Color, u_LinearClamp), 0));
	vec2 scale = u_Push.RenderSize / extent;
	return textureLod(sampler2D(u_Color, u_LinearClamp), ClampToRenderArea(uv * scale, scale, ivec2(extent)), 0).rgb;
}
void main()
{
	vec2 texel = 1.0 / u_Push.RenderSize;
	vec3 center = Sample(v_TexCoord);
	vec3 nw = Sample(v_TexCoord + vec2(-1, -1) * texel), ne = Sample(v_TexCoord + vec2(1, -1) * texel);
	vec3 sw = Sample(v_TexCoord + vec2(-1, 1) * texel), se = Sample(v_TexCoord + vec2(1, 1) * texel);
	const vec3 luma = vec3(0.299, 0.587, 0.114);
	float m = dot(center, luma), a = dot(nw, luma), b = dot(ne, luma), c = dot(sw, luma), d = dot(se, luma);
	float low = min(m, min(min(a, b), min(c, d))), high = max(m, max(max(a, b), max(c, d)));
	vec3 color = center;
	if (high - low >= max(0.0312, high * 0.125))
	{
		vec2 direction = vec2(-((a + b) - (c + d)), (a + c) - (b + d));
		float reduction = max((a + b + c + d) * (0.25 / 8.0), 1.0 / 128.0);
		direction = clamp(direction / (min(abs(direction.x), abs(direction.y)) + reduction), vec2(-8), vec2(8)) * texel;
		vec3 first = 0.5 * (Sample(v_TexCoord + direction * (1.0 / 3.0 - 0.5)) + Sample(v_TexCoord + direction * (2.0 / 3.0 - 0.5)));
		vec3 second = first * 0.5 + 0.25 * (Sample(v_TexCoord - direction * 0.5) + Sample(v_TexCoord + direction * 0.5));
		float l = dot(second, luma);
		color = l < low || l > high ? first : second;
	}
	if (u_Push.Dither != 0u)
		color += OutputDither(gl_FragCoord.xy);
	o_Color = vec4(color, 1);
}
