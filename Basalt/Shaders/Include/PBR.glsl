// Cook-Torrance BRDF (GGX distribution, Smith-GGX geometry, Schlick Fresnel).
#ifndef BASALT_PBR_GLSL
#define BASALT_PBR_GLSL

float DistributionGGX(float NdotH, float roughness)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
	return a2 / max(PI * d * d, 1e-7);
}

float GeometrySchlickGGX(float NdotX, float k)
{
	return NdotX / (NdotX * (1.0 - k) + k);
}

float GeometrySmith(float NdotV, float NdotL, float roughness)
{
	float r = roughness + 1.0;
	float k = (r * r) / 8.0;
	return GeometrySchlickGGX(NdotV, k) * GeometrySchlickGGX(NdotL, k);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0)
{
	return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
	return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Radiance reflected towards V from a light arriving along L with the given radiance.
vec3 EvaluateLight(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float metallic, float roughness, vec3 F0)
{
	vec3 H = normalize(V + L);
	float NdotL = max(dot(N, L), 0.0);
	float NdotV = max(dot(N, V), 1e-4);
	float NdotH = max(dot(N, H), 0.0);
	float HdotV = max(dot(H, V), 0.0);

	float D = DistributionGGX(NdotH, roughness);
	float G = GeometrySmith(NdotV, NdotL, roughness);
	vec3 F = FresnelSchlick(HdotV, F0);

	vec3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
	vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
	return (kD * albedo / PI + specular) * radiance * NdotL;
}

// Smooth inverse-square falloff that reaches zero at the light's range.
float RangeAttenuation(float distance, float range)
{
	float ratio = distance / max(range, 1e-4);
	float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
	return window * window / (distance * distance + 1.0);
}

#endif
