#ifndef BASALT_CUBEMAP_GLSL
#define BASALT_CUBEMAP_GLSL

#define PI 3.14159265359

// World direction of a cube-face texel (Vulkan face order +X, -X, +Y, -Y, +Z, -Z).
vec3 CubeDirection(uint face, vec2 uv)
{
	vec2 p = uv * 2.0 - 1.0;
	vec3 direction;
	if (face == 0u)
		direction = vec3(1.0, -p.y, -p.x);
	else if (face == 1u)
		direction = vec3(-1.0, -p.y, p.x);
	else if (face == 2u)
		direction = vec3(p.x, 1.0, p.y);
	else if (face == 3u)
		direction = vec3(p.x, -1.0, -p.y);
	else if (face == 4u)
		direction = vec3(p.x, -p.y, 1.0);
	else
		direction = vec3(-p.x, -p.y, -1.0);
	return normalize(direction);
}

// Van der Corput radical inverse for Hammersley points.
float RadicalInverse(uint bits)
{
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10;
}

vec2 Hammersley(uint i, uint count)
{
	return vec2(float(i) / float(count), RadicalInverse(i));
}

vec3 ImportanceSampleGGX(vec2 xi, vec3 N, float roughness)
{
	float a = roughness * roughness;
	float phi = 2.0 * PI * xi.x;
	float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
	float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
	vec3 H = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

	vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(up, N));
	vec3 bitangent = cross(N, tangent);
	return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

#endif
