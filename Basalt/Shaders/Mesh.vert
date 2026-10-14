#version 450
#include "Common.glsl"
#include "Skinning.glsl"

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec4 a_Tangent;
layout(location = 3) in vec2 a_TexCoord;
layout(location = 4) in uvec4 a_Joints;
layout(location = 5) in vec4 a_Weights;

layout(location = 0) out vec3 v_WorldPosition;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec4 v_Tangent;
layout(location = 3) out vec2 v_TexCoord;
layout(location = 4) flat out uint v_MaterialIndex;

// The depth prepass and the lighting pass must produce bit-identical depth.
invariant gl_Position;

void main()
{
	DrawData draw = u_Draws.Draws[u_Push.DrawIndex];
	mat4 skin = SkinMatrix(draw, a_Joints, a_Weights);
	vec4 world = draw.Model * skin * vec4(a_Position, 1.0);
	v_WorldPosition = world.xyz;
	mat3 linear = mat3(skin);
	float determinant = determinant(linear);
	// Zero scales and collapsing blends have no inverse; retain a finite direction.
	mat3 skinNormal = abs(determinant) > 1e-8 ? transpose(inverse(linear)) : mat3(1.0);
	vec3 normal = mat3(draw.Normal) * skinNormal * a_Normal;
	v_Normal = dot(normal, normal) > 1e-16 ? normalize(normal) : vec3(0.0, 1.0, 0.0);
	vec3 tangent = mat3(draw.Model) * linear * a_Tangent.xyz;
	if (dot(tangent, tangent) < 1e-16)
		tangent = cross(v_Normal, abs(v_Normal.x) < 0.9 ? vec3(1,0,0) : vec3(0,1,0));
	v_Tangent = vec4(normalize(tangent), a_Tangent.w * (determinant < 0.0 ? -1.0 : 1.0));
	v_TexCoord = a_TexCoord;
	v_MaterialIndex = draw.Indices.x;
	gl_Position = u_View.ViewProjection * world;
}
