#version 450

// Unlit: the color is linear HDR radiance; exposure and tonemapping follow in the tonemap pass.

layout(set = 1, binding = 1) uniform texture2D u_ParticleTexture;
layout(set = 1, binding = 2) uniform sampler u_ParticleSampler;

layout(location = 0) in vec2 v_TexCoord;
layout(location = 1) in vec4 v_Color;

layout(location = 0) out vec4 o_Color;

void main()
{
	o_Color = v_Color * texture(sampler2D(u_ParticleTexture, u_ParticleSampler), v_TexCoord);
}
