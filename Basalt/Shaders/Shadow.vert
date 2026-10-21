#version 450
#include "Common.glsl"
#include "Skinning.glsl"
#include "Morphing.glsl"

layout(location = 0) in vec3 a_Position;
layout(location = 1) in uvec4 a_Joints;
layout(location = 2) in vec4 a_Weights;

void main()
{
	DrawData draw = u_Draws.Draws[u_Push.DrawIndex + gl_InstanceIndex];
	vec3 position = a_Position;
	vec3 normal = vec3(0.0);
	ApplyMorphTargets(draw, position, normal);
	gl_Position = ShadowViewProjection(u_Push.ShadowViewIndex) * draw.Model * SkinMatrix(draw, a_Joints, a_Weights) * vec4(position, 1.0);
}
