#version 450
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_RCAS_F 1
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

vec4 FsrRcasLoadF(ivec2 p)
{
	return texelFetch(sampler2D(u_Input, u_PointClamp), clamp(p, ivec2(0), ivec2(u_Push.OutputSize) - 1), 0);
}
void FsrRcasInputF(inout float r, inout float g, inout float b) {}

void main()
{
	uvec4 con;
	FsrRcasCon(con, u_Push.Sharpness);
	vec3 color;
	FsrRcasF(color.r, color.g, color.b, uvec2(gl_FragCoord.xy), con);
	// Dither once, at the final 8-bit conversion, so RCAS does not sharpen the dither noise.
	float noise = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
	o_Color = vec4(clamp(color, 0.0, 1.0) + (noise - 0.5) / 255.0, 1.0);
}
