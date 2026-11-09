#version 450
#define FSR_EASU_F 1
#include "FSR1.glsl"

vec3 LoadInput(ivec2 p)
{
	// Clamp every gather tap, not just its centre: the unused part of a dynamic target may contain
	// earlier frames. Integer fetches also support one-pixel render areas without a special kernel.
	return texelFetch(sampler2D(u_Input, u_PointClamp), clamp(p, ivec2(0), ivec2(u_Push.RenderSize) - 1), 0).rgb;
}

vec4 GatherChannel(vec2 p, int channel)
{
	ivec2 base = ivec2(floor(p * vec2(textureSize(sampler2D(u_Input, u_PointClamp), 0)) - 0.5));
	// GLSL textureGather order relative to base: (0,1), (1,1), (1,0), (0,0).
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
