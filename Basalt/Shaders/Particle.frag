#version 450
#include "Common.glsl"
#include "PBR.glsl"
#include "LightClusters.glsl"
layout(set = 0, binding = 5) uniform textureCube u_IrradianceMap;
layout(set = 0, binding = 9) uniform sampler u_LinearSampler;

// Unlit color is linear HDR radiance; optional two-sided diffuse lighting scales it; exposure and tonemapping follow in the tonemap pass.

layout(set = 1, binding = 1) uniform texture2D u_ParticleTexture;
layout(set = 1, binding = 2) uniform sampler u_ParticleSampler;

layout(location = 0) in vec2 v_TexCoord;
layout(location = 1) in vec4 v_Color;
layout(location = 2) in vec3 v_Position;
layout(location = 3) flat in vec2 v_Effects;
layout(set = 1, binding = 3) uniform texture2D u_SceneDepth;

layout(location = 0) out vec4 o_Color;

void main()
{
	o_Color = v_Color * texture(sampler2D(u_ParticleTexture, u_ParticleSampler), v_TexCoord);
	if (v_Effects.x > 0.0)
	{
		float depth = texelFetch(sampler2D(u_SceneDepth, u_ParticleSampler), ivec2(gl_FragCoord.xy), 0).r;
		vec2 ndc = vec2(gl_FragCoord.x / u_View.ViewportSize.x * 2.0 - 1.0, 1.0 - gl_FragCoord.y / u_View.ViewportSize.y * 2.0);
		vec4 scene = u_View.InverseProjection * vec4(ndc, depth, 1.0);
		float particleDepth = -(u_View.View * vec4(v_Position, 1.0)).z;
		o_Color.a *= clamp((-scene.z / scene.w - particleDepth) / v_Effects.x, 0.0, 1.0);
	}
	if (v_Effects.y > 0.5)
	{
		// Two-sided diffuse billboards: smoke receives light from either side, without a specular lobe.
		vec3 normal = normalize(u_View.InverseView[2].xyz);
		vec3 lighting = u_View.AmbientColor.rgb + texture(samplerCube(u_IrradianceMap, u_LinearSampler), RotateY(normal, -u_View.EnvironmentParams.x)).rgb * u_View.AmbientColor.w;
		if (u_View.Sun.Direction.w > 0.5) lighting += u_View.Sun.Color.rgb / PI * abs(dot(normal, -normalize(u_View.Sun.Direction.xyz)));
		bool clustered = u_View.ClusterGrid.w != 0u;
		uint count = uint(u_View.EnvironmentParams.w), offset = 0u;
		if (clustered) {
			uvec2 tile = min(uvec2(gl_FragCoord.xy / u_View.ViewportSize.xy * vec2(u_View.ClusterGrid.xy)), u_View.ClusterGrid.xy-1u);
			uint slice = ClusterSlice(u_View.ClusterDepths, -(u_View.View * vec4(v_Position,1)).z, u_View.ClusterGrid.z);
			offset = ClusterListOffset(ClusterIndex(uvec3(tile,slice),u_View.ClusterGrid.xyz)); count = u_Lights.ClusterWords[offset];
		}
		for (uint i = 0u; i < count; ++i)
		{
			LightData light = u_Lights.Lights[clustered ? u_Lights.ClusterWords[offset+1u+i] : i];
			vec3 delta = light.Position.xyz - v_Position;
			float distance = length(delta);
			vec3 direction = delta / max(distance, 0.0001);
			float attenuation = RangeAttenuation(distance, light.Position.w);
			if (light.Color.w == LIGHT_TYPE_DIRECTIONAL) { direction = -normalize(light.Direction.xyz); attenuation = 1.0; }
			else if (light.Color.w == LIGHT_TYPE_SPOT) attenuation *= smoothstep(light.Direction.w, max(light.Direction.w + 0.0001, light.Params.x), dot(-direction, normalize(light.Direction.xyz)));
			lighting += light.Color.rgb * attenuation / PI * abs(dot(normal, direction));
		}
		o_Color.rgb *= lighting;
	}
}
