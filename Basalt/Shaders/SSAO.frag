#version 450

// Normal-oriented hemisphere SSAO on view-space positions reconstructed from depth.
layout(set = 0, binding = 0) uniform SSAOConstants
{
	mat4 Projection;
	mat4 InverseProjection;
	vec4 Kernel[32];
	vec4 Params;        // x: radius, y: bias, z: intensity, w: sample count
	vec4 ScreenSize;    // xy: size, zw: noise scale
} u_SSAO;

layout(set = 0, binding = 1) uniform texture2D u_Depth;
layout(set = 0, binding = 2) uniform texture2D u_Normals;
layout(set = 0, binding = 3) uniform texture2D u_Noise;
layout(set = 0, binding = 4) uniform sampler u_PointClamp;
layout(set = 0, binding = 5) uniform sampler u_PointWrap;

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out float o_Occlusion;

vec3 ViewPosition(vec2 uv)
{
	float depth = textureLod(sampler2D(u_Depth, u_PointClamp), uv, 0.0).r;
	vec4 clip = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
	vec4 view = u_SSAO.InverseProjection * clip;
	return view.xyz / view.w;
}

void main()
{
	float depth = textureLod(sampler2D(u_Depth, u_PointClamp), v_TexCoord, 0.0).r;
	if (depth >= 1.0)
	{
		o_Occlusion = 1.0;
		return;
	}

	vec3 position = ViewPosition(v_TexCoord);
	vec3 normal = normalize(textureLod(sampler2D(u_Normals, u_PointClamp), v_TexCoord, 0.0).xyz);
	vec3 random = vec3(textureLod(sampler2D(u_Noise, u_PointWrap), v_TexCoord * u_SSAO.ScreenSize.zw, 0.0).xy * 2.0 - 1.0, 0.0);

	vec3 tangent = normalize(random - normal * dot(random, normal));
	vec3 bitangent = cross(normal, tangent);
	mat3 TBN = mat3(tangent, bitangent, normal);

	float radius = u_SSAO.Params.x;
	float bias = u_SSAO.Params.y;
	int sampleCount = int(u_SSAO.Params.w);
	float occlusion = 0.0;
	for (int i = 0; i < sampleCount; i++)
	{
		vec3 samplePosition = position + TBN * u_SSAO.Kernel[i].xyz * radius;
		vec4 offset = u_SSAO.Projection * vec4(samplePosition, 1.0);
		vec2 sampleNdc = offset.xy / offset.w;
		vec2 sampleUV = vec2(sampleNdc.x * 0.5 + 0.5, 0.5 - sampleNdc.y * 0.5);
		if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThan(sampleUV, vec2(1.0))))
			continue;
		float sceneDepth = ViewPosition(sampleUV).z;
		float rangeCheck = smoothstep(0.0, 1.0, radius / max(abs(position.z - sceneDepth), 1e-4));
		occlusion += (sceneDepth >= samplePosition.z + bias ? 1.0 : 0.0) * rangeCheck;
	}

	float ao = 1.0 - occlusion / float(max(sampleCount, 1));
	o_Occlusion = pow(clamp(ao, 0.0, 1.0), u_SSAO.Params.z);
}
