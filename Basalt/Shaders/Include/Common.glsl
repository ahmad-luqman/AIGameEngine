// Shared declarations for scene shaders. Binding layout (see SceneRenderer.cpp, keep in sync):
//   set 0 (frame):    0 ViewConstants (UBO), 1 Draws (SSBO), 2 Materials (SSBO), 3 Lights + cluster lists (SSBO),
//                     4 shadow map array, 5 irradiance cube, 6 prefiltered cube, 7 BRDF LUT, 8 SSAO,
//                     9 linear clamp sampler, 10 shadow comparison sampler, 11 point clamp sampler,
//                     12 environment cube, 13 local shadow atlas, 14 local shadow maps (SSBO),
//                     push constants (DrawPush)
// Shadow pipelines use a reduced set 0: push constants, 0, 1, 2 and 14 only.
//   set 1 (material): 0 albedo, 1 normal, 2 metallic-roughness, 3 occlusion, 4 emissive, 5 material sampler,
//                     6 clearcoat, 7 coat roughness, 8 coat normal, 9 transmission
//   (the particle pipelines bind their own set 1; see Particle.vert)
//   set 2 (morph, vertex stage): 0 morph target deltas, 1 point sampler (see Morphing.glsl); every mesh pipeline,
//                     shadows included (their set 1 may be a hole)

#ifndef BASALT_COMMON_GLSL
#define BASALT_COMMON_GLSL

// MAX_CASCADES is supplied by CMake from the same value as the C++ view-constant layout.
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
	vec4 ViewportSize;    // xy: render-area size; zw: 1 / scene target size, NOT 1 / xy (the targets exceed the area under dynamic resolution), so gl_FragCoord.xy * zw is a target UV
	vec4 AmbientColor;    // rgb, w: IBL intensity
	vec4 EnvironmentParams; // x: rotation (radians), y: prefiltered mip count, z: SSAO enabled, w: light count
	vec4 CameraClip;      // x: near, y: far, zw: local biases (available without a Sun); z uses range-relative world distance, unlike Sun.ShadowParams.x
	DirectionalLight Sun;
	uvec4 ClusterGrid;    // xyz dimensions, w: clustered lookup enabled (0 = diagnostic all-lights path)
	vec4 ClusterDepths;   // near, far, orthographic, visualize visited-light count
	vec4 Quality;         // x: low-cost shadow filtering
	mat4 PreviousViewProjection;
} u_View;

struct DrawData
{
	mat4 Model;
	mat4 Normal;          // draw records: inverse-transpose of Model; unused for palette records
	uvec4 Indices;        // x: material index; y: editor picking token; zw: joint palette offset/count (0 count = rigid)
	uvec4 Morph;          // x: first morph weight record; y: active weight count (0 = unmorphed); z: delta texel base; w: texels per target
	uvec4 History;        // x: previous-state record in this frame's buffer, y: valid
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
	vec4 Extensions;      // clearcoat, coat roughness, transmission, use albedo alpha as coverage
	vec4 NormalParams;    // normal scale, occlusion strength, coat normal scale
	vec4 UVRows[18];      // two affine rows per MATERIAL_SLOT_*, first row.w selects UV0/UV1
};

layout(std430, set = 0, binding = 2) readonly buffer MaterialBuffer
{
	MaterialData Materials[];
} u_Materials;

#include "LightData.glsl"

struct LocalShadowMap
{
	mat4 ViewProjection;
	vec4 AtlasRect;       // scale.xy, offset.zw
};

layout(std430, set = 0, binding = 14) readonly buffer LocalShadowBuffer
{
	LocalShadowMap Maps[];
} u_LocalShadows;

layout(push_constant) uniform DrawPush
{
	// First mesh-instance record: DrawIndex + gl_InstanceIndex (firstInstance is zero).
	// Particle draws reuse this as the first particle instead.
	uint DrawIndex;
	uint ShadowViewIndex; // [0, MAX_CASCADES) selects a cascade; later indices select atlas maps.
} u_Push;

mat4 ShadowViewProjection(uint index)
{
	return index < MAX_CASCADES ? u_View.CascadeViewProjection[index] :
		u_LocalShadows.Maps[index - MAX_CASCADES].ViewProjection;
}

// Material flags (MaterialData.Params.w).
#define MATERIAL_HAS_ALBEDO_MAP 1u
#define MATERIAL_HAS_NORMAL_MAP 2u
#define MATERIAL_HAS_MR_MAP 4u
#define MATERIAL_HAS_OCCLUSION_MAP 8u
#define MATERIAL_HAS_EMISSIVE_MAP 16u
#define MATERIAL_HAS_CLEARCOAT_MAP 32u
#define MATERIAL_HAS_CLEARCOAT_ROUGHNESS_MAP 64u
#define MATERIAL_HAS_CLEARCOAT_NORMAL_MAP 128u
#define MATERIAL_HAS_TRANSMISSION_MAP 256u

// Texture slots, in the order of MaterialTextureSlot (Asset/MeshSource.h).
#define MATERIAL_SLOT_ALBEDO 0
#define MATERIAL_SLOT_NORMAL 1
#define MATERIAL_SLOT_METALLIC_ROUGHNESS 2
#define MATERIAL_SLOT_OCCLUSION 3
#define MATERIAL_SLOT_EMISSIVE 4
#define MATERIAL_SLOT_CLEARCOAT 5
#define MATERIAL_SLOT_CLEARCOAT_ROUGHNESS 6
#define MATERIAL_SLOT_CLEARCOAT_NORMAL 7
#define MATERIAL_SLOT_TRANSMISSION 8

vec3 RotateY(vec3 v, float angle)
{
	float s = sin(angle);
	float c = cos(angle);
	return vec3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

#endif
