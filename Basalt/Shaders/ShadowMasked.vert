#version 450
#include "Common.glsl"
#include "Skinning.glsl"
#include "Morphing.glsl"

// Shadow pass for alpha-tested materials: passes UVs so the fragment shader can discard.
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;
layout(location = 2) in uvec4 a_Joints;
layout(location = 3) in vec4 a_Weights;
layout(location = 4) in vec2 a_TexCoord1;

layout(location = 0) out vec2 v_TexCoord;
layout(location = 1) flat out uint v_MaterialIndex;
layout(location = 2) out vec2 v_TexCoord1;

void main()
{
	DrawData draw = u_Draws.Draws[u_Push.DrawIndex + gl_InstanceIndex];
	vec3 position = a_Position;
	vec3 normal = vec3(0.0);
	ApplyMorphTargets(draw, position, normal);
	v_TexCoord = a_TexCoord;
	v_TexCoord1 = a_TexCoord1;
	v_MaterialIndex = draw.Indices.x;
	gl_Position = ShadowViewProjection(u_Push.ShadowViewIndex) * draw.Model * SkinMatrix(draw, a_Joints, a_Weights) * vec4(position, 1.0);
}
