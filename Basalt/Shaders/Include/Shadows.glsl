// Cascaded shadow maps with percentage-closer soft shadows (PCSS).
#ifndef BASALT_SHADOWS_GLSL
#define BASALT_SHADOWS_GLSL

layout(set = 0, binding = 4) uniform texture2DArray u_ShadowMap;
layout(set = 0, binding = 10) uniform samplerShadow u_ShadowCompareSampler;
layout(set = 0, binding = 11) uniform sampler u_PointSampler;

const vec2 c_PoissonDisk[16] = vec2[](
	vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725), vec2(-0.09418410, -0.92938870), vec2(0.34495938, 0.29387760),
	vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464), vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
	vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420), vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
	vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590), vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790));

// Per-pixel rotation of the Poisson disk turns banding into fine noise.
float InterleavedGradientNoise(vec2 position)
{
	return fract(52.9829189 * fract(dot(position, vec2(0.06711056, 0.00583715))));
}

int SelectCascade(float viewDepth)
{
	int cascadeCount = int(u_View.Sun.ShadowParams.w);
	for (int i = 0; i < cascadeCount; i++)
	{
		if (viewDepth < u_View.CascadeSplits[i])
			return i;
	}
	return -1;
}

float SampleCascade(int cascade, vec3 worldPosition, vec3 normal, vec2 fragCoord)
{
	vec3 L = -normalize(u_View.Sun.Direction.xyz);
	float NdotL = clamp(dot(normal, L), 0.0, 1.0);
	// Normal offset scales with cascade size so distant cascades stay acne-free.
	float texelWorld = (u_View.CascadeSplits[cascade] * 2.0) / float(textureSize(sampler2DArray(u_ShadowMap, u_PointSampler), 0).x);
	vec3 offsetPosition = worldPosition + normal * u_View.Sun.ShadowParams.y * (1.0 - NdotL) * (1.0 + float(cascade)) + normal * texelWorld;

	vec4 lightClip = u_View.CascadeViewProjection[cascade] * vec4(offsetPosition, 1.0);
	vec3 coords = lightClip.xyz / lightClip.w;
	vec2 uv = vec2(coords.x * 0.5 + 0.5, 0.5 - coords.y * 0.5);
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || coords.z > 1.0)
		return 1.0;

	float receiverDepth = coords.z - u_View.Sun.ShadowParams.x;
	vec2 texelSize = 1.0 / vec2(textureSize(sampler2DArray(u_ShadowMap, u_PointSampler), 0).xy);
	float angle = InterleavedGradientNoise(fragCoord) * 2.0 * PI;
	mat2 rotation = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));

	// 1. Blocker search: average depth of occluders in the search region.
	float lightSize = u_View.Sun.ShadowParams.z;
	float searchRadius = max(lightSize * 6.0, 1.0) * texelSize.x / (1.0 + float(cascade));
	float blockerSum = 0.0;
	float blockerCount = 0.0;
	for (int i = 0; i < 16; i++)
	{
		vec2 offset = rotation * c_PoissonDisk[i] * searchRadius;
		float depth = texture(sampler2DArray(u_ShadowMap, u_PointSampler), vec3(uv + offset, float(cascade))).r;
		if (depth < receiverDepth)
		{
			blockerSum += depth;
			blockerCount += 1.0;
		}
	}
	if (blockerCount < 1.0)
		return 1.0;

	// 2. Penumbra width from the receiver/blocker distance (orthographic light: linear in depth).
	float averageBlocker = blockerSum / blockerCount;
	float penumbra = clamp((receiverDepth - averageBlocker) * lightSize * 400.0, 1.0, 12.0);
	float filterRadius = penumbra * texelSize.x / (1.0 + 0.5 * float(cascade));

	// 3. PCF with hardware comparison over the penumbra.
	float lit = 0.0;
	for (int i = 0; i < 16; i++)
	{
		vec2 offset = rotation * c_PoissonDisk[i] * filterRadius;
		lit += texture(sampler2DArrayShadow(u_ShadowMap, u_ShadowCompareSampler), vec4(uv + offset, float(cascade), receiverDepth));
	}
	return lit / 16.0;
}

float ComputeShadow(vec3 worldPosition, vec3 normal, float viewDepth, vec2 fragCoord)
{
	if (u_View.Sun.Color.w < 0.5)
		return 1.0;
	int cascade = SelectCascade(viewDepth);
	if (cascade < 0)
		return 1.0;

	float shadow = SampleCascade(cascade, worldPosition, normal, fragCoord);

	// Blend into the next cascade near the split to hide the seam.
	int cascadeCount = int(u_View.Sun.ShadowParams.w);
	float splitEnd = u_View.CascadeSplits[cascade];
	float splitStart = cascade == 0 ? 0.0 : u_View.CascadeSplits[cascade - 1];
	float blendZone = (splitEnd - splitStart) * 0.1;
	float blend = clamp((splitEnd - viewDepth) / blendZone, 0.0, 1.0);
	if (blend < 1.0)
	{
		float next = cascade + 1 < cascadeCount ? SampleCascade(cascade + 1, worldPosition, normal, fragCoord) : 1.0;
		shadow = mix(next, shadow, blend);
	}
	return shadow;
}

#endif
