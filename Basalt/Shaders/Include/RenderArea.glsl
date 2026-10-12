#ifndef BASALT_RENDER_AREA_GLSL
#define BASALT_RENDER_AREA_GLSL

// Dynamic resolution renders the scene into the top-left uvScale fraction (render size / target size) of its
// targets. Clamps a target UV to the centres of that area's edge texels, so point and bilinear taps never
// read beyond it, where the targets hold stale or cleared texels. When the area fills the targets this equals
// clamp-to-edge addressing, so full-size rendering is unchanged.
vec2 ClampToRenderArea(vec2 targetUV, vec2 uvScale, ivec2 targetSize)
{
	vec2 halfTexel = 0.5 / vec2(targetSize);
	return clamp(targetUV, halfTexel, uvScale - halfTexel);
}

#endif
