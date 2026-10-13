#version 450
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_EASU_F 1
#include "ffx_fsr1.h"

layout(set = 0, binding = 0) uniform texture2D u_Input;
layout(set = 0, binding = 1) uniform sampler u_PointClamp;
layout(push_constant) uniform FsrPush
{
	vec2 RenderSize;
	vec2 OutputSize;
	float Sharpness;
} u_Push;
layout(location = 0) out vec4 o_Color;

vec3 LoadInput(ivec2 p)
{
	// Clamp every gather tap, not just its centre: the unused part of a dynamic target may contain
	// earlier frames. Integer fetches also support one-pixel render areas without a special kernel.
	return texelFetch(sampler2D(u_Input, u_PointClamp), clamp(p, ivec2(0), ivec2(u_Push.RenderSize) - 1), 0).rgb;
}

vec4 GatherChannel(vec2 p, int channel)
{
	ivec2 base = ivec2(floor(p * vec2(textureSize(sampler2D(u_Input, u_PointClamp), 0)) - 0.5));
	// GLSL textureGather order: lower-left, lower-right, upper-right, upper-left in texture space.
	return vec4(LoadInput(base + ivec2(0, 1))[channel], LoadInput(base + ivec2(1, 1))[channel],
		LoadInput(base + ivec2(1, 0))[channel], LoadInput(base)[channel]);
}
vec4 FsrEasuRF(vec2 p) { return GatherChannel(p, 0); }
vec4 FsrEasuGF(vec2 p) { return GatherChannel(p, 1); }
vec4 FsrEasuBF(vec2 p) { return GatherChannel(p, 2); }

void main()
{
	uvec4 con0, con1, con2, con3;
	vec2 targetSize = vec2(textureSize(sampler2D(u_Input, u_PointClamp), 0));
	FsrEasuCon(con0, con1, con2, con3, u_Push.RenderSize.x, u_Push.RenderSize.y,
		targetSize.x, targetSize.y, u_Push.OutputSize.x, u_Push.OutputSize.y);
	vec3 color;
	FsrEasuF(color, uvec2(gl_FragCoord.xy), con0, con1, con2, con3);
	o_Color = vec4(clamp(color, 0.0, 1.0), 1.0);
}
