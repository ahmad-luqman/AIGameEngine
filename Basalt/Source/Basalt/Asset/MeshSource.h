#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Core/MemoryTracker.h"
#include "Basalt/Math/AABB.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <string>
#include <vector>

namespace Basalt {

	// Interleaved vertex layout shared by all meshes and the renderer (88 bytes).
	struct Vertex
	{
		glm::vec3 Position = { 0.0f, 0.0f, 0.0f };
		glm::vec3 Normal = { 0.0f, 1.0f, 0.0f };
		// xyz = tangent, w = bitangent sign (glTF convention).
		glm::vec4 Tangent = { 1.0f, 0.0f, 0.0f, 1.0f };
		glm::vec2 TexCoord = { 0.0f, 0.0f };
		glm::uvec4 Joints = glm::uvec4(0);
		glm::vec4 Weights = glm::vec4(0.0f);
		glm::vec2 TexCoord1 = { 0.0f, 0.0f };
	};
	static_assert(sizeof(Vertex) == 88);

	enum class AlphaMode
	{
		Opaque = 0,
		Mask,
		Blend
	};

	// Material texture maps in the order of MaterialData::TextureTransforms and GPU UV rows. Common.glsl's
	// MATERIAL_SLOT_* defines must match; a mismatch samples the wrong map's UVs without any error.
	enum class MaterialTextureSlot : uint32_t
	{
		Albedo = 0,
		Normal,
		MetallicRoughness,
		Occlusion,
		Emissive,
		Clearcoat,
		ClearcoatRoughness,
		ClearcoatNormal,
		Transmission,
		Count
	};
	constexpr size_t MaterialTextureSlotCount = static_cast<size_t>(MaterialTextureSlot::Count);
	static_assert(MaterialTextureSlotCount == 9, "update Common.glsl MATERIAL_SLOT_* and MaterialData::UVRows");

	// Per-texture affine UV transform and source coordinate set (glTF KHR_texture_transform).
	struct TextureTransform
	{
		glm::vec2 Offset = glm::vec2(0);
		glm::vec2 Scale = glm::vec2(1);
		float Rotation = 0;
		uint32_t TexCoord = 0; // 0 or 1 (the importer rejects other sets): TEXCOORD_0 or TEXCOORD_1.
	};

	// Material imported with a mesh: glTF metallic-roughness plus clearcoat, transmission, normal/occlusion
	// strengths and per-map KHR_texture_transform/texCoord. Texture fields are texture asset keys understood by
	// AssetManager::GetTexture (file paths or "<mesh>#image/<index>" for embedded images).
	struct MaterialData
	{
		std::string Name;
		glm::vec4 AlbedoColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		float Metallic = 0.0f;
		float Roughness = 0.5f;
		glm::vec3 EmissiveColor = { 0.0f, 0.0f, 0.0f };
		float EmissiveIntensity = 1.0f;
		AlphaMode Alpha = AlphaMode::Opaque;
		float AlphaCutoff = 0.5f;
		bool DoubleSided = false;
		std::string AlbedoMap;
		std::string NormalMap;
		std::string MetallicRoughnessMap;
		std::string OcclusionMap;
		std::string EmissiveMap;
		float Clearcoat = 0.0f;
		float ClearcoatRoughness = 0.0f;
		float Transmission = 0.0f;
		float NormalScale = 1.0f;
		float OcclusionStrength = 1.0f;
		float ClearcoatNormalScale = 1.0f;
		std::string ClearcoatMap;
		std::string ClearcoatRoughnessMap;
		std::string ClearcoatNormalMap;
		std::string TransmissionMap;
		// Indexed by MaterialTextureSlot.
		std::array<TextureTransform, MaterialTextureSlotCount> TextureTransforms;

		// Drawn in the blended pass: alpha blending, or thin transmission revealing what lies behind. Such
		// draws keep their triangle order and LOD 0 and cast no shadows.
		bool IsBlended() const { return Alpha == AlphaMode::Blend || Transmission > 0; }
	};

	// Bind-space influence bounds. Homogeneous weight sums retain conservative bounds even when
	// normalized floating-point weights do not sum to exactly one.
	struct JointInfluenceBounds
	{
		uint32_t Joint = 0; // Skin-palette index, not a node index.
		AABB Positions;     // Positions both unscaled and multiplied by the vertex's total weight sum.
		// Homogeneous w range: vertex weight sums widened to include 1 for the divided point.
		float MinWeightSum = 0;
		float MaxWeightSum = 0;
	};

	// Reduced triangle range in the same index buffer, sharing the submesh's base vertex.
	struct MeshLOD
	{
		uint32_t BaseIndex = 0;
		uint32_t IndexCount = 0;
		float Error = 0; // Simplifier error relative to the primitive's extent.
	};

	// A contiguous range of the index buffer drawn with one material.
	struct Submesh
	{
		uint32_t BaseVertex = 0;
		uint32_t BaseIndex = 0;
		uint32_t IndexCount = 0;
		uint32_t VertexCount = 0;
		uint32_t MaterialIndex = 0;
		AABB Bounds;
		// Derived by AddSubmesh; changing positions/joints/weights requires re-adding geometry and its GPU buffers.
		std::vector<JointInfluenceBounds> JointBounds;
		// Reduced ranges, most detailed first, sharing BaseVertex. CPU queries and physics use the base range.
		std::vector<MeshLOD> LODs;
		// Defensive for procedural meshes: zero-weight vertices collapse (w = 0); include the model origin.
		bool HasUnweightedVertices = false;
	};

	// One drawable mesh (a glTF mesh): a set of submeshes. MeshComponent::MeshIndex selects one.
	struct MeshData
	{
		std::string Name;
		std::vector<uint32_t> Submeshes;
		AABB Bounds;
	};

	// Imported node, instantiated for static assets or sampled internally by Animator.
	struct MeshNode
	{
		std::string Name;
		int32_t Parent = -1;
		int32_t MeshIndex = -1;
		int32_t SkinIndex = -1;
		bool Active = true; // In the default glTF scene (all nodes still participate in joint transforms).
		bool HasMatrix = false;
		glm::mat4 Matrix = glm::mat4(1); // Used verbatim when HasMatrix; such nodes cannot be animation targets.
		glm::vec3 Translation = { 0.0f, 0.0f, 0.0f };
		glm::quat Rotation = { 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale = { 1.0f, 1.0f, 1.0f };
	};

	// One skin joint and the matching bind-vertex-to-joint transform.
	struct SkinJoint
	{
		uint32_t Node = 0;
		glm::mat4 InverseBind = glm::mat4(1);
	};
	// Joints reference MeshSource::Nodes; keeping each inverse bind with its index prevents mismatches.
	struct MeshSkin
	{
		std::vector<SkinJoint> Joints;
	};

	enum class AnimationPath
	{
		Translation,
		Rotation,
		Scale
	};
	enum class AnimationInterpolation
	{
		Step,
		Linear,
		CubicSpline
	};

	// Cubic values store incoming tangent, value, outgoing tangent for each key.
	struct AnimationTrack
	{
		uint32_t Node = 0;
		AnimationPath Path = AnimationPath::Translation;
		AnimationInterpolation Interpolation = AnimationInterpolation::Linear;
		std::vector<float> Times;
		std::vector<glm::vec4> Values;
	};

	// Named glTF clip. Duration is the last key time, in seconds.
	struct AnimationClip
	{
		std::string Name;
		float Duration = 0.0f;
		std::vector<AnimationTrack> Tracks;
	};

	// CPU-side geometry of a mesh asset (a glTF file or a built-in primitive). GPU buffers are created
	// from it by the renderer on first use.
	class MeshSource
	{
	public:
		std::vector<Vertex> Vertices;
		std::vector<uint32_t> Indices;
		std::vector<Submesh> Submeshes;
		std::vector<MeshData> Meshes;
		std::vector<MaterialData> Materials;
		std::vector<MeshNode> Nodes;
		std::vector<MeshSkin> Skins;
		std::vector<AnimationClip> Animations;
		// Unsupported influence sets retain geometry for static MeshComponents, but cannot be animated.
		bool AnimationUnsupported = false;
		// Parent-before-child traversal, independent of glTF node array ordering. Builders must call
		// FinalizeHierarchy after setting Nodes and before any pose evaluation.
		std::vector<uint32_t> NodeOrder;
		AABB Bounds;
		// Encoded bytes of images embedded in the file (glTF "#image/<n>" keys); empty for external images.
		std::vector<std::vector<uint8_t>> EmbeddedImages;
		// The other files a glTF read (external buffers and images; project-relative, sorted): a change to one of them
		// is a change to this asset.
		std::vector<std::string> ExternalFiles;
		// Asset key this source was loaded from ("Assets/Models/Ship.gltf", "builtin://Cube").
		std::string Key;

		// Refresh after mutating payload vectors; AssetManager loads do this before publishing.
		void UpdateMemoryUsage();

	private:
		MemoryAllocation m_Memory;

	public:
		// Rebuilds NodeOrder; returns false for an invalid parent or cycle.
		bool FinalizeHierarchy();

		// Adds a submesh from raw geometry, updating bounds. Returns the submesh index.
		uint32_t AddSubmesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, uint32_t materialIndex);
		// Recomputes MeshData and global bounds from submesh bounds.
		void UpdateBounds();

		// Generates tangents from positions, normals and UVs (for meshes without authored tangents).
		static void GenerateTangents(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
		// Generates smooth normals (for meshes without authored normals).
		static void GenerateNormals(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
	};

	// Built-in primitives, sized to match the default collider shapes (unit cube, radius 0.5, ...).
	namespace Primitives {

		Ref<MeshSource> CreateCube();
		Ref<MeshSource> CreateSphere(uint32_t segments = 32, uint32_t rings = 16);
		// 1x1 plane in the XZ plane facing +Y.
		Ref<MeshSource> CreatePlane();
		// Radius 0.5, height 1, along Y.
		Ref<MeshSource> CreateCylinder(uint32_t segments = 32);
		// Radius 0.5, cylinder half-height 0.5 (total height 2), along Y.
		Ref<MeshSource> CreateCapsule(uint32_t segments = 32, uint32_t rings = 8);

		// "builtin://Cube" etc. Returns nullptr for unknown names.
		Ref<MeshSource> CreateFromKey(const std::string& key);
		bool IsBuiltinKey(const std::string& key);

	}

}
