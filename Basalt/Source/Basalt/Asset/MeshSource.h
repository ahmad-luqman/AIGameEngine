#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Core/MemoryTracker.h"
#include "Basalt/Math/AABB.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <span>
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

	// The full material model, shared by imported glTF materials, material files (.bmaterial) and inline Material
	// overrides (MaterialAsset::FromComponent): glTF metallic-roughness plus clearcoat, transmission, normal/occlusion
	// strengths and per-map KHR_texture_transform/texCoord. Texture fields are texture asset keys understood by
	// AssetManager::GetTexture (file paths, "<mesh>#image/<index>" for embedded images, or asset-uuid:// keys for
	// material-file references whose UUID no longer resolves).
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

	// Import limits for morph targets: targets per glTF mesh, and deltas (targets x vertices) per model. The renderer
	// keeps each model's deltas in one texture up to 4096 texels wide, two texels per delta, at most 2048 rows (128 MiB),
	// well within the portable 4096 x 4096 image limit.
	inline constexpr uint32_t MaxMorphTargets = 256;
	inline constexpr uint32_t MorphTextureWidth = 4096;
	inline constexpr uint32_t MaxMorphTextureRows = 2048;
	inline constexpr uint32_t MaxMorphDeltas = MorphTextureWidth * MaxMorphTextureRows / 2u;

	// Import limits for a model's triangle geometry, summed over every primitive that is read (primitives can share one
	// accessor). At the limits a model holds 1.4 GiB of Vertex data and 256 MiB of indices before per-attribute scratch.
	// Elements of accessors without a buffer view are zero-filled (apart from any sparse values) from almost no data, so a
	// few bytes of JSON could otherwise ask for seconds of import work per million elements (normals, tangents,
	// optimization and LODs; 4 million zero-filled positions took 27 s in the fuzz build, found by FuzzGltf). They have a
	// far tighter limit, counted once more per morph target of their primitive (each target can generate the normals
	// again); 2^16 import in under half a second there. Real files use them for sparse morph targets, which do not count
	// (MaxMorphDeltas bounds their memory).
	inline constexpr uint32_t MaxModelVertices = 1u << 24;
	inline constexpr uint32_t MaxModelIndices = 1u << 26;
	inline constexpr uint32_t MaxZeroFilledElements = 1u << 16;

	// One vertex's displacement in one morph target (glTF POSITION/NORMAL target attributes, bind space). TANGENT
	// deltas are not kept: the vertex shader re-orthogonalizes the tangent against the morphed normal.
	struct MorphDelta
	{
		glm::vec3 Position = glm::vec3(0);
		glm::vec3 Normal = glm::vec3(0);
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
		// Morph targets of the owning glTF mesh (every primitive of a mesh has the same count). The deltas are
		// MeshSource::MorphDeltas[MorphDeltaOffset + target * VertexCount + vertex].
		uint32_t MorphTargetCount = 0;
		uint32_t MorphDeltaOffset = 0;
		// Per target: the range of its position deltas, which widens Bounds (and JointBounds) by weight.
		std::vector<AABB> MorphBounds;
	};

	// One drawable mesh (a glTF mesh): a set of submeshes. MeshComponent::MeshIndex selects one.
	struct MeshData
	{
		std::string Name;
		std::vector<uint32_t> Submeshes;
		AABB Bounds;
		// Morph target names (glTF extras.targetNames, else "Target<n>"; unique within the mesh) and the default
		// weights (mesh.weights, else 0), one per target.
		std::vector<std::string> MorphTargets;
		std::vector<float> MorphWeights;
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
		// Default morph weights of the node's mesh (node.weights, else the mesh's); empty without morph targets.
		std::vector<float> MorphWeights;
		// First of this node's weights in a pose's MorphWeights (FinalizeHierarchy); -1 without morph targets.
		int32_t MorphWeightOffset = -1;
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
		Scale,
		// Morph target weights of the node's mesh: Width values per key (see AnimationTrack::Weights).
		Weights
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
		// Translation, rotation (x, y, z, w) and scale keys; empty for Weights tracks.
		std::vector<glm::vec4> Values;
		// Weights tracks: Width (the node's morph target count) floats per value, in Values' key/tangent order.
		std::vector<float> Weights;
		uint32_t Width = 0;
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
		// The model's materials, then the default material the importer (and built-in meshes) add for primitives without
		// one: it is not the model's, so "<model>#material/<n>" and extraction stop before it (GetImportedMaterialCount).
		std::vector<MaterialData> Materials;
		std::vector<MeshNode> Nodes;
		// Morph target deltas of every submesh (see Submesh::MorphDeltaOffset).
		std::vector<MorphDelta> MorphDeltas;
		// Total morph weights of the nodes with morph targets: the size of a pose's MorphWeights (FinalizeHierarchy).
		uint32_t MorphWeightCount = 0;
		// Materials without the trailing default one, which is also the default's index. Every MeshSource the importer or
		// Primitives make has that default; an empty list (a hand-built source) counts as none.
		size_t GetImportedMaterialCount() const { return Materials.empty() ? 0 : Materials.size() - 1; }
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
		// Rebuilds NodeOrder and lays out the nodes' morph weights (call after Submeshes, Meshes and Nodes are set). False,
		// with the reason in outError and nothing changed, for an invalid parent or cycle, or morph data that does not fit
		// together (per mesh: default weight count, submesh target counts, morph bounds and delta ranges; per node: weight
		// count).
		bool FinalizeHierarchy(std::string* outError = nullptr);

		// Adds a submesh from raw geometry, updating bounds. Returns the submesh index. `morphDeltas` holds
		// morphTargetCount * vertices.size() deltas, target-major (empty without morph targets).
		uint32_t AddSubmesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, uint32_t materialIndex, const std::vector<MorphDelta>& morphDeltas = {}, uint32_t morphTargetCount = 0);
		// Recomputes MeshData and global bounds from submesh bounds.
		void UpdateBounds();

		// Generates tangents from positions, normals and UVs (for meshes without authored tangents).
		static void GenerateTangents(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
		// Generates smooth normals (for meshes without authored normals).
		static void GenerateNormals(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
	};

	// The range of the displacement a submesh's morph targets apply at `weights` (one per target; missing ones count
	// as 0): sum over targets of weight * MorphBounds. Contains zero; an empty (invalid) box without morph targets.
	AABB MorphDisplacementBounds(const Submesh& submesh, std::span<const float> weights);
	// A submesh vertex (index relative to BaseVertex) with its morph deltas applied at `weights`.
	Vertex MorphVertex(const MeshSource& source, const Submesh& submesh, uint32_t vertex, std::span<const float> weights);
	// Exact bounds of a submesh's vertices morphed at `weights` (its Bounds without morph targets): editor framing and
	// selection.
	AABB MorphedBounds(const MeshSource& source, const Submesh& submesh, std::span<const float> weights);
	// A mesh's bounds as a MeshComponent draws it: its submeshes widened by their morph targets at the mesh's default
	// weights (conservative and cheap, for per-frame selection boxes).
	AABB MorphedMeshBounds(const MeshSource& source, uint32_t mesh);

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
