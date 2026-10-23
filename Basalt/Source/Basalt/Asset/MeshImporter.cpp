#include "Basalt/Asset/MeshImporter.h"

#include "Basalt/Animation/AnimationPose.h"
#include "Basalt/Asset/AnimationImporter.h"
#include "Basalt/Asset/AssetKey.h"
#include "Basalt/Asset/MaterialAsset.h"
#include "Basalt/Asset/MeshOptimizer.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Core/Profiler.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Scene/Scene.h"

#include <cgltf.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <set>

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

		bool IsDataUri(const char* uri);

		std::string DecodeUri(const char* uri)
		{
			std::string decoded = uri;
			cgltf_decode_uri(decoded.data());
			decoded.resize(std::strlen(decoded.c_str()));
			return decoded;
		}

		// glTF files are untrusted: an external buffer or image URI may only name a file inside the active
		// project (or, without one, inside the glTF's own folder). Absolute paths, URI schemes and '..'
		// escapes are rejected, so a model cannot make the engine read arbitrary files.
		bool CheckExternalUri(const char* uri, const std::filesystem::path& gltfPath, const std::filesystem::path& root, std::string& outError)
		{
			if (!uri || IsDataUri(uri))
				return true;
			const std::string decoded = DecodeUri(uri);

			const std::filesystem::path relative(decoded);
			const bool hasScheme = decoded.find(':') != std::string::npos;
			const bool rooted = relative.has_root_path() || decoded.starts_with('/') || decoded.starts_with('\\');
			const std::filesystem::path resolved = (gltfPath.parent_path() / relative).lexically_normal();
			const std::filesystem::path inside = resolved.lexically_relative(root.lexically_normal());
			if (hasScheme || rooted || decoded.empty() || inside.empty() || *inside.begin() == "..")
			{
				outError = gltfPath.filename().string() + ": external file '" + decoded + "' is outside the project";
				return false;
			}
			return true;
		}

		// cgltf allocates a buffer's declared byteLength before decoding its data URI or reading its file, so a few
		// bytes of JSON could request gigabytes (found by FuzzGltf). A buffer may not claim more bytes than its data
		// holds. Data URIs without a comma are left to cgltf_load_buffers, which rejects them before allocating.
		bool CheckBufferSizes(const cgltf_data* data, const std::filesystem::path& path, std::string& outError)
		{
			for (cgltf_size i = 0; i < data->buffers_count; i++)
			{
				const cgltf_buffer& buffer = data->buffers[i];
				// Without a URI the buffer is a GLB's binary chunk, which cgltf checks against the chunk's length.
				if (!buffer.uri)
					continue;
				uint64_t available = 0;
				if (IsDataUri(buffer.uri))
				{
					const char* comma = std::strchr(buffer.uri, ',');
					if (!comma)
						continue;
					// Each base64 character carries 6 bits.
					available = std::strlen(comma + 1) * 3 / 4;
				}
				else
				{
					// A file that cannot be sized is rejected here: cgltf would still open some of them (a directory on
					// Linux) and allocate the declared size before failing to read.
					const std::string file = DecodeUri(buffer.uri);
					std::error_code error;
					available = std::filesystem::file_size(path.parent_path() / file, error);
					if (error)
					{
						outError = path.filename().string() + ": buffer " + std::to_string(i) + " file '" + file + "' cannot be read (" + error.message() + ")";
						return false;
					}
				}
				if (buffer.size > available)
				{
					outError = path.filename().string() + ": buffer " + std::to_string(i) + " declares " + std::to_string(buffer.size) + " bytes but its data holds only " +
							   std::to_string(available);
					return false;
				}
			}
			return true;
		}

		// glTF requires accessor data to be aligned to its component size (spec 3.6.2.4). cgltf_validate reads
		// index data before checking that, through misaligned pointers (undefined behaviour, found by
		// FuzzGltf), so non-conforming files are rejected first.
		bool CheckAccessorAlignment(const cgltf_data* data, const std::filesystem::path& path, std::string& outError)
		{
			auto aligned = [](const cgltf_buffer_view* view, cgltf_size offset, cgltf_size size) { return !view || size == 0 || (view->offset + offset) % size == 0; };
			for (cgltf_size i = 0; i < data->accessors_count; i++)
			{
				const cgltf_accessor& accessor = data->accessors[i];
				const cgltf_size componentSize = cgltf_component_size(accessor.component_type);
				bool ok = aligned(accessor.buffer_view, accessor.offset, componentSize) && (componentSize == 0 || accessor.stride % componentSize == 0);
				if (accessor.is_sparse)
				{
					const cgltf_accessor_sparse& sparse = accessor.sparse;
					ok = ok && aligned(sparse.indices_buffer_view, sparse.indices_byte_offset, cgltf_component_size(sparse.indices_component_type)) &&
						 aligned(sparse.values_buffer_view, sparse.values_byte_offset, componentSize);
				}
				if (!ok)
				{
					outError = path.filename().string() + ": accessor " + std::to_string(i) + " is not aligned to its component size";
					return false;
				}
			}
			return true;
		}

		// cgltf_validate also reads sparse accessors' index data before it checks that buffer views fit their buffers
		// (found by FuzzGltf), so a view past its buffer's loaded bytes is rejected first.
		bool CheckBufferViewBounds(const cgltf_data* data, const std::filesystem::path& path, std::string& outError)
		{
			for (cgltf_size i = 0; i < data->buffer_views_count; i++)
			{
				const cgltf_buffer_view& view = data->buffer_views[i];
				if (!view.buffer || view.offset > view.buffer->size || view.size > view.buffer->size - view.offset)
				{
					outError = path.filename().string() + ": buffer view " + std::to_string(i) + " lies outside its buffer";
					return false;
				}
			}
			return true;
		}

		// The external files are listed as soon as the file parses, even when loading fails later (a missing buffer).
		GltfData LoadGltfData(const std::filesystem::path& path, const std::string& key, const std::filesystem::path& projectRoot, std::vector<std::string>& outExternalFiles, std::string& outError)
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

			static constexpr std::array<std::string_view, 6> Supported = { "KHR_texture_transform", "KHR_materials_emissive_strength", "KHR_materials_clearcoat", "KHR_materials_transmission", "KHR_mesh_quantization", "KHR_texture_basisu" };
			for (cgltf_size i = 0; i < data->extensions_required_count; i++)
				if (std::find(Supported.begin(), Supported.end(), std::string_view(data->extensions_required[i])) == Supported.end())
				{
					outError = key + ": unsupported required extension '" + data->extensions_required[i] + "'";
					return nullptr;
				}
			std::set<std::string> externalFiles;
			const auto addExternal = [&](const char* uri) {
				if (auto file = MeshImporter::GetExternalFileKey(key, uri))
					externalFiles.insert(std::move(*file));
			};
			for (cgltf_size i = 0; i < data->buffers_count; i++)
				addExternal(data->buffers[i].uri);
			for (cgltf_size i = 0; i < data->images_count; i++)
				addExternal(data->images[i].uri);
			outExternalFiles.assign(externalFiles.begin(), externalFiles.end());
			for (cgltf_size i = 0; i < data->buffers_count; i++)
			{
				if (!CheckExternalUri(data->buffers[i].uri, path, projectRoot, outError))
					return nullptr;
			}
			for (cgltf_size i = 0; i < data->images_count; i++)
			{
				if (!CheckExternalUri(data->images[i].uri, path, projectRoot, outError))
					return nullptr;
			}
			if (!CheckBufferSizes(data.get(), path, outError))
				return nullptr;
			result = cgltf_load_buffers(&options, data.get(), pathString.c_str());
			if (result != cgltf_result_success)
			{
				outError = path.filename().string() + ": cannot load buffers (" + ResultToString(result) + ")";
				return nullptr;
			}
			if (!CheckAccessorAlignment(data.get(), path, outError) || !CheckBufferViewBounds(data.get(), path, outError))
				return nullptr;
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

		std::string TextureKey(const cgltf_data* data, const cgltf_texture_view& view, const std::string& meshKey)
		{
			if (!view.texture)
				return {};
			const cgltf_image* image = view.texture->has_basisu ? view.texture->basisu_image : view.texture->image;
			if (!image)
				return {};
			if (auto external = MeshImporter::GetExternalFileKey(meshKey, image->uri))
				return *external;
			return MakeSubAssetKey(meshKey, EmbeddedImageMarker, static_cast<uint32_t>(image - data->images));
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

		// Why a mesh's morph targets cannot be kept within the import limits (empty when they can), given the deltas left
		// in the model's budget.
		std::string MorphLimitProblem(const cgltf_mesh& mesh, size_t budget)
		{
			const size_t targets = mesh.primitives_count > 0 ? mesh.primitives[0].targets_count : 0;
			if (targets > MaxMorphTargets)
				return fmt::format("{} morph targets exceed the limit of {} per mesh", targets, MaxMorphTargets);
			// Only triangle primitives import (the others are skipped with a warning).
			size_t deltas = 0;
			for (cgltf_size p = 0; p < mesh.primitives_count; p++)
				for (cgltf_size a = 0; a < mesh.primitives[p].attributes_count && mesh.primitives[p].type == cgltf_primitive_type_triangles; a++)
					if (const cgltf_attribute& attribute = mesh.primitives[p].attributes[a]; attribute.type == cgltf_attribute_type_position && attribute.data)
						deltas += attribute.data->count * targets;
			if (deltas > budget)
				return fmt::format("its morph targets need {} deltas, past the {} left of the limit of {} per model", deltas, budget, MaxMorphDeltas);
			return {};
		}

		// Morph target names (extras.targetNames, unique within the mesh) and default weights. cgltf_validate already
		// rejects the count mismatches; these checks keep the importer safe on its own.
		bool ReadMorphTargetNames(const cgltf_mesh& mesh, MeshData& meshData, const std::string& key, std::string& outError)
		{
			const size_t targets = mesh.primitives_count > 0 ? mesh.primitives[0].targets_count : 0;
			for (cgltf_size p = 1; p < mesh.primitives_count; p++)
				if (mesh.primitives[p].targets_count != targets)
				{
					outError = fmt::format("primitive {} has {} morph targets, primitive 0 has {}", p, mesh.primitives[p].targets_count, targets);
					return false;
				}
			if ((mesh.weights_count > 0 && mesh.weights_count != targets) || (mesh.target_names_count > 0 && mesh.target_names_count != targets))
			{
				outError = fmt::format("{} default weights and {} target names for {} morph targets", mesh.weights_count, mesh.target_names_count, targets);
				return false;
			}
			std::set<std::string> used;
			for (size_t t = 0; t < targets; t++)
			{
				const char* authored = t < mesh.target_names_count ? mesh.target_names[t] : nullptr;
				const std::string base = authored && authored[0] ? authored : "Target" + std::to_string(t);
				std::string name = base;
				for (size_t suffix = 1; !used.insert(name).second; suffix++)
					name = base + "_" + std::to_string(suffix);
				// Scripts address targets by name, so a renamed one is worth knowing about.
				if (name != base)
					BS_CORE_WARN("MeshImporter: {}: mesh '{}': morph target {} is named '{}' like an earlier one; it is '{}'", key, meshData.Name, t, base, name);
				meshData.MorphTargets.push_back(std::move(name));
				const float weight = t < mesh.weights_count ? mesh.weights[t] : 0.0f;
				if (!std::isfinite(weight))
				{
					outError = fmt::format("default weight {} of morph target '{}' is not finite", t, meshData.MorphTargets.back());
					return false;
				}
				meshData.MorphWeights.push_back(weight);
			}
			return true;
		}

		// Reads `targets` morph targets of a primitive into target-major deltas. A target without NORMAL deltas takes the
		// change in generated normals between the base and the fully morphed shape, so lighting follows the new surface.
		// outIgnored is set when a target carries attributes other than POSITION, NORMAL and TANGENT, which are dropped.
		bool ReadMorphTargets(const cgltf_primitive& primitive, size_t targets, const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, std::vector<MorphDelta>& outDeltas, size_t& budget, bool& outIgnored, std::string& outError)
		{
			if (targets == 0)
				return true;
			// MorphLimitProblem checked the whole mesh first; this keeps the budget honest on its own.
			if (vertices.size() > budget / targets)
			{
				outError = fmt::format("morph targets exceed the limit of {} deltas per model", MaxMorphDeltas);
				return false;
			}
			budget -= vertices.size() * targets;
			outDeltas.assign(targets * vertices.size(), MorphDelta{});
			std::vector<Vertex> generatedBase;
			std::vector<glm::vec3> data;
			for (size_t t = 0; t < targets; t++)
			{
				const cgltf_morph_target& target = primitive.targets[t];
				const cgltf_accessor* positions = nullptr;
				const cgltf_accessor* normals = nullptr;
				for (cgltf_size a = 0; a < target.attributes_count; a++)
				{
					const cgltf_attribute& attribute = target.attributes[a];
					if (attribute.type == cgltf_attribute_type_position && attribute.index == 0)
						positions = attribute.data;
					else if (attribute.type == cgltf_attribute_type_normal && attribute.index == 0)
						normals = attribute.data;
					else if (attribute.type != cgltf_attribute_type_tangent)
						outIgnored = true;
				}
				MorphDelta* deltas = outDeltas.data() + t * vertices.size();
				const auto read = [&](const cgltf_accessor* accessor, const char* name, glm::vec3 MorphDelta::* field) {
					if (!accessor)
						return true;
					const char* problem = nullptr;
					if (accessor->count != vertices.size())
						problem = "has a different count than POSITION";
					else if (!ReadAccessor(accessor, data, 3))
						problem = "is not a readable VEC3 accessor";
					else if (!std::ranges::all_of(data, [](const glm::vec3& v) { return Math::IsFinite(v); }))
						problem = "holds non-finite values";
					if (problem)
					{
						outError = fmt::format("morph target {} {} accessor {}", t, name, problem);
						return false;
					}
					for (size_t v = 0; v < vertices.size(); v++)
						deltas[v].*field = data[v];
					return true;
				};
				if (!read(positions, "POSITION", &MorphDelta::Position) || !read(normals, "NORMAL", &MorphDelta::Normal))
					return false;
				if (!positions || normals)
					continue;
				if (generatedBase.empty())
				{
					generatedBase = vertices;
					MeshSource::GenerateNormals(generatedBase, indices);
				}
				std::vector<Vertex> morphed = vertices;
				for (size_t v = 0; v < vertices.size(); v++)
					morphed[v].Position += deltas[v].Position;
				MeshSource::GenerateNormals(morphed, indices);
				for (size_t v = 0; v < vertices.size(); v++)
					deltas[v].Normal = morphed[v].Normal - generatedBase[v].Normal;
			}
			return true;
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

	std::optional<std::string> MeshImporter::GetExternalFileKey(const std::string& modelKey, const char* uri)
	{
		if (!uri || IsDataUri(uri))
			return std::nullopt;
		std::string decoded = uri;
		cgltf_decode_uri(decoded.data());
		decoded.resize(std::strlen(decoded.c_str()));
		return FileSystem::PathToGenericUtf8((FileSystem::PathFromUtf8(modelKey).parent_path() / FileSystem::PathFromUtf8(decoded)).lexically_normal());
	}

	std::optional<MeshImporter::Contents> MeshImporter::Inspect(const std::filesystem::path& path, std::string& outError)
	{
		cgltf_options options = {};
		cgltf_data* data = nullptr;
		const std::string file = FileSystem::PathToUtf8(path);
		if (const cgltf_result result = cgltf_parse_file(&options, file.c_str(), &data); result != cgltf_result_success)
		{
			outError = fmt::format("not a readable glTF ({})", ResultToString(result));
			return std::nullopt;
		}
		Contents contents;
		contents.Materials = data->materials_count;
		for (cgltf_size i = 0; i < data->images_count; i++)
		{
			const cgltf_image& image = data->images[i];
			contents.ImageUris.push_back(image.buffer_view || !image.uri || IsDataUri(image.uri) ? std::string() : std::string(image.uri));
		}
		cgltf_free(data);
		return contents;
	}

	std::optional<std::string> MeshImporter::CheckSubAsset(const Contents& contents, std::string_view key)
	{
		const std::string model(GetAssetSource(key));
		if (const auto material = ParseSubAssetIndex(key, ImportedMaterialMarker))
		{
			if (*material >= contents.Materials)
				return fmt::format("'{}' has {} material(s)", model, contents.Materials);
			return std::nullopt;
		}
		const auto image = ParseSubAssetIndex(key, EmbeddedImageMarker);
		if (!image)
			return fmt::format("'{}' is not a model's embedded image or material (<model>#image/<n>, <model>#material/<n>)", key);
		if (*image >= contents.ImageUris.size())
			return fmt::format("'{}' has {} image(s)", model, contents.ImageUris.size());
		if (!contents.ImageUris[*image].empty())
			return fmt::format("image {} of '{}' is the external file '{}'; reference that file instead", *image, model, contents.ImageUris[*image]);
		return std::nullopt;
	}

	Ref<MeshSource> MeshImporter::LoadGltf(const std::filesystem::path& path, const std::string& key, std::string& outError)
	{
		std::vector<std::string> externalFiles;
		return LoadGltf(path, key, externalFiles, outError);
	}

	Ref<MeshSource> MeshImporter::LoadGltf(const std::filesystem::path& path, const std::string& key, std::vector<std::string>& outExternalFiles, std::string& outError)
	{
		const auto root = Project::GetActive() ? Project::GetActive()->GetDirectory() : path.parent_path();
		return LoadGltf(path, key, root, outExternalFiles, outError);
	}

	Ref<MeshSource> MeshImporter::LoadGltf(const std::filesystem::path& path, const std::string& key, const std::filesystem::path& projectRoot, std::vector<std::string>& outExternalFiles, std::string& outError)
	{
		BS_PROFILE_SCOPE("Import glTF");
		outExternalFiles.clear();
		GltfData data = LoadGltfData(path, key, projectRoot, outExternalFiles, outError);
		if (!data)
			return nullptr;

		if (((data->skins_count || data->animations_count) && data->nodes_count > 65536) || data->skins_count > 4096 || data->animations_count > 4096)
		{
			outError = key + ": exceeds model limits (65536 nodes, 4096 skins/clips)";
			return nullptr;
		}
		Ref<MeshSource> source = CreateRef<MeshSource>();
		source->Key = key;
		source->ExternalFiles = outExternalFiles;

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
				result.AlbedoMap = TextureKey(data.get(), pbr.base_color_texture, key);
				result.MetallicRoughnessMap = TextureKey(data.get(), pbr.metallic_roughness_texture, key);
			}
			result.NormalMap = TextureKey(data.get(), material.normal_texture, key);
			result.OcclusionMap = TextureKey(data.get(), material.occlusion_texture, key);
			result.EmissiveMap = TextureKey(data.get(), material.emissive_texture, key);
			result.EmissiveColor = { material.emissive_factor[0], material.emissive_factor[1], material.emissive_factor[2] };
			if (material.has_emissive_strength)
				result.EmissiveIntensity = material.emissive_strength.emissive_strength;

			result.Clearcoat = material.has_clearcoat ? material.clearcoat.clearcoat_factor : 0.0f;
			result.ClearcoatRoughness = material.has_clearcoat ? material.clearcoat.clearcoat_roughness_factor : 0.0f;
			result.Transmission = material.has_transmission ? material.transmission.transmission_factor : 0.0f;
			result.ClearcoatMap = TextureKey(data.get(), material.clearcoat.clearcoat_texture, key);
			result.ClearcoatRoughnessMap = TextureKey(data.get(), material.clearcoat.clearcoat_roughness_texture, key);
			result.ClearcoatNormalMap = TextureKey(data.get(), material.clearcoat.clearcoat_normal_texture, key);
			result.TransmissionMap = TextureKey(data.get(), material.transmission.transmission_texture, key);
			result.NormalScale = material.normal_texture.texture ? material.normal_texture.scale : 1.0f;
			result.OcclusionStrength = material.occlusion_texture.texture ? material.occlusion_texture.scale : 1.0f;
			result.ClearcoatNormalScale = material.clearcoat.clearcoat_normal_texture.texture ? material.clearcoat.clearcoat_normal_texture.scale : 1.0f;
			const std::string where = key + ": material '" + result.Name + "'";
			// In MaterialTextureSlot order.
			static constexpr std::array<const char*, MaterialTextureSlotCount> SlotNames = { "baseColorTexture", "normalTexture", "metallicRoughnessTexture", "occlusionTexture", "emissiveTexture", "clearcoatTexture", "clearcoatRoughnessTexture", "clearcoatNormalTexture", "transmissionTexture" };
			const std::array<const cgltf_texture_view*, MaterialTextureSlotCount> views = { &material.pbr_metallic_roughness.base_color_texture, &material.normal_texture, &material.pbr_metallic_roughness.metallic_roughness_texture, &material.occlusion_texture, &material.emissive_texture, &material.clearcoat.clearcoat_texture, &material.clearcoat.clearcoat_roughness_texture, &material.clearcoat.clearcoat_normal_texture, &material.transmission.transmission_texture };
			for (size_t slot = 0; slot < result.TextureTransforms.size(); slot++)
			{
				const auto& v = *views[slot];
				auto& transform = result.TextureTransforms[slot];
				if (!v.texture)
					continue;
				const int coordinate = v.has_transform && v.transform.has_texcoord ? v.transform.texcoord : v.texcoord;
				if (coordinate < 0 || coordinate > 1)
				{
					outError = fmt::format("{} {}: TEXCOORD_{} is unsupported (only TEXCOORD_0 and TEXCOORD_1)", where, SlotNames[slot], coordinate);
					return nullptr;
				}
				transform.TexCoord = static_cast<uint32_t>(coordinate);
				if (v.has_transform)
				{
					transform.Offset = { v.transform.offset[0], v.transform.offset[1] };
					transform.Scale = { v.transform.scale[0], v.transform.scale[1] };
					transform.Rotation = v.transform.rotation;
				}
				if (!Math::IsFinite(transform.Offset) || !Math::IsFinite(transform.Scale) || !std::isfinite(transform.Rotation))
				{
					outError = fmt::format("{} {}: non-finite KHR_texture_transform", where, SlotNames[slot]);
					return nullptr;
				}
				if (v.texture->has_basisu && !v.texture->basisu_image)
				{
					outError = fmt::format("{} {}: KHR_texture_basisu has no source image", where, SlotNames[slot]);
					return nullptr;
				}
			}
			const auto unit = [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; };
			const std::pair<const char*, float> unitFactors[] = { { "clearcoatFactor", result.Clearcoat }, { "clearcoatRoughnessFactor", result.ClearcoatRoughness }, { "transmissionFactor", result.Transmission }, { "occlusionTexture.strength", result.OcclusionStrength } };
			for (const auto& [name, value] : unitFactors)
				if (!unit(value))
				{
					outError = fmt::format("{}: {} {} must be in [0, 1]", where, name, value);
					return nullptr;
				}
			if (!std::isfinite(result.EmissiveIntensity) || result.EmissiveIntensity < 0)
			{
				outError = fmt::format("{}: emissiveStrength {} must be finite and nonnegative", where, result.EmissiveIntensity);
				return nullptr;
			}
			const std::pair<const char*, float> scales[] = { { "normalTexture.scale", result.NormalScale }, { "clearcoatNormalTexture.scale", result.ClearcoatNormalScale } };
			for (const auto& [name, value] : scales)
				if (!std::isfinite(value))
				{
					outError = fmt::format("{}: {} {} must be finite", where, name, value);
					return nullptr;
				}
			result.Alpha = material.alpha_mode == cgltf_alpha_mode_mask ? AlphaMode::Mask : (material.alpha_mode == cgltf_alpha_mode_blend ? AlphaMode::Blend : AlphaMode::Opaque);
			result.AlphaCutoff = material.alpha_cutoff;
			result.DoubleSided = material.double_sided;
			// The glTF-named checks above explain most problems; this catches the rest (non-finite factors) so an
			// imported material can always be extracted to a material file and previewed.
			if (auto problem = MaterialAsset::Validate(result))
			{
				outError = where + ": " + *problem;
				return nullptr;
			}
			source->Materials.push_back(std::move(result));
		}
		const uint32_t defaultMaterial = static_cast<uint32_t>(source->Materials.size());
		source->Materials.push_back(MaterialData{ .Name = "Default" });

		// Geometry.
		size_t morphBudget = MaxMorphDeltas;
		std::vector<bool> droppedMorphs;
		for (cgltf_size meshIndex = 0; meshIndex < data->meshes_count; meshIndex++)
		{
			const cgltf_mesh& mesh = data->meshes[meshIndex];
			MeshData meshData;
			meshData.Name = mesh.name ? mesh.name : "Mesh" + std::to_string(meshIndex);
			// Morph targets past the import limits are dropped rather than failing the model: its geometry still loads.
			const std::string morphProblem = MorphLimitProblem(mesh, morphBudget);
			droppedMorphs.push_back(!morphProblem.empty());
			if (!morphProblem.empty())
				BS_CORE_WARN("MeshImporter: {}: mesh '{}': {}; it keeps its base shape without morph targets", key, meshData.Name, morphProblem);
			else if (!ReadMorphTargetNames(mesh, meshData, key, outError))
			{
				outError = fmt::format("{}: mesh '{}': {}", key, meshData.Name, outError);
				return nullptr;
			}
			bool ignoredMorphAttributes = false;

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
				const cgltf_accessor* texCoords1 = nullptr;
				const cgltf_accessor* joints = nullptr;
				const cgltf_accessor* weights = nullptr;
				for (cgltf_size a = 0; a < primitive.attributes_count; a++)
				{
					const cgltf_attribute& attribute = primitive.attributes[a];
					if ((attribute.type == cgltf_attribute_type_joints || attribute.type == cgltf_attribute_type_weights) && attribute.index != 0)
					{
						source->AnimationUnsupported = true;
						continue;
					}
					if (attribute.type == cgltf_attribute_type_joints)
						joints = attribute.data;
					else if (attribute.type == cgltf_attribute_type_weights)
						weights = attribute.data;
					else if (attribute.type == cgltf_attribute_type_position)
						positions = attribute.data;
					else if (attribute.type == cgltf_attribute_type_normal)
						normals = attribute.data;
					else if (attribute.type == cgltf_attribute_type_tangent)
						tangents = attribute.data;
					else if (attribute.type == cgltf_attribute_type_texcoord && attribute.index == 1)
						texCoords1 = attribute.data;
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

				if (joints || weights)
				{
					std::vector<glm::vec4> jointData, weightData;
					if (!joints || !weights || joints->count != positions->count || weights->count != positions->count ||
						(joints->component_type != cgltf_component_type_r_8u && joints->component_type != cgltf_component_type_r_16u) || joints->normalized ||
						!((weights->component_type == cgltf_component_type_r_32f && !weights->normalized) ||
						  ((weights->component_type == cgltf_component_type_r_8u || weights->component_type == cgltf_component_type_r_16u) && weights->normalized)) ||
						!ReadAccessor(joints, jointData, 4) || !ReadAccessor(weights, weightData, 4))
					{
						outError = key + ": mesh '" + meshData.Name + "' primitive " + std::to_string(primitiveIndex) + ": invalid joint/weight accessor";
						return nullptr;
					}
					for (size_t v = 0; v < vertices.size(); v++)
					{
						for (int k = 0; k < 4; k++)
						{
							if (!std::isfinite(weightData[v][k]) || weightData[v][k] < 0.0f)
							{
								outError = key + ": mesh '" + meshData.Name + "' primitive " + std::to_string(primitiveIndex) + ": skin weights must be finite and nonnegative";
								return nullptr;
							}
						}
						const float sum = glm::dot(weightData[v], glm::vec4(1));
						if (!std::isfinite(sum) || sum <= 0.0f)
						{
							outError = key + ": mesh '" + meshData.Name + "' primitive " + std::to_string(primitiveIndex) + ": skin weights must have a positive finite sum";
							return nullptr;
						}
						vertices[v].Joints = glm::uvec4(jointData[v]);
						vertices[v].Weights = weightData[v] / sum;
					}
				}

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

				const bool hasUV1 = texCoords1 && texCoords1->count == positions->count && ReadAccessor(texCoords1, texCoordData, 2);
				if (hasUV1)
					for (size_t v = 0; v < vertices.size(); v++)
						vertices[v].TexCoord1 = texCoordData[v];

				if (primitive.material && !hasUV1)
					for (const auto& transform : source->Materials[static_cast<size_t>(primitive.material - data->materials)].TextureTransforms)
						if (transform.TexCoord == 1)
						{
							const char* problem = !texCoords1 ? "has no TEXCOORD_1" : (texCoords1->count != positions->count ? "has a TEXCOORD_1 whose count differs from POSITION" : "has an unreadable TEXCOORD_1");
							outError = fmt::format("{}: material '{}' uses TEXCOORD_1 but mesh '{}' {}", key, source->Materials[static_cast<size_t>(primitive.material - data->materials)].Name, meshData.Name, problem);
							return nullptr;
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

				const uint32_t targets = static_cast<uint32_t>(meshData.MorphTargets.size());
				std::vector<MorphDelta> morphDeltas;
				if (!ReadMorphTargets(primitive, targets, vertices, indices, morphDeltas, morphBudget, ignoredMorphAttributes, outError))
				{
					outError = fmt::format("{}: mesh '{}' primitive {}: {}", key, meshData.Name, primitiveIndex, outError);
					return nullptr;
				}

				const uint32_t materialIndex = primitive.material ? static_cast<uint32_t>(primitive.material - data->materials) : defaultMaterial;
				const bool reorder = !source->Materials[materialIndex].IsBlended();
				const bool optimized = MeshOptimizer::Optimize(vertices, indices, reorder, &morphDeltas, targets);
				if (!optimized)
				{
					BS_CORE_WARN("MeshImporter: {}: optimization and LODs skipped for empty, malformed or non-finite geometry in mesh '{}'", path.filename().string(), meshData.Name);
				}
				const auto submesh = source->AddSubmesh(vertices, indices, materialIndex, morphDeltas, targets);
				if (optimized)
					MeshOptimizer::GenerateLODs(*source, submesh);
				meshData.Submeshes.push_back(submesh);
			}
			if (ignoredMorphAttributes)
				BS_CORE_WARN("MeshImporter: {}: mesh '{}': morph targets keep only POSITION and NORMAL deltas; their other attributes are ignored", key, meshData.Name);
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
			result.SkinIndex = node.skin ? static_cast<int32_t>(node.skin - data->skins) : -1;
			if (result.MeshIndex >= 0)
			{
				const MeshData& mesh = source->Meshes[result.MeshIndex];
				// A mesh whose morph targets were dropped (MorphLimitProblem) ignores the node's weights too.
				const bool dropped = droppedMorphs[static_cast<size_t>(result.MeshIndex)];
				if (!dropped && node.weights_count > 0 && node.weights_count != mesh.MorphTargets.size())
				{
					outError = fmt::format("{}: node '{}' has {} weights for {} morph targets", key, result.Name, node.weights_count, mesh.MorphTargets.size());
					return nullptr;
				}
				result.MorphWeights = node.weights_count > 0 && !dropped ? std::vector<float>(node.weights, node.weights + node.weights_count) : mesh.MorphWeights;
				if (!std::ranges::all_of(result.MorphWeights, [](float w) { return std::isfinite(w); }))
				{
					outError = fmt::format("{}: node '{}' has non-finite morph weights", key, result.Name);
					return nullptr;
				}
			}
			if (node.has_matrix)
			{
				glm::mat4 matrix;
				std::memcpy(&matrix, node.matrix, sizeof(float) * 16);
				result.HasMatrix = true;
				result.Matrix = matrix;
				if (!Math::DecomposeAffineTransform(matrix, result.Translation, result.Rotation, result.Scale))
				{
					outError = key + ": node matrix cannot be decomposed";
					return nullptr;
				}
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
			glm::vec4 rotation(result.Rotation.x, result.Rotation.y, result.Rotation.z, result.Rotation.w);
			if (!Math::NormalizeQuaternion(rotation))
			{
				outError = key + ": invalid node rotation";
				return nullptr;
			}
			result.Rotation = glm::quat(rotation.w, rotation.x, rotation.y, rotation.z);
			source->Nodes.push_back(std::move(result));
		}
		// A file with meshes but no nodes still gets something to instantiate.
		if (source->Nodes.empty())
		{
			for (size_t m = 0; m < source->Meshes.size(); m++)
				source->Nodes.push_back(MeshNode{ .Name = source->Meshes[m].Name, .MeshIndex = static_cast<int32_t>(m), .MorphWeights = source->Meshes[m].MorphWeights });
		}

		if (!ImportAnimations(*data, *source, outError))
			return nullptr;
		if (source->AnimationUnsupported)
		{
			BS_CORE_WARN("MeshImporter: {}: more than four skin influences per vertex; retaining static geometry only", key);
			// Static MeshComponents draw their mesh's default weights, so node weights that differ are lost.
			for (const MeshNode& node : source->Nodes)
				if (node.MeshIndex >= 0 && node.MorphWeights != source->Meshes[node.MeshIndex].MorphWeights)
					BS_CORE_WARN("MeshImporter: {}: node '{}' has its own morph weights, which the static geometry cannot show; it shows mesh '{}''s defaults", key, node.Name, source->Meshes[node.MeshIndex].Name);
		}

		std::vector<glm::mat4> rest;
		if (!EvaluateAnimation(*source, nullptr, 0, rest))
		{
			outError = key + ": non-finite node hierarchy";
			return nullptr;
		}
		for (const auto& skin : source->Skins)
			if (BuildSkinPalette(skin, rest).empty())
			{
				outError = key + ": non-finite skin bind transform";
				return nullptr;
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

		// Morph targets need an Animator too: node weights, weight clips and script weights live in its pose.
		if (!source.AnimationUnsupported && (!source.Skins.empty() || !source.Animations.empty() || source.MorphWeightCount > 0))
		{
			root.AddComponent<AnimatorComponent>().Mesh = source.Key;
			return root;
		}

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
