#version 450

// Resamples the scene depth (render resolution) to the output resolution, so overlays drawn on the output
// image are depth-tested against the scene when it renders at a different size.
layout(set = 0, binding = 0) uniform texture2D u_Depth;
layout(set = 0, binding = 1) uniform sampler u_PointClamp;

layout(push_constant) uniform DepthUpsamplePush
{
	vec2 UVScale; // render size / target size (the scene covers the depth target's top-left corner)
} u_Push;

layout(location = 0) in vec2 v_TexCoord;

void main()
{
	vec2 halfTexel = 0.5 / vec2(textureSize(sampler2D(u_Depth, u_PointClamp), 0));
	vec2 uv = clamp(v_TexCoord * u_Push.UVScale, halfTexel, u_Push.UVScale - halfTexel);
	gl_FragDepth = textureLod(sampler2D(u_Depth, u_PointClamp), uv, 0.0).r;
}
