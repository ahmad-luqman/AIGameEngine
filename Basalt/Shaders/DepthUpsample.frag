#version 450

// Resamples the scene depth (render resolution) to the output resolution, so overlays drawn on the output
// image are depth-tested against the scene when it renders at a different size.
layout(set = 0, binding = 0) uniform texture2D u_Depth;
layout(set = 0, binding = 1) uniform sampler u_PointClamp;

layout(location = 0) in vec2 v_TexCoord;

void main()
{
	gl_FragDepth = textureLod(sampler2D(u_Depth, u_PointClamp), v_TexCoord, 0.0).r;
}
