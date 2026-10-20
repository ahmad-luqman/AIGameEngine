#version 450
#include "Common.glsl"

// Unlit: the color is linear HDR radiance; exposure and tonemapping follow in the tonemap pass.

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
		vec3 lighting = u_View.AmbientColor.rgb;
		if (u_View.Sun.Direction.w > 0.5) lighting += u_View.Sun.Color.rgb * abs(dot(normal, -u_View.Sun.Direction.xyz));
		for (uint i = 0u; i < uint(u_View.EnvironmentParams.w); ++i)
		{
			LightData light = u_Lights.Lights[i];
			vec3 delta = light.Position.xyz - v_Position;
			float distance = length(delta);
			vec3 direction = delta / max(distance, 0.0001);
			float attenuation = pow(clamp(1.0 - pow(distance / max(light.Position.w, 0.0001), 4.0), 0.0, 1.0), 2.0) / max(distance * distance, 0.01);
			if (light.Color.w == LIGHT_TYPE_DIRECTIONAL) { direction = -light.Direction.xyz; attenuation = 1.0; }
			else if (light.Color.w == LIGHT_TYPE_SPOT) attenuation *= smoothstep(light.Direction.w, max(light.Direction.w + 0.0001, light.Params.x), dot(-direction, light.Direction.xyz));
			lighting += light.Color.rgb * attenuation * abs(dot(normal, direction));
		}
		o_Color.rgb *= lighting;
	}
}
