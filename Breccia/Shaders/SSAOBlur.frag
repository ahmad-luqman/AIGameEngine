#version 450
#include "RenderArea.glsl"

// Separable depth-aware (bilateral) blur: smooths SSAO noise without bleeding across edges. Taps are fetched by
// texel: point-sampling at 1.5-texel offsets would put odd taps on texel boundaries, where rounding differs between
// drivers and target sizes.
layout(set = 0, binding = 0) uniform texture2D u_Input;
layout(set = 0, binding = 1) uniform texture2D u_Depth;
layout(set = 0, binding = 2) uniform sampler u_PointClamp;

layout(push_constant) uniform BlurPush
{
	ivec2 Axis;         // (1, 0) or (0, 1)
	float Near;
	float Far;
	float Orthographic; // 1 for orthographic cameras (depth is already linear)
	float Padding;      // aligns RenderSize to 8 bytes in both std430 and the C++ mirror (SsaoBlurPush)
	ivec2 RenderSize;   // texels of the render area in the top-left of the targets
} u_Push;

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out float o_Occlusion;

// Taps at i * 1.5 texels: even ones on a texel, odd ones halfway between two, which take both at half weight.
const float Weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
const int LowerOffsets[5] = int[](0, 1, 3, 4, 6);
const bool Split[5] = bool[](false, true, false, true, false);

float LinearDepth(float depth)
{
	if (u_Push.Orthographic > 0.5)
		return u_Push.Near + depth * (u_Push.Far - u_Push.Near);
	return u_Push.Near * u_Push.Far / (u_Push.Far - depth * (u_Push.Far - u_Push.Near));
}

// Adds the texel offset along the axis from center (clamped to the render area: beyond it the SSAO targets hold
// results of frames rendered larger), weighted down by its depth difference from the center.
void Tap(ivec2 center, int offset, ivec2 lastTexel, float centerDepth, float weight, inout float total, inout float weightSum)
{
	ivec2 texel = clamp(center + u_Push.Axis * offset, ivec2(0), lastTexel);
	float sampleDepth = LinearDepth(texelFetch(sampler2D(u_Depth, u_PointClamp), texel, 0).r);
	weight *= exp(-abs(sampleDepth - centerDepth) * 4.0 / max(centerDepth * 0.05, 1e-3));
	total += texelFetch(sampler2D(u_Input, u_PointClamp), texel, 0).r * weight;
	weightSum += weight;
}

void main()
{
	// Bounded by the targets, so even inconsistent constants cannot fetch outside them.
	ivec2 renderSize = min(u_Push.RenderSize, textureSize(sampler2D(u_Depth, u_PointClamp), 0));
	ivec2 lastTexel = renderSize - 1;
	ivec2 center = RenderAreaTexel(v_TexCoord, renderSize);
	float centerDepth = LinearDepth(texelFetch(sampler2D(u_Depth, u_PointClamp), center, 0).r);

	float total = texelFetch(sampler2D(u_Input, u_PointClamp), center, 0).r * Weights[0];
	float weightSum = Weights[0];
	for (int i = 1; i < 5; i++)
	{
		float weight = Split[i] ? Weights[i] * 0.5 : Weights[i];
		for (int side = -1; side <= 1; side += 2)
		{
			Tap(center, side * LowerOffsets[i], lastTexel, centerDepth, weight, total, weightSum);
			if (Split[i])
				Tap(center, side * (LowerOffsets[i] + 1), lastTexel, centerDepth, weight, total, weightSum);
		}
	}
	o_Occlusion = total / weightSum;
}
