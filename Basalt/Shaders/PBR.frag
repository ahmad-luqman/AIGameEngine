#version 450
#include "Common.glsl"
#include "Material.glsl"
#include "PBR.glsl"
#include "Shadows.glsl"

layout(set = 0, binding = 5) uniform textureCube u_IrradianceMap;
layout(set = 0, binding = 6) uniform textureCube u_PrefilteredMap;
layout(set = 0, binding = 7) uniform texture2D u_BRDFLut;
layout(set = 0, binding = 8) uniform texture2D u_SSAO;
layout(set = 0, binding = 9) uniform sampler u_LinearSampler;

layout(location = 0) in vec3 v_WorldPosition;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec4 v_Tangent;
layout(location = 3) in vec2 v_TexCoord;
layout(location = 4) flat in uint v_MaterialIndex;

layout(location = 0) out vec4 o_Color;

void main()
{
	MaterialData material = u_Materials.Materials[v_MaterialIndex];
	Surface surface = EvaluateSurface(material, v_Normal, v_Tangent, v_TexCoord, gl_FrontFacing);
	if (material.Params.z > 0.0 && surface.Alpha < material.Params.z)
		discard;

	vec3 N = surface.Normal;
	vec3 V = normalize(u_View.CameraPosition.xyz - v_WorldPosition);
	vec3 F0 = mix(vec3(0.04), surface.Albedo, surface.Metallic);
	vec3 color = vec3(0.0);

	// Directional light with cascaded soft shadows.
	if (u_View.Sun.Direction.w > 0.5)
	{
		float viewDepth = -(u_View.View * vec4(v_WorldPosition, 1.0)).z;
		float shadow = ComputeShadow(v_WorldPosition, normalize(v_Normal), viewDepth, gl_FragCoord.xy);
		vec3 L = -normalize(u_View.Sun.Direction.xyz);
		color += EvaluateLight(N, V, L, u_View.Sun.Color.rgb * shadow, surface.Albedo, surface.Metallic, surface.Roughness, F0);
	}

	// Point and spot lights.
	uint lightCount = uint(u_View.EnvironmentParams.w);
	for (uint i = 0u; i < lightCount; i++)
	{
		LightData light = u_Lights.Lights[i];
		vec3 toLight = light.Position.xyz - v_WorldPosition;
		float distance = length(toLight);
		if (distance > light.Position.w)
			continue;
		vec3 L = toLight / max(distance, 1e-4);
		float attenuation = RangeAttenuation(distance, light.Position.w);
		if (light.Color.w > 0.5)
		{
			float cosAngle = dot(-L, normalize(light.Direction.xyz));
			attenuation *= smoothstep(light.Direction.w, light.Params.x, cosAngle);
		}
		color += EvaluateLight(N, V, L, light.Color.rgb * attenuation, surface.Albedo, surface.Metallic, surface.Roughness, F0);
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
	vec3 ambient = (kD * irradiance * surface.Albedo + prefiltered * (F * brdf.x + brdf.y)) * u_View.AmbientColor.w;
	ambient += u_View.AmbientColor.rgb * surface.Albedo;

	float occlusion = surface.Occlusion;
	if (u_View.EnvironmentParams.z > 0.5)
		occlusion *= texture(sampler2D(u_SSAO, u_LinearSampler), gl_FragCoord.xy * u_View.ViewportSize.zw).r;
	color += ambient * occlusion;
	color += surface.Emissive;

	o_Color = vec4(color, surface.Alpha);
}
