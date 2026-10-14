#ifndef BASALT_LIGHT_DATA_GLSL
#define BASALT_LIGHT_DATA_GLSL

struct LightData
{
	vec4 Position;        // xyz, w: range (point, spot)
	vec4 Color;           // rgb * intensity, w: type (0 point, 1 spot, 2 unshadowed directional)
	vec4 Direction;       // xyz: direction the light travels (spot, directional), w: cos(outer)
	vec4 Params;          // x: cos(inner), y: first shadow map + 1 (0 = none), z: PCF radius in texels
};

#endif
