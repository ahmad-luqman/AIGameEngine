#include "Basalt/Asset/AnimationImporter.h"

#include "Basalt/Core/Log.h"
#include "Basalt/Math/Math.h"

#include <cgltf.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <set>

namespace Basalt {

	namespace {
		// Accounts for decoded values, padded vec3 slots and implicit inverse-bind matrices.
		class FloatBudget
		{
		public:
			bool Fits(size_t count, size_t width = 1) const { return count <= m_Remaining / width; }
			bool Consume(size_t count, size_t width = 1)
			{
				if (!Fits(count, width))
				{
					m_Exceeded = true;
					return false;
				}
				m_Remaining -= count * width;
				return true;
			}
			bool Exceeded() const { return m_Exceeded; }

		private:
			size_t m_Remaining = size_t{ 16 } * 1024 * 1024;
			bool m_Exceeded = false;
		};

		bool Read(const cgltf_accessor* accessor, cgltf_type type, std::vector<float>& values, FloatBudget& budget, bool allowQuantized = false)
		{
			if (!accessor || accessor->type != type)
				return false;
			const bool floating = accessor->component_type == cgltf_component_type_r_32f && !accessor->normalized;
			const bool quantized = allowQuantized && accessor->normalized &&
								   (accessor->component_type == cgltf_component_type_r_8 || accessor->component_type == cgltf_component_type_r_8u ||
									accessor->component_type == cgltf_component_type_r_16 || accessor->component_type == cgltf_component_type_r_16u);
			if (!floating && !quantized)
				return false;
			const size_t width = cgltf_num_components(type);
			if (!budget.Consume(accessor->count, width))
				return false;
			values.resize(accessor->count * width);
			return cgltf_accessor_unpack_floats(accessor, values.data(), values.size()) == values.size() &&
				   std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); });
		}
	}

	namespace {
		// Imports validated hierarchy, skins and tracks under one decoded-data budget.
		class AnimationImport
		{
		public:
			AnimationImport(const cgltf_data& data, MeshSource& source, std::string& error)
				: m_Data(data)
				, m_Source(source)
				, m_Error(error)
			{}
			bool Run() { return Hierarchy() && Skins() && Vertices() && Tracks(); }

		private:
			bool Fail(const std::string& reason)
			{
				m_Error = m_Source.Key + ": " + m_Context + (m_Budget.Exceeded() ? "decoded animation budget exceeded: " : "") + reason;
				return false;
			}
			bool Hierarchy()
			{
				std::string problem;
				if (!m_Source.FinalizeHierarchy(&problem))
					return Fail(problem);
				if (m_Data.scene)
				{
					for (auto& node : m_Source.Nodes)
						node.Active = false;
					for (size_t i = 0; i < m_Data.scene->nodes_count; i++)
						m_Source.Nodes[m_Data.scene->nodes[i] - m_Data.nodes].Active = true;
					for (uint32_t i : m_Source.NodeOrder)
						if (m_Source.Nodes[i].Parent >= 0 && m_Source.Nodes[m_Source.Nodes[i].Parent].Active)
							m_Source.Nodes[i].Active = true;
				}
				return true;
			}
			bool Skins()
			{
				for (size_t i = 0; i < m_Data.skins_count; i++)
				{
					const auto& input = m_Data.skins[i];
					m_Context = "skin " + std::to_string(i) + (input.name ? " (" + std::string(input.name) + ")" : "") + ": ";
					if (input.joints_count == 0 || input.joints_count > 65536)
						return Fail("skin must contain 1..65536 joints");
					MeshSkin skin;
					std::set<uint32_t> unique;
					for (size_t j = 0; j < input.joints_count; j++)
					{
						const auto index = static_cast<uint32_t>(input.joints[j] - m_Data.nodes);
						if (!unique.insert(index).second)
							return Fail("duplicate skin joint");
						skin.Joints.push_back({ index, glm::mat4(1) });
					}
					if (!m_Budget.Fits(input.joints_count, 16))
						return Fail("skin data exceeds the decoded animation budget");
					if (!input.inverse_bind_matrices)
						m_Budget.Consume(input.joints_count, 16);
					if (input.inverse_bind_matrices)
					{
						std::vector<float> values;
						if (input.inverse_bind_matrices->count < input.joints_count || !Read(input.inverse_bind_matrices, cgltf_type_mat4, values, m_Budget))
							return Fail("invalid inverse bind matrices");
						for (size_t j = 0; j < input.joints_count; j++)
						{
							auto matrix = glm::make_mat4(values.data() + j * 16);
							if (matrix[0].w != 0 || matrix[1].w != 0 || matrix[2].w != 0 || matrix[3].w != 1)
								return Fail("inverse bind matrix must be affine");
							skin.Joints[j].InverseBind = matrix;
						}
					}
					m_Source.Skins.push_back(std::move(skin));
				}
				return true;
			}
			bool Vertices()
			{
				// Bound per-model skinning work even when many mesh nodes reference the same large skin.
				size_t remainingJointReferences = 65536;
				for (const auto& node : m_Source.Nodes)
				{
					if (node.SkinIndex < 0 || node.MeshIndex < 0)
						continue;
					const auto& skin = m_Source.Skins[node.SkinIndex];
					m_Context = "node '" + node.Name + "' mesh " + std::to_string(node.MeshIndex) + ": ";
					if (node.Active)
					{
						if (skin.Joints.size() > remainingJointReferences)
							return Fail("model exceeds 65536 skin joint references across active mesh nodes");
						remainingJointReferences -= skin.Joints.size();
					}
					for (uint32_t submeshIndex : m_Source.Meshes[node.MeshIndex].Submeshes)
					{
						const auto& submesh = m_Source.Submeshes[submeshIndex];
						for (size_t v = submesh.BaseVertex; v < static_cast<size_t>(submesh.BaseVertex) + submesh.VertexCount; v++)
						{
							const auto& vertex = m_Source.Vertices[v];
							if (glm::dot(vertex.Weights, glm::vec4(1)) <= 0)
								return Fail("skinned primitive needs JOINTS_0 and WEIGHTS_0");
							for (int k = 0; k < 4; k++)
								if (vertex.Joints[k] >= skin.Joints.size())
									return Fail("vertex joint index exceeds skin joint count");
						}
					}
				}
				return true;
			}
			bool Tracks()
			{
				std::set<std::string> clipNames;
				for (size_t i = 0; i < m_Data.animations_count; i++)
				{
					const auto& input = m_Data.animations[i];
					AnimationClip clip;
					clip.Name = input.name && input.name[0] ? input.name : "Animation" + std::to_string(i);
					// Names are the public selection key; make duplicates deterministic and selectable.
					const std::string baseName = clip.Name;
					for (size_t suffix = 1; !clipNames.insert(clip.Name).second; suffix++)
						clip.Name = baseName + "_" + std::to_string(suffix);
					std::set<std::pair<uint32_t, AnimationPath>> targets;
					bool skippedWeights = false;
					for (size_t c = 0; c < input.channels_count; c++)
					{
						const auto& channel = input.channels[c];
						m_Context = "clip '" + clip.Name + "' channel " + std::to_string(c) + ": ";
						if (!channel.target_node)
						{
							BS_CORE_WARN("MeshImporter: {}: clip '{}' channel {}: skipping unsupported pointer animation", m_Source.Key, clip.Name, c);
							continue;
						}
						if (!channel.sampler)
							return Fail("animation channel needs a target and sampler");
						if (channel.target_node->has_matrix && channel.target_path != cgltf_animation_path_type_weights)
							return Fail("animated node must use TRS, not matrix");
						AnimationTrack track;
						track.Node = static_cast<uint32_t>(channel.target_node - m_Data.nodes);
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
							case cgltf_animation_path_type_weights:
								track.Path = AnimationPath::Weights;
								track.Width = static_cast<uint32_t>(m_Source.Nodes[track.Node].MorphWeights.size());
								// cgltf_validate sizes the channel by the mesh's targets, so a node without weights here normally
								// lost its targets to the importer's limits; an empty channel on a target-less mesh lands here too.
								if (track.Width == 0)
								{
									BS_CORE_WARN("MeshImporter: {}: clip '{}' channel {}: skipping the morph weights of node '{}', which has no morph targets (dropped or none)", m_Source.Key, clip.Name, c, m_Source.Nodes[track.Node].Name);
									skippedWeights = true;
									continue;
								}
								break;
							default:
								return Fail("unsupported animation path");
						}
						if (!targets.emplace(track.Node, track.Path).second)
							return Fail("duplicate animation target");
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
								return Fail("unsupported animation interpolation");
						}
						if (!Read(sampler.input, cgltf_type_scalar, track.Times, m_Budget) || track.Times.empty())
							return Fail("invalid animation times");
						float previous = -1;
						for (float time : track.Times)
						{
							if (time < 0 || time <= previous)
								return Fail("animation times must be nonnegative and strictly increasing");
							previous = time;
						}
						clip.Duration = std::max(clip.Duration, track.Times.back());
						if (track.Path == AnimationPath::Weights)
						{
							const size_t values = track.Times.size() * (track.Interpolation == AnimationInterpolation::CubicSpline ? 3 : 1);
							if (sampler.output->count != values * track.Width)
								return Fail(fmt::format("{} morph weight values for {} keys of {} targets (expected {})", sampler.output->count, track.Times.size(), track.Width, values * track.Width));
							if (!Read(sampler.output, cgltf_type_scalar, track.Weights, m_Budget, true))
								return Fail("morph weight values must be finite scalar floats or normalized integers");
							clip.Tracks.push_back(std::move(track));
							continue;
						}
						const bool rotation = track.Path == AnimationPath::Rotation;
						const bool cubic = track.Interpolation == AnimationInterpolation::CubicSpline;
						std::vector<float> values;
						if (!Read(sampler.output, rotation ? cgltf_type_vec4 : cgltf_type_vec3, values, m_Budget, rotation) || sampler.output->count != track.Times.size() * (cubic ? 3 : 1))
							return Fail("invalid animation values");
						const size_t width = rotation ? 4 : 3;
						// vec3 tracks are stored in vec4 slots; charge their padding too.
						if (!rotation)
						{
							if (!m_Budget.Consume(sampler.output->count))
								return Fail("animation values exceed the decoded animation budget");
						}
						for (size_t v = 0; v < sampler.output->count; v++)
						{
							glm::vec4 value(0);
							for (size_t k = 0; k < width; k++)
								value[static_cast<int>(k)] = values[v * width + k];
							if (rotation && (!cubic || v % 3 == 1))
							{
								if (!Math::NormalizeQuaternion(value))
									return Fail("invalid animation quaternion");
							}
							track.Values.push_back(value);
						}
						clip.Tracks.push_back(std::move(track));
					}
					if (!clip.Tracks.empty())
						m_Source.Animations.push_back(std::move(clip));
					else if (skippedWeights)
						BS_CORE_WARN("MeshImporter: {}: clip '{}' has no tracks left after skipping its morph weights, so it is dropped", m_Source.Key, clip.Name);
				}
				return true;
			}

			const cgltf_data& m_Data;
			MeshSource& m_Source;
			std::string& m_Error;
			std::string m_Context;
			FloatBudget m_Budget;
		};
	}

	bool ImportAnimations(const cgltf_data& data, MeshSource& source, std::string& outError)
	{
		return AnimationImport(data, source, outError).Run();
	}
}
