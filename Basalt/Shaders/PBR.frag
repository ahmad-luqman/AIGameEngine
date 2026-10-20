#version 450
#include "Common.glsl"
#include "Material.glsl"
#include "PBR.glsl"
#include "Shadows.glsl"
#include "LightClusters.glsl"

layout(set = 0, binding = 5) uniform textureCube u_IrradianceMap;
layout(set = 0, binding = 6) uniform textureCube u_PrefilteredMap;
layout(set = 0, binding = 7) uniform texture2D u_BRDFLut;
layout(set = 0, binding = 8) uniform texture2D u_SSAO;
layout(set = 0, binding = 9) uniform sampler u_LinearSampler;

layout(location = 0) in vec3 v_WorldPosition;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec4 v_Tangent;
layout(location = 3) in vec2 v_TexCoord;
layout(location = 4) flat in uvec2 v_InstanceIndices;

layout(location = 0) out vec4 o_Color;

layout(location = 5) in vec2 v_TexCoord1;

// The transparent pass approximates thin transmission by passing through the destination;
// reflected specular stays visible even at full transmission. Refraction/volume are not modeled.
vec3 EvaluateSurfaceLight(Surface surface, vec3 V, vec3 L, vec3 radiance, vec3 F0)
{
	vec3 base = EvaluateLight(surface.Normal, V, L, radiance, surface.Albedo * (1.0 - surface.Transmission), surface.Metallic, surface.Roughness, F0);
	if (surface.Clearcoat <= 0.0) return base;
	float coatF = FresnelSchlick(max(dot(surface.ClearcoatNormal, V), 0.0), vec3(0.04)).r;
	vec3 coat = EvaluateLight(surface.ClearcoatNormal, V, L, radiance, vec3(0), 0.0, surface.ClearcoatRoughness, vec3(0.04));
	return base * (1.0 - surface.Clearcoat * coatF) + surface.Clearcoat * coat;
}

void main()
{
	MaterialData material = u_Materials.Materials[v_InstanceIndices.x];
	Surface surface = EvaluateSurface(material, v_Normal, v_Tangent, v_TexCoord, v_TexCoord1, v_WorldPosition, gl_FrontFacing);
	if (material.Params.z > 0.0 && surface.Alpha < material.Params.z)
		discard;

	vec3 N = surface.Normal;
	vec3 geometricNormal = normalize(v_Normal);
	vec3 V = normalize(u_View.CameraPosition.xyz - v_WorldPosition);
	vec3 F0 = mix(vec3(0.04), surface.Albedo, surface.Metallic);
	vec3 color = vec3(0.0);
	float viewDepth = -(u_View.View * vec4(v_WorldPosition, 1.0)).z;
	bool clustered = u_View.ClusterGrid.w != 0u;

	// Directional light with cascaded soft shadows.
	if (u_View.Sun.Direction.w > 0.5)
	{
		float shadow = ComputeShadow(v_WorldPosition, geometricNormal, viewDepth, gl_FragCoord.xy);
		vec3 L = -normalize(u_View.Sun.Direction.xyz);
		color += EvaluateSurfaceLight(surface, V, L, u_View.Sun.Color.rgb * shadow, F0);
	}

	// Additional directional lights (unshadowed), then point and spot lights.
	uint lightCount = uint(u_View.EnvironmentParams.w);
	uint clusterOffset = 0u;
	if (clustered)
	{
		// Use the active render area, not the backing allocation or output dimensions.
		uvec2 tile = min(uvec2(gl_FragCoord.xy / u_View.ViewportSize.xy * vec2(u_View.ClusterGrid.xy)), u_View.ClusterGrid.xy - 1u);
		uint slice = ClusterSlice(u_View.ClusterDepths, viewDepth, u_View.ClusterGrid.z);
		clusterOffset = ClusterListOffset(ClusterIndex(uvec3(tile, slice), u_View.ClusterGrid.xyz));
		lightCount = u_Lights.ClusterWords[clusterOffset];
	}
	uint visitedLights = 0u;
	for (uint i = 0u; i < lightCount; i++)
	{
		visitedLights++;
		uint lightIndex = clustered ? u_Lights.ClusterWords[clusterOffset + 1u + i] : i;
		LightData light = u_Lights.Lights[lightIndex];
		if (light.Color.w == LIGHT_TYPE_DIRECTIONAL)
		{
			color += EvaluateSurfaceLight(surface, V, -normalize(light.Direction.xyz), light.Color.rgb, F0);
			continue;
		}
		vec3 toLight = light.Position.xyz - v_WorldPosition;
		float distance = length(toLight);
		if (distance > light.Position.w)
			continue;
		vec3 L = toLight / max(distance, 1e-4);
		float attenuation = RangeAttenuation(distance, light.Position.w);
		if (light.Color.w == LIGHT_TYPE_SPOT)
		{
			float cosAngle = dot(-L, normalize(light.Direction.xyz));
			attenuation *= smoothstep(light.Direction.w, light.Params.x, cosAngle);
		}
		if (attenuation > 0.0)
			attenuation *= ComputeLocalShadow(light, v_WorldPosition, geometricNormal);
		color += EvaluateSurfaceLight(surface, V, L, light.Color.rgb * attenuation, F0);
	}

	if (u_View.ClusterDepths.w > 0.5)
	{
		o_Color = vec4(vec3(float(visitedLights) / float(MAX_LIGHTS)), 1.0);
		return;
	}

	// Image-based ambient lighting (diffuse irradiance + split-sum specular), occluded by SSAO.
	float NdotV = max(dot(N, V), 1e-4);
	vec3 F = FresnelSchlickRoughness(NdotV, F0, surface.Roughness);
	vec3 kD = (1.0 - F) * (1.0 - surface.Metallic);
	float rotation = u_View.EnvironmentParams.x;
	vec3 irradiance = texture(samplerCube(u_IrradianceMap, u_LinearSampler), RotateY(N, -rotation)).rgb;
	vec3 R = reflect(-V, N);
	float maxMip = u_View.EnvironmentParams.y - 1.0;
	vec3 prefiltered = textureLod(samplerCube(u_PrefilteredMap, u_LinearSampler), RotateY(R, -rotation), surface.Roughness * maxMip).rgb;
	vec2 brdf = texture(sampler2D(u_BRDFLut, u_LinearSampler), vec2(NdotV, surface.Roughness)).rg;
	vec3 ambient = (kD * irradiance * surface.Albedo * (1.0 - surface.Transmission) + prefiltered * (F * brdf.x + brdf.y)) * u_View.AmbientColor.w;
	ambient += u_View.AmbientColor.rgb * surface.Albedo * (1.0 - surface.Transmission);
	if (surface.Clearcoat > 0.0)
	{
		float coatNV = max(dot(surface.ClearcoatNormal, V), 1e-4);
		float coatF = FresnelSchlick(coatNV, vec3(0.04)).r;
		vec3 coatR = RotateY(reflect(-V, surface.ClearcoatNormal), -rotation);
		vec3 coatEnv = textureLod(samplerCube(u_PrefilteredMap, u_LinearSampler), coatR, surface.ClearcoatRoughness * maxMip).rgb;
		vec2 coatBRDF = texture(sampler2D(u_BRDFLut, u_LinearSampler), vec2(coatNV, surface.ClearcoatRoughness)).rg;
		ambient = ambient * (1.0 - surface.Clearcoat * coatF) + surface.Clearcoat * coatEnv * (0.04 * coatBRDF.x + coatBRDF.y) * u_View.AmbientColor.w;
	}

	float occlusion = surface.Occlusion;
	if (u_View.EnvironmentParams.z > 0.5)
		occlusion *= texture(sampler2D(u_SSAO, u_LinearSampler), gl_FragCoord.xy * u_View.ViewportSize.zw).r;
	color += ambient * occlusion;
	color += surface.Emissive;

	float coverage = material.Extensions.w > 0.5 ? surface.Alpha : 1.0;
	o_Color = vec4(color * coverage, coverage * (1.0 - surface.Transmission));
}
