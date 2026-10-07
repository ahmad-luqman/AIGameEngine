#version 450
#include "Common.glsl"

layout(location = 0) out vec3 v_Direction;

void main()
{
	vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
	// At the far plane so the depth test keeps it behind all geometry.
	gl_Position = vec4(ndc, 1.0, 1.0);
	vec4 viewDirection = u_View.InverseProjection * vec4(ndc, 1.0, 1.0);
	v_Direction = (u_View.InverseView * vec4(viewDirection.xyz / viewDirection.w, 0.0)).xyz;
}
