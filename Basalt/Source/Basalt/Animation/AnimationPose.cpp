#include "Basalt/Animation/AnimationPose.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Math/Math.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

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
