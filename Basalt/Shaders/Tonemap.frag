#version 450

// HDR -> display: exposure, tonemapping operator, sRGB encoding and dithering.
layout(set = 0, binding = 0) uniform texture2D u_HDR;
layout(set = 0, binding = 1) uniform sampler u_PointClamp;
layout(set = 0, binding = 2) uniform texture2D u_SSAO;
layout(set = 0, binding = 3) uniform texture2D u_Normals;
layout(set = 0, binding = 4) uniform texture2D u_Depth;

layout(push_constant) uniform TonemapPush
{
	float Exposure;
	uint Operator;      // 0 None, 1 Reinhard, 2 ACES, 3 AgX
	uint DebugView;     // 0 final, 1 SSAO, 2 view-space normals, 3 depth
} u_Push;

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

// ACES fitted curve (Stephen Hill), with the sRGB -> ACEScg input/output matrices.
vec3 ACESFitted(vec3 color)
{
	const mat3 inputMatrix = mat3(0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777);
	const mat3 outputMatrix = mat3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602);
	color = inputMatrix * color;
	vec3 a = color * (color + 0.0245786) - 0.000090537;
	vec3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
	return clamp(outputMatrix * (a / b), 0.0, 1.0);
}

// AgX (Benjamin Wrensch's minimal fit), base look.
vec3 AgX(vec3 color)
{
	const mat3 inset = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051, 0.0784335999999992, 0.878468636469772, 0.0784336, 0.0792237451477643, 0.0791661274605434, 0.879142973793104);
	const mat3 outset = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438, -0.0980208811401368, 1.15190312990417, -0.0980434501171241, -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
	const float minEv = -12.47393;
	const float maxEv = 4.026069;
	color = inset * color;
	color = clamp(log2(max(color, 1e-10)), minEv, maxEv);
	color = (color - minEv) / (maxEv - minEv);
	vec3 x2 = color * color;
	vec3 x4 = x2 * x2;
	color = 15.5 * x4 * x2 - 40.14 * x4 * color + 31.96 * x4 - 6.868 * x2 * color + 0.4298 * x2 + 0.1191 * color - 0.00232;
	color = outset * color;
	// The AgX curve outputs display-referred values that already include a 2.2 power; undo it so the
	// shared sRGB encode below applies.
	return pow(clamp(color, 0.0, 1.0), vec3(2.2));
}

vec3 LinearToSRGB(vec3 color)
{
	vec3 low = color * 12.92;
	vec3 high = 1.055 * pow(color, vec3(1.0 / 2.4)) - 0.055;
	return mix(high, low, lessThanEqual(color, vec3(0.0031308)));
}

void main()
{
	if (u_Push.DebugView == 1u)
	{
		o_Color = vec4(vec3(textureLod(sampler2D(u_SSAO, u_PointClamp), v_TexCoord, 0.0).r), 1.0);
		return;
	}
	if (u_Push.DebugView == 2u)
	{
		o_Color = vec4(textureLod(sampler2D(u_Normals, u_PointClamp), v_TexCoord, 0.0).xyz * 0.5 + 0.5, 1.0);
		return;
	}
	if (u_Push.DebugView == 3u)
	{
		float depth = textureLod(sampler2D(u_Depth, u_PointClamp), v_TexCoord, 0.0).r;
		o_Color = vec4(vec3(pow(depth, 64.0)), 1.0);
		return;
	}

	vec3 color = textureLod(sampler2D(u_HDR, u_PointClamp), v_TexCoord, 0.0).rgb * u_Push.Exposure;
	if (u_Push.Operator == 1u)
		color = color / (1.0 + color);
	else if (u_Push.Operator == 2u)
		color = ACESFitted(color);
	else if (u_Push.Operator == 3u)
		color = AgX(color);
	color = LinearToSRGB(clamp(color, 0.0, 1.0));

	// Triangular dither removes banding in dark gradients (8-bit output).
	float noise = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
	color += (noise - 0.5) / 255.0;
	o_Color = vec4(color, 1.0);
}
