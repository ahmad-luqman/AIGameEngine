// Material texture bindings (set 1) and surface evaluation shared by the prepass and lighting pass.
#ifndef BASALT_MATERIAL_GLSL
#define BASALT_MATERIAL_GLSL

layout(set = 1, binding = 0) uniform texture2D u_AlbedoMap;
layout(set = 1, binding = 1) uniform texture2D u_NormalMap;
layout(set = 1, binding = 2) uniform texture2D u_MetallicRoughnessMap;
layout(set = 1, binding = 3) uniform texture2D u_OcclusionMap;
layout(set = 1, binding = 4) uniform texture2D u_EmissiveMap;
layout(set = 1, binding = 5) uniform sampler u_MaterialSampler;

struct Surface
{
	vec3 Albedo;
	float Alpha;
	vec3 Normal;
	float Metallic;
	float Roughness;
	float Occlusion;
	vec3 Emissive;
};

Surface EvaluateSurface(MaterialData material, vec3 normal, vec4 tangent, vec2 uv, bool frontFacing)
{
	uint flags = uint(material.Params.w);
	Surface surface;

	vec4 albedo = material.AlbedoColor;
	if ((flags & MATERIAL_HAS_ALBEDO_MAP) != 0u)
		albedo *= texture(sampler2D(u_AlbedoMap, u_MaterialSampler), uv);
	surface.Albedo = albedo.rgb;
	surface.Alpha = albedo.a;

	vec3 N = normalize(normal);
	if (!frontFacing)
		N = -N;
	if ((flags & MATERIAL_HAS_NORMAL_MAP) != 0u)
	{
		vec3 T = normalize(tangent.xyz - N * dot(N, tangent.xyz));
		vec3 B = cross(N, T) * tangent.w;
		vec3 tangentNormal = texture(sampler2D(u_NormalMap, u_MaterialSampler), uv).xyz * 2.0 - 1.0;
		N = normalize(mat3(T, B, N) * tangentNormal);
	}
	surface.Normal = N;

	surface.Metallic = material.Params.x;
	surface.Roughness = material.Params.y;
	if ((flags & MATERIAL_HAS_MR_MAP) != 0u)
	{
		vec4 mr = texture(sampler2D(u_MetallicRoughnessMap, u_MaterialSampler), uv);
		surface.Roughness *= mr.g;
		surface.Metallic *= mr.b;
	}
	surface.Roughness = clamp(surface.Roughness, 0.045, 1.0);
	surface.Metallic = clamp(surface.Metallic, 0.0, 1.0);

	surface.Occlusion = 1.0;
	if ((flags & MATERIAL_HAS_OCCLUSION_MAP) != 0u)
		surface.Occlusion = texture(sampler2D(u_OcclusionMap, u_MaterialSampler), uv).r;

	surface.Emissive = material.Emissive.rgb;
	if ((flags & MATERIAL_HAS_EMISSIVE_MAP) != 0u)
		surface.Emissive *= texture(sampler2D(u_EmissiveMap, u_MaterialSampler), uv).rgb;
	return surface;
}

#endif
