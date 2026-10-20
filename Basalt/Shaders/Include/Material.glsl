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

// UV of a MATERIAL_SLOT_* map: its affine transform applied to UV0, or to UV1 when the first row's w is set.
vec2 MaterialUV(MaterialData material, int slot, vec2 uv, vec2 uv1)
{
	vec4 row = material.UVRows[slot * 2];
	vec3 p = vec3(row.w > 0.5 ? uv1 : uv, 1.0);
	return vec2(dot(row.xyz, p), dot(material.UVRows[slot * 2 + 1].xyz, p));
}

// True when a map reads different UVs than the vertex tangents were built for (UV1 or a non-identity transform).
// The identity rows also have w = 0, so UV1 counts as transformed.
bool HasUVTransform(MaterialData material, int slot)
{
	return material.UVRows[slot * 2] != vec4(1, 0, 0, 0) || material.UVRows[slot * 2 + 1] != vec4(0, 1, 0, 0);
}

vec4 SampleMap(texture2D map, MaterialData material, int slot, vec2 uv, vec2 uv1)
{
	return texture(sampler2D(map, u_MaterialSampler), MaterialUV(material, slot, uv, uv1));
}

// transformed: the map's UVs differ from those the vertex tangent was built for, so the tangent frame is derived
// from screen-space derivatives of uv and position instead.
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
			// The second solution is dP/dv. glTF UVs grow downward, so the map's +Y (image up) is -dP/dv: keep
			// cross(N, T) when it already points against dP/dv, as MeshSource's tangent.w does.
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
		alpha *= SampleMap(u_AlbedoMap, material, MATERIAL_SLOT_ALBEDO, uv, uv1).a;
	return alpha;
}

Surface EvaluateSurface(MaterialData material, vec3 normal, vec4 tangent, vec2 uv, vec2 uv1, vec3 position, bool frontFacing)
{
	uint flags = uint(material.Params.w);
	Surface surface;

	vec4 albedo = material.AlbedoColor;
	if ((flags & MATERIAL_HAS_ALBEDO_MAP) != 0u)
		albedo *= SampleMap(u_AlbedoMap, material, MATERIAL_SLOT_ALBEDO, uv, uv1);
	surface.Albedo = albedo.rgb;
	surface.Alpha = albedo.a;

	vec3 geometricNormal = frontFacing ? normalize(normal) : -normalize(normal);
	vec3 N = geometricNormal;
	if ((flags & MATERIAL_HAS_NORMAL_MAP) != 0u)
	{
		vec2 normalUV = MaterialUV(material, MATERIAL_SLOT_NORMAL, uv, uv1);
		vec3 sampled = texture(sampler2D(u_NormalMap, u_MaterialSampler), normalUV).xyz;
		N = MapNormal(N, tangent, sampled, normalUV, position, material.NormalParams.x, HasUVTransform(material, MATERIAL_SLOT_NORMAL));
	}
	surface.Normal = N;

	surface.Metallic = material.Params.x;
	surface.Roughness = material.Params.y;
	if ((flags & MATERIAL_HAS_MR_MAP) != 0u)
	{
		vec4 mr = SampleMap(u_MetallicRoughnessMap, material, MATERIAL_SLOT_METALLIC_ROUGHNESS, uv, uv1);
		surface.Roughness *= mr.g;
		surface.Metallic *= mr.b;
	}
	surface.Roughness = clamp(surface.Roughness, 0.045, 1.0);
	surface.Metallic = clamp(surface.Metallic, 0.0, 1.0);

	surface.Occlusion = 1.0;
	if ((flags & MATERIAL_HAS_OCCLUSION_MAP) != 0u)
		surface.Occlusion = mix(1.0, SampleMap(u_OcclusionMap, material, MATERIAL_SLOT_OCCLUSION, uv, uv1).r, material.NormalParams.y);

	surface.Emissive = material.Emissive.rgb;
	if ((flags & MATERIAL_HAS_EMISSIVE_MAP) != 0u)
		surface.Emissive *= SampleMap(u_EmissiveMap, material, MATERIAL_SLOT_EMISSIVE, uv, uv1).rgb;

	surface.Clearcoat = material.Extensions.x;
	if ((flags & MATERIAL_HAS_CLEARCOAT_MAP) != 0u)
		surface.Clearcoat *= SampleMap(u_ClearcoatMap, material, MATERIAL_SLOT_CLEARCOAT, uv, uv1).r;
	surface.ClearcoatRoughness = material.Extensions.y;
	if ((flags & MATERIAL_HAS_CLEARCOAT_ROUGHNESS_MAP) != 0u)
		surface.ClearcoatRoughness *= SampleMap(u_ClearcoatRoughnessMap, material, MATERIAL_SLOT_CLEARCOAT_ROUGHNESS, uv, uv1).g;
	surface.ClearcoatRoughness = clamp(surface.ClearcoatRoughness, 0.045, 1.0);
	surface.ClearcoatNormal = geometricNormal;
	if ((flags & MATERIAL_HAS_CLEARCOAT_NORMAL_MAP) != 0u)
	{
		vec2 coatUV = MaterialUV(material, MATERIAL_SLOT_CLEARCOAT_NORMAL, uv, uv1);
		vec3 sampled = texture(sampler2D(u_ClearcoatNormalMap, u_MaterialSampler), coatUV).xyz;
		surface.ClearcoatNormal = MapNormal(geometricNormal, tangent, sampled, coatUV, position, material.NormalParams.z, HasUVTransform(material, MATERIAL_SLOT_CLEARCOAT_NORMAL));
	}

	surface.Transmission = material.Extensions.z;
	if ((flags & MATERIAL_HAS_TRANSMISSION_MAP) != 0u)
		surface.Transmission *= SampleMap(u_TransmissionMap, material, MATERIAL_SLOT_TRANSMISSION, uv, uv1).r;
	surface.Transmission *= 1.0 - surface.Metallic;
	return surface;
}

#endif
