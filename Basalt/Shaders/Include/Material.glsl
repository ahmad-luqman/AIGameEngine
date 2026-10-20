// Material texture bindings (set 1) and surface evaluation shared by the prepass and lighting pass.
#ifndef BASALT_MATERIAL_GLSL
#define BASALT_MATERIAL_GLSL

layout(set = 1, binding = 0) uniform texture2D u_AlbedoMap;
layout(set = 1, binding = 1) uniform texture2D u_NormalMap;
layout(set = 1, binding = 2) uniform texture2D u_MetallicRoughnessMap;
layout(set = 1, binding = 3) uniform texture2D u_OcclusionMap;
layout(set = 1, binding = 4) uniform texture2D u_EmissiveMap;
layout(set = 1, binding = 5) uniform sampler u_MaterialSampler;
layout(set = 1, binding = 6) uniform texture2D u_ClearcoatMap;
layout(set = 1, binding = 7) uniform texture2D u_ClearcoatRoughnessMap;
layout(set = 1, binding = 8) uniform texture2D u_ClearcoatNormalMap;
layout(set = 1, binding = 9) uniform texture2D u_TransmissionMap;

struct Surface
{
	vec3 Albedo;
	float Alpha;
	vec3 Normal;
	float Metallic;
	float Roughness;
	float Occlusion;
	vec3 Emissive;
	float Clearcoat;
	float ClearcoatRoughness;
	vec3 ClearcoatNormal;
	float Transmission;
};

vec2 MaterialUV(MaterialData material, int slot, vec2 uv, vec2 uv1)
{
	vec4 row = material.UVRows[slot * 2];
	vec3 p = vec3(row.w > 0.5 ? uv1 : uv, 1.0);
	return vec2(dot(row.xyz, p), dot(material.UVRows[slot * 2 + 1].xyz, p));
}

vec3 MapNormal(vec3 N, vec4 tangent, vec3 sampled, vec2 uv, vec3 position, float scale, bool transformed)
{
	vec3 T = normalize(tangent.xyz - N * dot(N, tangent.xyz));
	vec3 B = cross(N, T) * tangent.w;
	if (transformed)
	{
		vec3 dx = dFdx(position), dy = dFdy(position);
		vec2 tx = dFdx(uv), ty = dFdy(uv);
		float determinant = tx.x * ty.y - tx.y * ty.x;
		if (abs(determinant) > 1e-10)
		{
			T = normalize((dx * ty.y - dy * tx.y) / determinant);
			T = normalize(T - N * dot(N, T));
			B = cross(N, T) * (dot(cross(N, T), (dy * tx.x - dx * ty.x) / determinant) < 0.0 ? 1.0 : -1.0);
		}
	}
	sampled = sampled * 2.0 - 1.0;
	sampled.xy *= scale;
	vec3 mapped = mat3(T, B, N) * sampled;
	return dot(mapped, mapped) > 1e-12 ? normalize(mapped) : N;
}

float MaterialAlpha(MaterialData material, vec2 uv, vec2 uv1)
{
	float alpha = material.AlbedoColor.a;
	if ((uint(material.Params.w) & MATERIAL_HAS_ALBEDO_MAP) != 0u)
		alpha *= texture(sampler2D(u_AlbedoMap, u_MaterialSampler), MaterialUV(material, 0, uv, uv1)).a;
	return alpha;
}

Surface EvaluateSurface(MaterialData material, vec3 normal, vec4 tangent, vec2 uv, vec2 uv1, vec3 position, bool frontFacing)
{
	uint flags = uint(material.Params.w);
	Surface surface;

	vec4 albedo = material.AlbedoColor;
	if ((flags & MATERIAL_HAS_ALBEDO_MAP) != 0u)
		albedo *= texture(sampler2D(u_AlbedoMap, u_MaterialSampler), MaterialUV(material, 0, uv, uv1));
	surface.Albedo = albedo.rgb;
	surface.Alpha = albedo.a;

	vec3 N = normalize(normal);
	if (!frontFacing)
		N = -N;
	if ((flags & MATERIAL_HAS_NORMAL_MAP) != 0u)
	{
		vec2 normalUV = MaterialUV(material, 1, uv, uv1);
		bool transformed = material.UVRows[2] != vec4(1,0,0,0) || material.UVRows[3] != vec4(0,1,0,0);
		N = MapNormal(N, tangent, texture(sampler2D(u_NormalMap, u_MaterialSampler), normalUV).xyz, normalUV, position, material.NormalParams.x, transformed);
	}
	surface.Normal = N;

	surface.Metallic = material.Params.x;
	surface.Roughness = material.Params.y;
	if ((flags & MATERIAL_HAS_MR_MAP) != 0u)
	{
		vec4 mr = texture(sampler2D(u_MetallicRoughnessMap, u_MaterialSampler), MaterialUV(material, 2, uv, uv1));
		surface.Roughness *= mr.g;
		surface.Metallic *= mr.b;
	}
	surface.Roughness = clamp(surface.Roughness, 0.045, 1.0);
	surface.Metallic = clamp(surface.Metallic, 0.0, 1.0);

	surface.Occlusion = 1.0;
	if ((flags & MATERIAL_HAS_OCCLUSION_MAP) != 0u)
		surface.Occlusion = mix(1.0, texture(sampler2D(u_OcclusionMap, u_MaterialSampler), MaterialUV(material, 3, uv, uv1)).r, material.NormalParams.y);

	surface.Emissive = material.Emissive.rgb;
	if ((flags & MATERIAL_HAS_EMISSIVE_MAP) != 0u)
		surface.Emissive *= texture(sampler2D(u_EmissiveMap, u_MaterialSampler), MaterialUV(material, 4, uv, uv1)).rgb;
	surface.Clearcoat = material.Extensions.x;
	surface.ClearcoatRoughness = material.Extensions.y;
	surface.Transmission = material.Extensions.z;
	if ((flags & MATERIAL_HAS_CLEARCOAT_MAP) != 0u) surface.Clearcoat *= texture(sampler2D(u_ClearcoatMap, u_MaterialSampler), MaterialUV(material, 5, uv, uv1)).r;
	if ((flags & MATERIAL_HAS_CLEARCOAT_ROUGHNESS_MAP) != 0u) surface.ClearcoatRoughness *= texture(sampler2D(u_ClearcoatRoughnessMap, u_MaterialSampler), MaterialUV(material, 6, uv, uv1)).g;
	surface.ClearcoatRoughness = clamp(surface.ClearcoatRoughness, 0.045, 1.0);
	surface.ClearcoatNormal = frontFacing ? normalize(normal) : -normalize(normal);
	if ((flags & MATERIAL_HAS_CLEARCOAT_NORMAL_MAP) != 0u)
	{
		vec2 coatUV = MaterialUV(material, 7, uv, uv1);
		surface.ClearcoatNormal = MapNormal(surface.ClearcoatNormal, tangent, texture(sampler2D(u_ClearcoatNormalMap, u_MaterialSampler), coatUV).xyz, coatUV, position, material.NormalParams.z, true);
	}
	if ((flags & MATERIAL_HAS_TRANSMISSION_MAP) != 0u) surface.Transmission *= texture(sampler2D(u_TransmissionMap, u_MaterialSampler), MaterialUV(material, 8, uv, uv1)).r;
	surface.Transmission *= 1.0 - surface.Metallic;
	return surface;
}

#endif
