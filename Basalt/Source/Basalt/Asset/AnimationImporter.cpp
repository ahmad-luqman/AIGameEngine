#include "Basalt/Asset/AnimationImporter.h"

#include <cgltf.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <set>

namespace Basalt {

	namespace {
		bool Read(const cgltf_accessor* accessor, cgltf_type type, std::vector<float>& values, size_t& remainingFloats)
		{
			if (!accessor || accessor->type != type || accessor->component_type != cgltf_component_type_r_32f || accessor->normalized)
				return false;
			const size_t width = cgltf_num_components(type);
			if (accessor->count > remainingFloats / width)
				return false;
			remainingFloats -= accessor->count * width;
			values.resize(accessor->count * width);
			return cgltf_accessor_unpack_floats(accessor, values.data(), values.size()) == values.size() &&
				   std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); });
		}
	}

	bool ImportAnimations(const cgltf_data& data, MeshSource& source, std::string& outError)
	{
		// At most 64 MiB of decoded animation/skin floats per asset, including repeated sampler references.
		size_t remainingFloats = size_t{ 16 } * 1024 * 1024;
		auto fail = [&](const std::string& reason) { outError = source.Key + ": " + reason; return false; };
		// Iterative traversal avoids stack growth for adversarially deep hierarchies.
		std::vector<std::vector<uint32_t>> children(source.Nodes.size());
		source.NodeOrder.reserve(source.Nodes.size());
		for (size_t i = 0; i < source.Nodes.size(); i++)
		{
			const auto& node = source.Nodes[i];
			if (node.Parent < 0)
				source.NodeOrder.push_back(static_cast<uint32_t>(i));
			else if (static_cast<size_t>(node.Parent) < source.Nodes.size())
				children[node.Parent].push_back(static_cast<uint32_t>(i));
			else
				return fail("invalid node parent");
		}
		for (size_t i = 0; i < source.NodeOrder.size(); i++)
			for (uint32_t child : children[source.NodeOrder[i]])
				source.NodeOrder.push_back(child);
		if (source.NodeOrder.size() != source.Nodes.size())
			return fail("cyclic node hierarchy");
		if (data.scene)
		{
			for (auto& node : source.Nodes)
				node.Active = false;
			for (size_t i = 0; i < data.scene->nodes_count; i++)
				source.Nodes[data.scene->nodes[i] - data.nodes].Active = true;
			for (uint32_t i : source.NodeOrder)
				if (source.Nodes[i].Parent >= 0 && source.Nodes[source.Nodes[i].Parent].Active)
					source.Nodes[i].Active = true;
		}
		for (size_t i = 0; i < data.skins_count; i++)
		{
			const auto& input = data.skins[i];
			if (input.joints_count == 0 || input.joints_count > 65536)
				return fail("skin must contain 1..65536 joints");
			MeshSkin skin;
			std::set<uint32_t> unique;
			for (size_t j = 0; j < input.joints_count; j++)
			{
				const auto index = static_cast<uint32_t>(input.joints[j] - data.nodes);
				if (!unique.insert(index).second)
					return fail("duplicate skin joint");
				skin.Joints.push_back(index);
			}
			if (input.joints_count > remainingFloats / 16)
				return fail("skin data exceeds the decoded animation budget");
			if (!input.inverse_bind_matrices)
				remainingFloats -= input.joints_count * 16;
			skin.InverseBindMatrices.assign(input.joints_count, glm::mat4(1));
			if (input.inverse_bind_matrices)
			{
				std::vector<float> values;
				if (input.inverse_bind_matrices->count < input.joints_count || !Read(input.inverse_bind_matrices, cgltf_type_mat4, values, remainingFloats))
					return fail("invalid inverse bind matrices");
				for (size_t j = 0; j < input.joints_count; j++)
				{
					auto matrix = glm::make_mat4(values.data() + j * 16);
					if (matrix[0].w != 0 || matrix[1].w != 0 || matrix[2].w != 0 || matrix[3].w != 1)
						return fail("inverse bind matrix must be affine");
					skin.InverseBindMatrices[j] = matrix;
				}
			}
			source.Skins.push_back(std::move(skin));
		}
		for (const auto& node : source.Nodes)
		{
			if (node.SkinIndex < 0 || node.MeshIndex < 0)
				continue;
			const auto& skin = source.Skins[node.SkinIndex];
			for (uint32_t submeshIndex : source.Meshes[node.MeshIndex].Submeshes)
			{
				const auto& submesh = source.Submeshes[submeshIndex];
				for (size_t v = submesh.BaseVertex; v < static_cast<size_t>(submesh.BaseVertex) + submesh.VertexCount; v++)
				{
					const auto& vertex = source.Vertices[v];
					if (glm::dot(vertex.Weights, glm::vec4(1)) <= 0)
						return fail("skinned primitive needs JOINTS_0 and WEIGHTS_0");
					for (int k = 0; k < 4; k++)
						if (vertex.Joints[k] >= skin.Joints.size())
							return fail("vertex joint index exceeds skin joint count");
				}
			}
		}
		std::set<std::string> clipNames;
		for (size_t i = 0; i < data.animations_count; i++)
		{
			const auto& input = data.animations[i];
			AnimationClip clip;
			clip.Name = input.name && input.name[0] ? input.name : "Animation" + std::to_string(i);
			// Names are the public selection key; make duplicates deterministic and selectable.
			const std::string baseName = clip.Name;
			for (size_t suffix = 1; !clipNames.insert(clip.Name).second; suffix++)
				clip.Name = baseName + "_" + std::to_string(suffix);
			std::set<std::pair<uint32_t, AnimationPath>> targets;
			for (size_t c = 0; c < input.channels_count; c++)
			{
				const auto& channel = input.channels[c];
				if (!channel.target_node || !channel.sampler)
					return fail("animation channel needs a target and sampler");
				if (channel.target_node->has_matrix)
					return fail("animated node must use TRS, not matrix");
				AnimationTrack track;
				track.Node = static_cast<uint32_t>(channel.target_node - data.nodes);
				switch (channel.target_path)
				{
					case cgltf_animation_path_type_translation:
						track.Path = AnimationPath::Translation;
						break;
					case cgltf_animation_path_type_rotation:
						track.Path = AnimationPath::Rotation;
						break;
					case cgltf_animation_path_type_scale:
						track.Path = AnimationPath::Scale;
						break;
					default:
						return fail("morph-weight animation is not supported");
				}
				if (!targets.emplace(track.Node, track.Path).second)
					return fail("duplicate animation target");
				const auto& sampler = *channel.sampler;
				switch (sampler.interpolation)
				{
					case cgltf_interpolation_type_step:
						track.Interpolation = AnimationInterpolation::Step;
						break;
					case cgltf_interpolation_type_linear:
						track.Interpolation = AnimationInterpolation::Linear;
						break;
					case cgltf_interpolation_type_cubic_spline:
						track.Interpolation = AnimationInterpolation::CubicSpline;
						break;
					default:
						return fail("unsupported animation interpolation");
				}
				if (!Read(sampler.input, cgltf_type_scalar, track.Times, remainingFloats) || track.Times.empty())
					return fail("invalid animation times");
				float previous = -1;
				for (float time : track.Times)
				{
					if (time < 0 || time <= previous)
						return fail("animation times must be nonnegative and strictly increasing");
					previous = time;
				}
				clip.Duration = std::max(clip.Duration, track.Times.back());
				const bool rotation = track.Path == AnimationPath::Rotation;
				const bool cubic = track.Interpolation == AnimationInterpolation::CubicSpline;
				std::vector<float> values;
				if (!Read(sampler.output, rotation ? cgltf_type_vec4 : cgltf_type_vec3, values, remainingFloats) || sampler.output->count != track.Times.size() * (cubic ? 3 : 1))
					return fail("invalid animation values");
				const size_t width = rotation ? 4 : 3;
				// vec3 tracks are stored in vec4 slots; charge their padding too.
				if (!rotation)
				{
					if (sampler.output->count > remainingFloats)
						return fail("animation values exceed the decoded animation budget");
					remainingFloats -= sampler.output->count;
				}
				for (size_t v = 0; v < sampler.output->count; v++)
				{
					glm::vec4 value(0);
					for (size_t k = 0; k < width; k++)
						value[static_cast<int>(k)] = values[v * width + k];
					if (rotation && (!cubic || v % 3 == 1))
					{
						const float length = glm::length(value);
						if (!std::isfinite(length) || length < 1e-6f)
							return fail("invalid animation quaternion");
						value /= length;
					}
					track.Values.push_back(value);
				}
				clip.Tracks.push_back(std::move(track));
			}
			source.Animations.push_back(std::move(clip));
		}
		return true;
	}
}
