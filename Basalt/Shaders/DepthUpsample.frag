#version 450
#include "RenderArea.glsl"

// Resamples the scene depth (render resolution) to the output resolution, so overlays drawn on the output
// image are depth-tested against the scene when it renders at a different size.
layout(set = 0, binding = 0) uniform texture2D u_Depth;
layout(set = 0, binding = 1) uniform sampler u_PointClamp;

layout(push_constant) uniform DepthUpsamplePush
{
	vec2 UVScale; // render size / target size (RenderArea.glsl)
} u_Push;

layout(location = 0) in vec2 v_TexCoord;

void main()
{
	vec2 uv = ClampToRenderArea(v_TexCoord * u_Push.UVScale, u_Push.UVScale, textureSize(sampler2D(u_Depth, u_PointClamp), 0));
	gl_FragDepth = textureLod(sampler2D(u_Depth, u_PointClamp), uv, 0.0).r;
}
