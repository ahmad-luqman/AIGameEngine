#ifndef BASALT_RENDER_AREA_GLSL
#define BASALT_RENDER_AREA_GLSL

// Dynamic resolution renders the scene into the top-left uvScale fraction (render size / target size) of its
// targets. Clamps a target UV to the centres of that area's edge texels. When the area fills the targets
// this equals clamp-to-edge addressing. Use SampleRenderArea for filtering images with undefined padding.
vec2 ClampToRenderArea(vec2 targetUV, vec2 uvScale, ivec2 targetSize)
{
	vec2 halfTexel = 0.5 / vec2(targetSize);
	return clamp(targetUV, halfTexel, uvScale - halfTexel);
}

// Clamp integer taps as well as coordinates: filtering at an edge centre can still touch undefined
// padding (including NaNs), especially when an active dimension is one pixel.
vec4 SampleRenderArea(texture2D source, sampler sourceSampler, vec2 uv, vec2 activeSize)
{
	vec2 position = clamp(uv * activeSize - 0.5, vec2(0), activeSize - 1.0);
	ivec2 lower = ivec2(floor(position));
	ivec2 upper = min(lower + 1, ivec2(activeSize) - 1);
	vec2 weight = fract(position);
	vec4 top = mix(texelFetch(sampler2D(source, sourceSampler), lower, 0), texelFetch(sampler2D(source, sourceSampler), ivec2(upper.x, lower.y), 0), weight.x);
	vec4 bottom = mix(texelFetch(sampler2D(source, sourceSampler), ivec2(lower.x, upper.y), 0), texelFetch(sampler2D(source, sourceSampler), upper, 0), weight.x);
	return mix(top, bottom, weight.y);
}

#endif
