// Depth tuple: near, far, orthographic (0/1), fragment count-diagnostic flag. Mirrors LightClusters.cpp.
// Logarithmic perspective slices concentrate cells near the eye; orthographic slices are linear.
#ifndef BASALT_LIGHT_CLUSTERS_GLSL
#define BASALT_LIGHT_CLUSTERS_GLSL

float ClusterBoundary(vec4 depths, uint slice, uint slices)
{
	float t = float(slice) / float(slices);
	return depths.z > 0.5 ? mix(depths.x, depths.y, t) : depths.x * pow(depths.y / depths.x, t);
}

uint ClusterSlice(vec4 depths, float viewDepth, uint slices)
{
	float t = depths.z > 0.5 ? (viewDepth - depths.x) / (depths.y - depths.x) :
		log(max(viewDepth, depths.x) / depths.x) / log(depths.y / depths.x);
	return uint(clamp(t * float(slices), 0.0, float(slices - 1u)));
}

uint ClusterIndex(uvec3 cell, uvec3 grid)
{
	return (cell.z * grid.y + cell.y) * grid.x + cell.x;
}

uint ClusterListOffset(uint index)
{
	return index * (MAX_LIGHTS + 1u);
}

#endif
