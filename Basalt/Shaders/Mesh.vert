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
	// Cofactors give the inverse-transpose direction without division by a tiny determinant.
	// Normalize first to prevent squared/cubed scale underflow in cofactors and determinant.
	vec3 largest = max(max(abs(linear[0]), abs(linear[1])), abs(linear[2]));
	float scale = max(largest.x, max(largest.y, largest.z));
	mat3 stable = scale > 0.0 ? linear / scale : mat3(0.0);
	float orientation = determinant(stable);
	mat3 cofactors = mat3(cross(stable[1], stable[2]), cross(stable[2], stable[0]), cross(stable[0], stable[1]));
	vec3 normal = mat3(draw.Normal) * cofactors * a_Normal * (orientation < 0.0 ? -1.0 : 1.0);
	v_Normal = StableNormalize(normal, vec3(0.0, 1.0, 0.0));
	vec3 tangent = mat3(draw.Model) * linear * a_Tangent.xyz;
	tangent = StableNormalize(tangent, cross(v_Normal, abs(v_Normal.x) < 0.9 ? vec3(1,0,0) : vec3(0,1,0)));
	v_Tangent = vec4(tangent, a_Tangent.w * (orientation < 0.0 ? -1.0 : 1.0));
	v_TexCoord = a_TexCoord;
	v_MaterialIndex = draw.Indices.x;
	gl_Position = u_View.ViewProjection * world;
}
