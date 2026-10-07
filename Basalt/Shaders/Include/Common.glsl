// Shared declarations for scene shaders. Binding layout (see SceneRenderer.cpp, keep in sync):
//   set 0 (frame):    0 ViewConstants (UBO), 1 Draws (SSBO), 2 Materials (SSBO), 3 Lights (SSBO),
//                     4 shadow map array, 5 irradiance cube, 6 prefiltered cube, 7 BRDF LUT, 8 SSAO,
//                     9 linear clamp sampler, 10 shadow comparison sampler, 11 point clamp sampler,
//                     push constants (DrawPush)
//   set 1 (material): 0 albedo, 1 normal, 2 metallic-roughness, 3 occlusion, 4 emissive, 5 material sampler

#ifndef BASALT_COMMON_GLSL
#define BASALT_COMMON_GLSL

#define MAX_CASCADES 4
#define PI 3.14159265359

struct DirectionalLight
{
	vec4 Direction;       // xyz: direction the light travels (world), w: enabled
	vec4 Color;           // rgb * intensity, w: shadows enabled
	vec4 ShadowParams;    // x: bias, y: normal bias, z: softness (light size), w: cascade count
};

layout(set = 0, binding = 0) uniform ViewConstants
{
	mat4 View;
	mat4 Projection;
	mat4 ViewProjection;
	mat4 InverseView;
	mat4 InverseProjection;
	mat4 CascadeViewProjection[MAX_CASCADES];
	vec4 CascadeSplits;   // view-space far distance of each cascade
	vec4 CameraPosition;  // xyz, w: unused
	vec4 ViewportSize;    // xy: size, zw: 1/size
	vec4 AmbientColor;    // rgb, w: IBL intensity
	vec4 EnvironmentParams; // x: rotation (radians), y: prefiltered mip count, z: SSAO enabled, w: light count
	vec4 CameraClip;      // x: near, y: far
	DirectionalLight Sun;
} u_View;

struct DrawData
{
	mat4 Model;
	mat4 Normal;          // inverse-transpose of Model (upper 3x3 used)
	uvec4 Indices;        // x: material index
};

layout(std430, set = 0, binding = 1) readonly buffer DrawBuffer
{
	DrawData Draws[];
} u_Draws;

struct MaterialData
{
	vec4 AlbedoColor;
	vec4 Emissive;        // rgb * intensity
	vec4 Params;          // x: metallic, y: roughness, z: alpha cutoff (0 = opaque), w: flags
};

layout(std430, set = 0, binding = 2) readonly buffer MaterialBuffer
{
	MaterialData Materials[];
} u_Materials;

struct LightData
{
	vec4 Position;        // xyz, w: range
	vec4 Color;           // rgb * intensity, w: type (0 point, 1 spot)
	vec4 Direction;       // xyz (spot), w: cos(outer)
	vec4 Params;          // x: cos(inner)
};

layout(std430, set = 0, binding = 3) readonly buffer LightBuffer
{
	LightData Lights[];
} u_Lights;

layout(push_constant) uniform DrawPush
{
	uint DrawIndex;
	uint CascadeIndex;
} u_Push;

// Material flags (MaterialData.Params.w).
#define MATERIAL_HAS_ALBEDO_MAP 1u
#define MATERIAL_HAS_NORMAL_MAP 2u
#define MATERIAL_HAS_MR_MAP 4u
#define MATERIAL_HAS_OCCLUSION_MAP 8u
#define MATERIAL_HAS_EMISSIVE_MAP 16u

vec3 RotateY(vec3 v, float angle)
{
	float s = sin(angle);
	float c = cos(angle);
	return vec3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

#endif
