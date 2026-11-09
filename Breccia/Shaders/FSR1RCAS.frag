#version 450
#include "Dither.glsl"
#define FSR_RCAS_F 1
#include "FSR1.glsl"

vec4 FsrRcasLoadF(ivec2 p)
{
	return texelFetch(sampler2D(u_Input, u_PointClamp), clamp(p, ivec2(0), ivec2(u_Push.OutputSize) - 1), 0);
}
// RCAS input is already normalized perceptual color; no conversion is needed in this callback.
void FsrRcasInputF(inout float r, inout float g, inout float b) {}

void main()
{
	uvec4 con;
	FsrRcasCon(con, u_Push.Sharpness);
	vec3 color;
	FsrRcasF(color.r, color.g, color.b, uvec2(gl_FragCoord.xy), con);
	// Dither once, at the final 8-bit conversion, so RCAS does not sharpen the dither noise.
	o_Color = vec4(clamp(color, 0.0, 1.0) + OutputDither(gl_FragCoord.xy), 1.0);
}
