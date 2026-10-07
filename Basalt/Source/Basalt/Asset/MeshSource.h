#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Math/AABB.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace Basalt {

	// Interleaved vertex layout shared by all meshes and the renderer (48 bytes).
	struct Vertex
	{
		glm::vec3 Position = { 0.0f, 0.0f, 0.0f };
		glm::vec3 Normal = { 0.0f, 1.0f, 0.0f };
		// xyz = tangent, w = bitangent sign (glTF convention).
		glm::vec4 Tangent = { 1.0f, 0.0f, 0.0f, 1.0f };
		glm::vec2 TexCoord = { 0.0f, 0.0f };
	};
	static_assert(sizeof(Vertex) == 48);

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

	// A contiguous range of the index buffer drawn with one material.
	struct Submesh
	{
		uint32_t BaseVertex = 0;
		uint32_t BaseIndex = 0;
		uint32_t IndexCount = 0;
		uint32_t VertexCount = 0;
		uint32_t MaterialIndex = 0;
		AABB Bounds;
	};

	// One drawable mesh (a glTF mesh): a set of submeshes. MeshComponent::MeshIndex selects one.
	struct MeshData
	{
		std::string Name;
		std::vector<uint32_t> Submeshes;
		AABB Bounds;
	};

	// Node of an imported scene hierarchy, used to recreate it as entities.
	struct MeshNode
	{
		std::string Name;
		int32_t Parent = -1;
		int32_t MeshIndex = -1;
		glm::vec3 Translation = { 0.0f, 0.0f, 0.0f };
		glm::quat Rotation = { 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale = { 1.0f, 1.0f, 1.0f };
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
		AABB Bounds;
		// Encoded bytes of images embedded in the file (glTF "#image/<n>" keys); empty for external images.
		std::vector<std::vector<uint8_t>> EmbeddedImages;
		// Asset key this source was loaded from ("Assets/Models/Ship.gltf", "builtin://Cube").
		std::string Key;

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
