#version 450
#include "Common.glsl"

// Camera-facing particle billboards, pulled from a storage buffer: six vertices (two triangles) per
// particle, no vertex buffer. Set 1 (see SceneRenderer.cpp, keep in sync): 0 particles (SSBO),
// 1 texture, 2 sampler. u_Push.DrawIndex holds the batch's first particle.

struct ParticleData
{
	vec4 PositionSize;    // xyz: world position, w: billboard size
	vec4 Color;           // linear HDR color, a: opacity
};

layout(std430, set = 1, binding = 0) readonly buffer ParticleBuffer
{
	ParticleData Particles[];
} u_Particles;

layout(location = 0) out vec2 v_TexCoord;
layout(location = 1) out vec4 v_Color;

const vec2 Corners[6] = vec2[](vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(0.5, 0.5), vec2(-0.5, -0.5), vec2(0.5, 0.5), vec2(-0.5, 0.5));

void main()
{
	ParticleData particle = u_Particles.Particles[u_Push.DrawIndex + uint(gl_VertexIndex) / 6u];
	vec2 corner = Corners[gl_VertexIndex % 6];
	// The camera's right and up axes (world space) span the billboard.
	vec3 right = u_View.InverseView[0].xyz;
	vec3 up = u_View.InverseView[1].xyz;
	vec3 position = particle.PositionSize.xyz + (right * corner.x + up * corner.y) * particle.PositionSize.w;
	gl_Position = u_View.ViewProjection * vec4(position, 1.0);
	// Texture (0, 0) is the top-left texel, so the billboard's top edge (corner.y = 0.5) maps to v = 0.
	v_TexCoord = vec2(corner.x + 0.5, 0.5 - corner.y);
	v_Color = particle.Color;
}
