#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Core/MemoryTracker.h"
#include "Basalt/Math/AABB.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace Basalt {

	// Interleaved vertex layout shared by all meshes and the renderer (80 bytes).
	struct Vertex
	{
		glm::vec3 Position = { 0.0f, 0.0f, 0.0f };
		glm::vec3 Normal = { 0.0f, 1.0f, 0.0f };
		// xyz = tangent, w = bitangent sign (glTF convention).
		glm::vec4 Tangent = { 1.0f, 0.0f, 0.0f, 1.0f };
		glm::vec2 TexCoord = { 0.0f, 0.0f };
		glm::uvec4 Joints = glm::uvec4(0);
		glm::vec4 Weights = glm::vec4(0.0f);
	};
	static_assert(sizeof(Vertex) == 80);

	enum class AlphaMode
	{
		Opaque = 0,
		Mask,
		Blend
	};

	// Material imported with a mesh (glTF metallic-roughness). Texture fields are texture asset keys
	// understood by AssetManager::GetTexture (file paths or "<mesh>#image/<index>" for embedded images).
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
