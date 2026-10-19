#include "Basalt/Animation/AnimationPose.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Scene/Components.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace Basalt {

	namespace {
		glm::quat Quaternion(const glm::vec4& v)
		{
			return { v.w, v.x, v.y, v.z };
		}

		glm::vec4 Sample(const AnimationTrack& track, float time)
		{
			const bool cubic = track.Interpolation == AnimationInterpolation::CubicSpline;
			const size_t stride = cubic ? 3 : 1;
			const size_t offset = cubic ? 1 : 0;
			BS_CORE_ASSERT(!track.Times.empty() && track.Values.size() == track.Times.size() * stride);
			const auto upper = std::upper_bound(track.Times.begin(), track.Times.end(), time);
			if (upper == track.Times.begin())
				return track.Values[offset];
			if (upper == track.Times.end())
				return track.Values[(track.Times.size() - 1) * stride + offset];
			const size_t right = static_cast<size_t>(upper - track.Times.begin());
			const size_t left = right - 1;
			const glm::vec4 a = track.Values[left * stride + offset];
			const glm::vec4 b = track.Values[right * stride + offset];
			if (track.Interpolation == AnimationInterpolation::Step)
				return a;
			const float duration = track.Times[right] - track.Times[left];
			const float t = (time - track.Times[left]) / duration;
			glm::vec4 value;
			if (cubic)
			{
				const float t2 = t * t, t3 = t2 * t;
				value = (2 * t3 - 3 * t2 + 1) * a + (t3 - 2 * t2 + t) * duration * track.Values[left * 3 + 2] +
						(-2 * t3 + 3 * t2) * b + (t3 - t2) * duration * track.Values[right * 3];
			}
			else if (track.Path == AnimationPath::Rotation)
			{
				const glm::quat q = glm::slerp(Quaternion(a), Quaternion(b), t);
				value = { q.x, q.y, q.z, q.w };
			}
			else
				value = glm::mix(a, b, t);
			if (track.Path == AnimationPath::Rotation)
			{
				if (!Math::NormalizeQuaternion(value, 1e-8f))
					value = a;
			}
			return value;
		}
	}

	AnimationLocalPose SampleAnimation(const MeshSource& source, const AnimationClip* clip, float time)
	{
		AnimationLocalPose pose;
		for (const auto& node : source.Nodes)
			pose.Nodes.push_back({ node.Translation, node.Rotation, node.Scale });
		if (clip)
			for (const auto& track : clip->Tracks)
			{
				const glm::vec4 value = Sample(track, time);
				switch (track.Path)
				{
					case AnimationPath::Translation:
						pose.Nodes[track.Node].Translation = glm::vec3(value);
						break;
					case AnimationPath::Rotation:
						pose.Nodes[track.Node].Rotation = Quaternion(value);
						break;
					case AnimationPath::Scale:
						pose.Nodes[track.Node].Scale = glm::vec3(value);
						break;
				}
			}
		return pose;
	}

	void BlendAnimationPose(AnimationLocalPose& target, const AnimationLocalPose& from, float weight)
	{
		BS_CORE_ASSERT(target.Nodes.size() == from.Nodes.size());
		for (size_t i = 0; i < target.Nodes.size(); i++)
		{
			target.Nodes[i].Translation = glm::mix(from.Nodes[i].Translation, target.Nodes[i].Translation, weight);
			target.Nodes[i].Rotation = glm::normalize(glm::slerp(from.Nodes[i].Rotation, target.Nodes[i].Rotation, weight));
			target.Nodes[i].Scale = glm::mix(from.Nodes[i].Scale, target.Nodes[i].Scale, weight);
		}
	}

	bool ComposeAnimationPose(const MeshSource& source, const AnimationLocalPose& pose, std::vector<glm::mat4>& globals)
	{
		BS_CORE_ASSERT(source.NodeOrder.size() == source.Nodes.size());
		BS_CORE_ASSERT(pose.Nodes.size() == source.Nodes.size());
		globals.resize(source.Nodes.size());
		for (uint32_t i : source.NodeOrder)
		{
			const glm::mat4 local = source.Nodes[i].HasMatrix ? source.Nodes[i].Matrix : Math::ComposeTransform(pose.Nodes[i].Translation, pose.Nodes[i].Rotation, pose.Nodes[i].Scale);
			globals[i] = source.Nodes[i].Parent >= 0 ? globals[source.Nodes[i].Parent] * local : local;
			if (!Math::IsFinite(globals[i]))
				return false;
		}
		return true;
	}

	std::vector<uint8_t> AnimatedChannels(const MeshSource& source, const AnimationClip& clip)
	{
		std::vector<uint8_t> channels(source.Nodes.size(), 0);
		for (const auto& track : clip.Tracks)
		{
			switch (track.Path)
			{
				case AnimationPath::Translation:
					channels[track.Node] |= AnimatedChannel::Translation;
					break;
				case AnimationPath::Rotation:
					channels[track.Node] |= AnimatedChannel::Rotation;
					break;
				case AnimationPath::Scale:
					channels[track.Node] |= AnimatedChannel::Scale;
					break;
			}
		}
		return channels;
	}

	std::vector<float> AnimationMaskWeights(const MeshSource& source, const std::vector<AnimationMaskJoint>& mask, std::vector<std::string>& outMissing)
	{
		// The inspector adds entries before they are named: until one is, the mask is still empty.
		if (std::ranges::all_of(mask, [](const AnimationMaskJoint& entry) { return entry.Joint.empty(); }))
			return std::vector<float>(source.Nodes.size(), 1.0f);
		std::vector<std::optional<float>> named(source.Nodes.size());
		for (const AnimationMaskJoint& entry : mask)
		{
			if (entry.Joint.empty())
				continue;
			if (const int32_t node = FindNode(source, entry.Joint); node >= 0)
				named[node] = std::isfinite(entry.Weight) ? std::clamp(entry.Weight, 0.0f, 1.0f) : 0.0f;
			else
				outMissing.push_back(entry.Joint);
		}
		std::vector<float> weights(source.Nodes.size(), 0.0f);
		// Parents come before their children in NodeOrder.
		for (uint32_t i : source.NodeOrder)
		{
			const int32_t parent = source.Nodes[i].Parent;
			if (const std::optional<float>& own = named[i])
				weights[i] = *own;
			else if (parent >= 0)
				weights[i] = weights[parent];
		}
		return weights;
	}

	void OverrideAnimationPose(AnimationLocalPose& target, const AnimationLocalPose& layer, const AnimationLayerMask& mask, float weight)
	{
		BS_CORE_ASSERT(target.Nodes.size() == layer.Nodes.size() && mask.Weights.size() == target.Nodes.size() && mask.Channels.size() == target.Nodes.size());
		for (size_t i = 0; i < target.Nodes.size(); i++)
		{
			const float amount = weight * mask.Weights[i];
			const uint8_t channels = mask.Channels[i];
			if (amount <= 0.0f || channels == 0)
				continue;
			AnimationNodePose& node = target.Nodes[i];
			const AnimationNodePose& source = layer.Nodes[i];
			if (channels & AnimatedChannel::Translation)
				node.Translation = glm::mix(node.Translation, source.Translation, amount);
			if (channels & AnimatedChannel::Rotation)
				node.Rotation = glm::normalize(glm::slerp(node.Rotation, source.Rotation, amount));
			if (channels & AnimatedChannel::Scale)
				node.Scale = glm::mix(node.Scale, source.Scale, amount);
		}
	}

	void AddAnimationPose(AnimationLocalPose& target, const AnimationLocalPose& layer, const AnimationLocalPose& reference, const AnimationLayerMask& mask, float weight)
	{
		BS_CORE_ASSERT(target.Nodes.size() == layer.Nodes.size() && reference.Nodes.size() == target.Nodes.size() && mask.Weights.size() == target.Nodes.size() && mask.Channels.size() == target.Nodes.size());
		for (size_t i = 0; i < target.Nodes.size(); i++)
		{
			const float amount = weight * mask.Weights[i];
			const uint8_t channels = mask.Channels[i];
			if (amount <= 0.0f || channels == 0)
				continue;
			AnimationNodePose& node = target.Nodes[i];
			const AnimationNodePose& source = layer.Nodes[i];
			const AnimationNodePose& base = reference.Nodes[i];
			if (channels & AnimatedChannel::Translation)
				node.Translation += amount * (source.Translation - base.Translation);
			if (channels & AnimatedChannel::Rotation)
			{
				const glm::quat delta = glm::normalize(source.Rotation * glm::inverse(base.Rotation));
				node.Rotation = glm::normalize(glm::slerp(glm::quat(1, 0, 0, 0), delta, amount) * node.Rotation);
			}
			if (channels & AnimatedChannel::Scale)
			{
				// A zero reference scale has no ratio; that axis keeps the target's scale.
				glm::vec3 ratio(1.0f);
				for (int axis = 0; axis < 3; axis++)
					if (std::abs(base.Scale[axis]) > 1e-6f)
						ratio[axis] = source.Scale[axis] / base.Scale[axis];
				node.Scale *= glm::mix(glm::vec3(1.0f), ratio, amount);
			}
		}
	}

	int32_t FindNode(const MeshSource& source, const std::string& name)
	{
		const auto node = std::ranges::find(source.Nodes, name, &MeshNode::Name);
		return node == source.Nodes.end() ? -1 : static_cast<int32_t>(node - source.Nodes.begin());
	}

	bool IsAncestor(const MeshSource& source, uint32_t ancestor, uint32_t node)
	{
		for (int32_t parent = source.Nodes[node].Parent; parent >= 0; parent = source.Nodes[parent].Parent)
			if (static_cast<uint32_t>(parent) == ancestor)
				return true;
		return false;
	}

	bool SkinPalettesFinite(const MeshSource& source, const std::vector<glm::mat4>& globals)
	{
		return std::ranges::all_of(source.Skins, [&](const MeshSkin& skin) { return !BuildSkinPalette(skin, globals).empty(); });
	}

	bool EvaluateAnimation(const MeshSource& source, const AnimationClip* clip, float time, std::vector<glm::mat4>& globals)
	{
		return ComposeAnimationPose(source, SampleAnimation(source, clip, time), globals);
	}

	std::vector<glm::mat4> BuildSkinPalette(const MeshSkin& skin, const std::vector<glm::mat4>& globals)
	{
		std::vector<glm::mat4> result;
		result.reserve(skin.Joints.size());
		for (const auto& joint : skin.Joints)
		{
			const glm::mat4 matrix = globals[joint.Node] * joint.InverseBind;
			if (!Math::IsFinite(matrix))
				return {};
			result.push_back(matrix);
		}
		return result;
	}

	AABB CachedSkinBounds(const MeshSource& source, const Submesh& submesh, const std::vector<glm::mat4>& palette)
	{
		if (submesh.JointBounds.empty())
			return SkinBounds(source, submesh, palette);
		AABB bounds;
		if (submesh.HasUnweightedVertices)
			bounds.Expand(glm::vec3(0));
		// For sum s > 0, the skinned point is the convex combination
		// sum_j (weight_j / s) * palette_j * (position * s, s).
		// Union the joint boxes; adding them would incorrectly scale multi-joint vertices.
		for (const auto& influence : submesh.JointBounds)
		{
			if (influence.Joint >= palette.size())
			{
				// The shader drops invalid influences, leaving the missing share at the origin.
				bounds.Expand(glm::vec3(0));
				continue;
			}
			const glm::vec4 low(influence.Positions.Min, influence.MinWeightSum);
			const glm::vec4 high(influence.Positions.Max, influence.MaxWeightSum);
			glm::vec3 minimum(0), maximum(0);
			for (int column = 0; column < 4; ++column)
			{
				const glm::vec3 a = glm::vec3(palette[influence.Joint][column]) * low[column];
				const glm::vec3 b = glm::vec3(palette[influence.Joint][column]) * high[column];
				minimum += glm::min(a, b);
				maximum += glm::max(a, b);
			}
			bounds.Expand(minimum);
			bounds.Expand(maximum);
		}
		// Cover roundoff in the shader's weighted sums at the boundary of a culling plane.
		if (bounds.IsValid())
		{
			const glm::vec3 padding = glm::max(glm::abs(bounds.Min), glm::abs(bounds.Max)) * 1e-5f + glm::vec3(1e-6f);
			bounds.Min -= padding;
			bounds.Max += padding;
		}
		return bounds;
	}

	AABB SkinBounds(const MeshSource& source, const Submesh& submesh, const std::vector<glm::mat4>& palette)
	{
		AABB bounds;
		for (size_t i = submesh.BaseVertex; i < static_cast<size_t>(submesh.BaseVertex) + submesh.VertexCount; i++)
		{
			const auto& vertex = source.Vertices[i];
			glm::vec4 position(0);
			for (int j = 0; j < 4; j++)
				position += vertex.Weights[j] * (palette[vertex.Joints[j]] * glm::vec4(vertex.Position, 1));
			bounds.Expand(glm::vec3(position));
		}
		return bounds;
	}
}
