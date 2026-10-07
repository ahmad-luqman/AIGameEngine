#version 450

// Separable depth-aware (bilateral) blur: smooths SSAO noise without bleeding across edges.
layout(set = 0, binding = 0) uniform texture2D u_Input;
layout(set = 0, binding = 1) uniform texture2D u_Depth;
layout(set = 0, binding = 2) uniform sampler u_PointClamp;

layout(push_constant) uniform BlurPush
{
	vec2 Direction;     // texel step (1/width, 0) or (0, 1/height)
	float Near;
	float Far;
	float Orthographic; // 1 for orthographic cameras (depth is already linear)
} u_Push;

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out float o_Occlusion;

float LinearDepth(float depth)
{
	if (u_Push.Orthographic > 0.5)
		return u_Push.Near + depth * (u_Push.Far - u_Push.Near);
	return u_Push.Near * u_Push.Far / (u_Push.Far - depth * (u_Push.Far - u_Push.Near));
}

void main()
{
	const float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
	float centerDepth = LinearDepth(textureLod(sampler2D(u_Depth, u_PointClamp), v_TexCoord, 0.0).r);

	float total = textureLod(sampler2D(u_Input, u_PointClamp), v_TexCoord, 0.0).r * weights[0];
	float weightSum = weights[0];
	for (int i = 1; i < 5; i++)
	{
		for (int side = -1; side <= 1; side += 2)
		{
			vec2 uv = v_TexCoord + u_Push.Direction * float(i * side) * 1.5;
			float sampleDepth = LinearDepth(textureLod(sampler2D(u_Depth, u_PointClamp), uv, 0.0).r);
			float depthWeight = exp(-abs(sampleDepth - centerDepth) * 4.0 / max(centerDepth * 0.05, 1e-3));
			float weight = weights[i] * depthWeight;
			total += textureLod(sampler2D(u_Input, u_PointClamp), uv, 0.0).r * weight;
			weightSum += weight;
		}
	}
	o_Occlusion = total / weightSum;
}
