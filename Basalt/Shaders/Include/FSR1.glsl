// Shared 32-bit AMD FSR 1 interface. Each pass defines FSR_EASU_F or FSR_RCAS_F before inclusion.
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#include "ffx_fsr1.h"

layout(set = 0, binding = 0) uniform texture2D u_Input;
layout(set = 0, binding = 1) uniform sampler u_PointClamp;
// Matches FsrPush in Fsr1Upscaler.cpp.
layout(push_constant) uniform FsrPush
{
	vec2 RenderSize;
	vec2 OutputSize;
	float Sharpness;
} u_Push;
layout(location = 0) out vec4 o_Color;
