#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(set = 0, binding = 0) uniform utexture2D u_Ids;
layout(push_constant) uniform OutlinePush
{
	uvec2 RenderSize;
	uvec2 OutputSize;
	uint Selected;
	uint Padding;
} u_Push;
layout(location = 0) out vec4 o_Color;
bool SelectedAt(ivec2 pixel)
{
	// A clipped surface continues outside the view; the viewport border is not a silhouette.
	pixel = clamp(pixel, ivec2(0), ivec2(u_Push.OutputSize) - 1);
	ivec2 texel = ivec2((vec2(pixel) + 0.5) * vec2(u_Push.RenderSize) / vec2(u_Push.OutputSize));
	texel = clamp(texel, ivec2(0), ivec2(u_Push.RenderSize) - 1);
	return texelFetch(u_Ids, texel, 0).r == u_Push.Selected;
}
void main()
{
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	bool center = SelectedAt(pixel);
	bool edge = false;
	for (int y = -1; y <= 1; ++y)
		for (int x = -1; x <= 1; ++x)
			edge = edge || SelectedAt(pixel + ivec2(x, y)) != center;
	if (!edge)
		discard;
	o_Color = vec4(1.0, 0.75, 0.1, 1.0);
}
