#ifndef BASALT_LIGHT_DATA_GLSL
#define BASALT_LIGHT_DATA_GLSL

struct LightData
{
	vec4 Position;        // xyz, w: range (point, spot)
	vec4 Color;           // rgb * intensity, w: type (0 point, 1 spot, 2 unshadowed directional)
	vec4 Direction;       // xyz: direction the light travels (spot, directional), w: cos(outer)
	vec4 Params;          // x: cos(inner), y: first shadow map + 1 (0 = none), z: PCF radius in texels
};

// One descriptor covers both the CPU-uploaded light records and GPU-built lists. Keeping them
// together preserves the four-storage-buffer frame layout supported by Vulkan core devices.
#ifdef BASALT_LIGHT_BUFFER_WRITABLE
layout(std430, set = 0, binding = 3) buffer LightBuffer
#else
layout(std430, set = 0, binding = 3) readonly buffer LightBuffer
#endif
{
	LightData Lights[MAX_LIGHTS];
	uint ClusterWords[]; // per cell: count followed by MAX_LIGHTS ordered indices
} u_Lights;

#endif
