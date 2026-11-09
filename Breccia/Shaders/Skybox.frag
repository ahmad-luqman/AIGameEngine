#version 450
#include "Common.glsl"

layout(set = 0, binding = 9) uniform sampler u_LinearSampler;
layout(set = 0, binding = 12) uniform textureCube u_EnvironmentMap;

layout(location = 0) in vec3 v_Direction;
layout(location = 0) out vec4 o_Color;

void main()
{
	vec3 direction = RotateY(normalize(v_Direction), -u_View.EnvironmentParams.x);
	vec3 color = textureLod(samplerCube(u_EnvironmentMap, u_LinearSampler), direction, 0.0).rgb;
	o_Color = vec4(color * u_View.AmbientColor.w, 1.0);
}
