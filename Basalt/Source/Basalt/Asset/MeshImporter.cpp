#include "Basalt/Asset/MeshImporter.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Scene/Scene.h"

#include <cgltf.h>

#include <cstdlib>
#include <cstring>
#include <memory>

namespace Basalt {

	namespace {

		struct GltfDeleter
		{
			void operator()(cgltf_data* data) const { cgltf_free(data); }
		};
		using GltfData = std::unique_ptr<cgltf_data, GltfDeleter>;

		const char* ResultToString(cgltf_result result)
		{
			switch (result)
			{
				case cgltf_result_data_too_short:
					return "data too short";
				case cgltf_result_unknown_format:
					return "unknown format";
				case cgltf_result_invalid_json:
					return "invalid JSON";
				case cgltf_result_invalid_gltf:
					return "invalid glTF";
				case cgltf_result_invalid_options:
					return "invalid options";
				case cgltf_result_file_not_found:
					return "file not found";
				case cgltf_result_io_error:
					return "I/O error";
				case cgltf_result_out_of_memory:
					return "out of memory";
				case cgltf_result_legacy_gltf:
					return "glTF 1.0 is not supported";
				default:
					return "unknown error";
			}
		}

		GltfData LoadGltfData(const std::filesystem::path& path, std::string& outError)
		{
			const std::string pathString = path.string();
			cgltf_options options = {};
			cgltf_data* raw = nullptr;
			cgltf_result result = cgltf_parse_file(&options, pathString.c_str(), &raw);
			GltfData data(raw);
			if (result != cgltf_result_success)
			{
				outError = path.filename().string() + ": " + ResultToString(result);
				return nullptr;
			}
			result = cgltf_load_buffers(&options, data.get(), pathString.c_str());
			if (result != cgltf_result_success)
			{
				outError = path.filename().string() + ": cannot load buffers (" + ResultToString(result) + ")";
				return nullptr;
			}
			result = cgltf_validate(data.get());
			if (result != cgltf_result_success)
			{
				outError = path.filename().string() + ": validation failed (" + ResultToString(result) + ")";
				return nullptr;
			}
			return data;
		}

		bool IsDataUri(const char* uri)
		{
			return uri && std::string_view(uri).rfind("data:", 0) == 0;
		}

		std::string TextureKey(const cgltf_data* data, const cgltf_texture_view& view, const std::filesystem::path& gltfPath, const std::string& meshKey)
		{
			if (!view.texture || !view.texture->image)
				return {};
			const cgltf_image* image = view.texture->image;
			if (image->uri && !IsDataUri(image->uri))
			{
				std::string uri = image->uri;
				cgltf_decode_uri(uri.data());
				uri.resize(std::strlen(uri.c_str()));
				return Project::MakeRelative(gltfPath.parent_path() / uri);
			}
			return meshKey + "#image/" + std::to_string(image - data->images);
		}

		// Reads a float accessor (including sparse accessors) into a fresh vector. False if the accessor's
		// data does not have the expected shape.
		template<typename T>
		bool ReadAccessor(const cgltf_accessor* accessor, std::vector<T>& out, cgltf_size components)
		{
			out.assign(accessor->count, T(0.0f));
			if (cgltf_num_components(accessor->type) != components)
				return false;
			const cgltf_size floatCount = accessor->count * components;
			return cgltf_accessor_unpack_floats(accessor, reinterpret_cast<float*>(out.data()), floatCount) == floatCount;
		}

		bool ReadIndices(const cgltf_accessor* accessor, std::vector<uint32_t>& out)
		{
			out.assign(accessor->count, 0u);
			if (accessor->count == 0)
				return true;
			if (cgltf_accessor_unpack_indices(accessor, out.data(), sizeof(uint32_t), accessor->count) == accessor->count)
				return true;
			// unpack_indices does not support sparse accessors; floats represent indices exactly below 2^24.
			std::vector<float> values(accessor->count);
			if (cgltf_num_components(accessor->type) != 1 || cgltf_accessor_unpack_floats(accessor, values.data(), values.size()) != values.size())
				return false;
			for (size_t i = 0; i < values.size(); i++)
			{
				if (values[i] < 0.0f || values[i] >= 16777216.0f)
					return false;
				out[i] = static_cast<uint32_t>(values[i]);
			}
			return true;
		}

		// Encoded bytes (PNG/JPEG/...) of an image stored inside the glTF (buffer view or data URI).
		// Empty for images referenced by an external file.
		std::vector<uint8_t> ExtractEmbeddedImage(const cgltf_image& image)
		{
			if (image.buffer_view)
			{
				const cgltf_buffer_view* view = image.buffer_view;
				if (!view->buffer || !view->buffer->data || view->offset + view->size > view->buffer->size)
					return {};
				const auto* bytes = static_cast<const uint8_t*>(view->buffer->data) + view->offset;
				return std::vector<uint8_t>(bytes, bytes + view->size);
			}
			if (IsDataUri(image.uri))
			{
				const std::string_view uri(image.uri);
				const size_t comma = uri.find(',');
				if (comma == std::string_view::npos || uri.substr(0, comma).find(";base64") == std::string_view::npos)
					return {};
				const std::string_view payload = uri.substr(comma + 1);
				size_t padding = 0;
				if (!payload.empty() && payload.back() == '=')
					padding = payload.size() >= 2 && payload[payload.size() - 2] == '=' ? 2 : 1;
				if (payload.size() % 4 != 0)
					return {};
				const size_t decodedSize = payload.size() / 4 * 3 - padding;
				void* decoded = nullptr;
				cgltf_options options = {};
				// cgltf reads exactly 4 * ceil(decodedSize / 3) characters, which the size check above guarantees exist.
				if (cgltf_load_buffer_base64(&options, decodedSize, payload.data(), &decoded) != cgltf_result_success) // NOLINT(bugprone-suspicious-stringview-data-usage)
					return {};
				const auto* bytes = static_cast<const uint8_t*>(decoded);
				std::vector<uint8_t> result(bytes, bytes + decodedSize);
				std::free(decoded);
				return result;
			}
			return {};
		}

	}

	Ref<MeshSource> MeshImporter::LoadGltf(const std::filesystem::path& path, const std::string& key, std::string& outError)
	{
		GltfData data = LoadGltfData(path, outError);
		if (!data)
			return nullptr;

		Ref<MeshSource> source = CreateRef<MeshSource>();
		source->Key = key;

		// Materials (plus a default one at the end for primitives without a material).
		for (cgltf_size i = 0; i < data->materials_count; i++)
		{
			const cgltf_material& material = data->materials[i];
			MaterialData result;
			result.Name = material.name ? material.name : "Material" + std::to_string(i);
			if (material.has_pbr_metallic_roughness)
			{
				const cgltf_pbr_metallic_roughness& pbr = material.pbr_metallic_roughness;
				result.AlbedoColor = { pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2], pbr.base_color_factor[3] };
				result.Metallic = pbr.metallic_factor;
				result.Roughness = pbr.roughness_factor;
				result.AlbedoMap = TextureKey(data.get(), pbr.base_color_texture, path, key);
				result.MetallicRoughnessMap = TextureKey(data.get(), pbr.metallic_roughness_texture, path, key);
			}
			result.NormalMap = TextureKey(data.get(), material.normal_texture, path, key);
			result.OcclusionMap = TextureKey(data.get(), material.occlusion_texture, path, key);
			result.EmissiveMap = TextureKey(data.get(), material.emissive_texture, path, key);
			result.EmissiveColor = { material.emissive_factor[0], material.emissive_factor[1], material.emissive_factor[2] };
			if (material.has_emissive_strength)
				result.EmissiveIntensity = material.emissive_strength.emissive_strength;
			result.Alpha = material.alpha_mode == cgltf_alpha_mode_mask ? AlphaMode::Mask : (material.alpha_mode == cgltf_alpha_mode_blend ? AlphaMode::Blend : AlphaMode::Opaque);
			result.AlphaCutoff = material.alpha_cutoff;
			result.DoubleSided = material.double_sided;
			source->Materials.push_back(std::move(result));
		}
		const uint32_t defaultMaterial = static_cast<uint32_t>(source->Materials.size());
		source->Materials.push_back(MaterialData{ .Name = "Default" });

		// Geometry.
		for (cgltf_size meshIndex = 0; meshIndex < data->meshes_count; meshIndex++)
		{
			const cgltf_mesh& mesh = data->meshes[meshIndex];
			MeshData meshData;
			meshData.Name = mesh.name ? mesh.name : "Mesh" + std::to_string(meshIndex);

			for (cgltf_size primitiveIndex = 0; primitiveIndex < mesh.primitives_count; primitiveIndex++)
			{
				const cgltf_primitive& primitive = mesh.primitives[primitiveIndex];
				if (primitive.type != cgltf_primitive_type_triangles)
				{
					BS_CORE_WARN("MeshImporter: {}: skipping non-triangle primitive in mesh '{}'", path.filename().string(), meshData.Name);
					continue;
				}

				const cgltf_accessor* positions = nullptr;
				const cgltf_accessor* normals = nullptr;
				const cgltf_accessor* tangents = nullptr;
				const cgltf_accessor* texCoords = nullptr;
				for (cgltf_size a = 0; a < primitive.attributes_count; a++)
				{
					const cgltf_attribute& attribute = primitive.attributes[a];
					if (attribute.type == cgltf_attribute_type_position)
						positions = attribute.data;
					else if (attribute.type == cgltf_attribute_type_normal)
						normals = attribute.data;
					else if (attribute.type == cgltf_attribute_type_tangent)
						tangents = attribute.data;
					else if (attribute.type == cgltf_attribute_type_texcoord && attribute.index == 0)
						texCoords = attribute.data;
				}
				if (!positions || positions->count == 0)
				{
					BS_CORE_WARN("MeshImporter: {}: primitive without positions in mesh '{}'", path.filename().string(), meshData.Name);
					continue;
				}

				std::vector<Vertex> vertices(positions->count);
				std::vector<glm::vec3> positionData;
				if (!ReadAccessor(positions, positionData, 3))
				{
					BS_CORE_WARN("MeshImporter: {}: unreadable positions in mesh '{}'", path.filename().string(), meshData.Name);
					continue;
				}
				for (size_t v = 0; v < vertices.size(); v++)
					vertices[v].Position = positionData[v];

				std::vector<glm::vec3> normalData;
				const bool hasNormals = normals && normals->count == positions->count && ReadAccessor(normals, normalData, 3);
				if (hasNormals)
				{
					for (size_t v = 0; v < vertices.size(); v++)
						vertices[v].Normal = normalData[v];
				}
				std::vector<glm::vec2> texCoordData;
				if (texCoords && texCoords->count == positions->count && ReadAccessor(texCoords, texCoordData, 2))
				{
					for (size_t v = 0; v < vertices.size(); v++)
						vertices[v].TexCoord = texCoordData[v];
				}

				std::vector<uint32_t> indices;
				if (primitive.indices)
				{
					if (!ReadIndices(primitive.indices, indices))
					{
						BS_CORE_WARN("MeshImporter: {}: unreadable indices in mesh '{}'", path.filename().string(), meshData.Name);
						continue;
					}
				}
				else
				{
					indices.resize(vertices.size());
					for (size_t i = 0; i < indices.size(); i++)
						indices[i] = static_cast<uint32_t>(i);
				}
				// Reject out-of-range indices instead of reading past the vertex buffer on the GPU.
				bool indicesValid = indices.size() % 3 == 0;
				for (uint32_t index : indices)
					indicesValid = indicesValid && index < vertices.size();
				if (!indicesValid)
				{
					BS_CORE_WARN("MeshImporter: {}: invalid index data in mesh '{}'", path.filename().string(), meshData.Name);
					continue;
				}

				if (!hasNormals)
					MeshSource::GenerateNormals(vertices, indices);
				std::vector<glm::vec4> tangentData;
				if (tangents && tangents->count == positions->count && ReadAccessor(tangents, tangentData, 4))
				{
					for (size_t v = 0; v < vertices.size(); v++)
						vertices[v].Tangent = tangentData[v];
				}
				else
				{
					MeshSource::GenerateTangents(vertices, indices);
				}

				const uint32_t materialIndex = primitive.material ? static_cast<uint32_t>(primitive.material - data->materials) : defaultMaterial;
				meshData.Submeshes.push_back(source->AddSubmesh(vertices, indices, materialIndex));
			}
			source->Meshes.push_back(std::move(meshData));
		}

		// Node hierarchy (default scene, or every node if no scene is defined).
		for (cgltf_size i = 0; i < data->nodes_count; i++)
		{
			const cgltf_node& node = data->nodes[i];
			MeshNode result;
			result.Name = node.name ? node.name : "Node" + std::to_string(i);
			result.Parent = node.parent ? static_cast<int32_t>(node.parent - data->nodes) : -1;
			result.MeshIndex = node.mesh ? static_cast<int32_t>(node.mesh - data->meshes) : -1;
			if (node.has_matrix)
			{
				glm::mat4 matrix;
				std::memcpy(&matrix, node.matrix, sizeof(float) * 16);
				Math::DecomposeTransform(matrix, result.Translation, result.Rotation, result.Scale);
			}
			else
			{
				if (node.has_translation)
					result.Translation = { node.translation[0], node.translation[1], node.translation[2] };
				if (node.has_rotation)
					result.Rotation = glm::quat(node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]);
				if (node.has_scale)
					result.Scale = { node.scale[0], node.scale[1], node.scale[2] };
			}
			source->Nodes.push_back(std::move(result));
		}
		// A file with meshes but no nodes still gets something to instantiate.
		if (source->Nodes.empty())
		{
			for (size_t m = 0; m < source->Meshes.size(); m++)
				source->Nodes.push_back(MeshNode{ .Name = source->Meshes[m].Name, .MeshIndex = static_cast<int32_t>(m) });
		}

		// Keep embedded images' encoded bytes so textures decode without re-reading the file.
		source->EmbeddedImages.resize(data->images_count);
		for (cgltf_size i = 0; i < data->images_count; i++)
			source->EmbeddedImages[i] = ExtractEmbeddedImage(data->images[i]);

		if (source->Submeshes.empty())
		{
			outError = path.filename().string() + ": contains no triangle geometry";
			return nullptr;
		}

		source->UpdateBounds();
		return source;
	}

	Entity MeshImporter::InstantiateHierarchy(Scene& scene, const MeshSource& source, const std::string& name, Entity parent)
	{
		Entity root = scene.CreateEntity(name);
		if (parent)
			scene.SetParent(root, parent, false);

		std::vector<Entity> nodeEntities(source.Nodes.size());
		for (size_t i = 0; i < source.Nodes.size(); i++)
		{
			const MeshNode& node = source.Nodes[i];
			Entity entity = scene.CreateEntity(node.Name);
			auto& transform = entity.GetTransform();
			transform.Translation = node.Translation;
			transform.Rotation = node.Rotation;
			transform.Scale = node.Scale;
			if (node.MeshIndex >= 0)
			{
				auto& mesh = entity.AddComponent<MeshComponent>();
				mesh.Mesh = source.Key;
				mesh.MeshIndex = static_cast<uint32_t>(node.MeshIndex);
			}
			nodeEntities[i] = entity;
		}

		// glTF guarantees parents form a forest, so parent indices are valid and acyclic.
		for (size_t i = 0; i < source.Nodes.size(); i++)
		{
			const int32_t parentIndex = source.Nodes[i].Parent;
			Entity parentEntity = parentIndex >= 0 && static_cast<size_t>(parentIndex) < nodeEntities.size() ? nodeEntities[parentIndex] : root;
			scene.SetParent(nodeEntities[i], parentEntity, false);
		}
		return root;
	}

}
