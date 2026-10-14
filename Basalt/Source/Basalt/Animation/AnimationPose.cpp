#include "Basalt/Animation/AnimationPose.h"

#include "Basalt/Animation/AnimationSystem.h"
#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

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
				const float length = glm::length(value);
				value = std::isfinite(length) && length > 1e-8f ? value / length : a;
			}
			return value;
		}
	}

	AnimationLocalPose SampleAnimation(const MeshSource& source, const AnimationClip* clip, float time)
	{
		AnimationLocalPose pose;
		for (const auto& node : source.Nodes)
		{
			pose.Translations.push_back(node.Translation);
			pose.Rotations.push_back(node.Rotation);
			pose.Scales.push_back(node.Scale);
		}
		if (clip)
			for (const auto& track : clip->Tracks)
			{
				const glm::vec4 value = Sample(track, time);
				switch (track.Path)
				{
					case AnimationPath::Translation:
						pose.Translations[track.Node] = glm::vec3(value);
						break;
					case AnimationPath::Rotation:
						pose.Rotations[track.Node] = Quaternion(value);
						break;
					case AnimationPath::Scale:
						pose.Scales[track.Node] = glm::vec3(value);
						break;
				}
			}
		return pose;
	}

	void BlendAnimationPose(AnimationLocalPose& target, const AnimationLocalPose& from, float weight)
	{
		for (size_t i = 0; i < target.Translations.size(); i++)
		{
			target.Translations[i] = glm::mix(from.Translations[i], target.Translations[i], weight);
			target.Rotations[i] = glm::normalize(glm::slerp(from.Rotations[i], target.Rotations[i], weight));
			target.Scales[i] = glm::mix(from.Scales[i], target.Scales[i], weight);
		}
	}

	bool ComposeAnimationPose(const MeshSource& source, const AnimationLocalPose& pose, std::vector<glm::mat4>& globals)
	{
		globals.resize(source.Nodes.size());
		for (uint32_t i : source.NodeOrder)
		{
			const glm::mat4 local = source.Nodes[i].HasMatrix ? source.Nodes[i].Matrix : glm::translate(glm::mat4(1), pose.Translations[i]) * glm::mat4_cast(pose.Rotations[i]) * glm::scale(glm::mat4(1), pose.Scales[i]);
			globals[i] = source.Nodes[i].Parent >= 0 ? globals[source.Nodes[i].Parent] * local : local;
			for (int c = 0; c < 4; c++)
				for (int r = 0; r < 4; r++)
					if (!std::isfinite(globals[i][c][r]))
						return false;
		}
		return true;
	}

	bool EvaluateAnimation(const MeshSource& source, const AnimationClip* clip, float time, std::vector<glm::mat4>& globals)
	{
		return ComposeAnimationPose(source, SampleAnimation(source, clip, time), globals);
	}

	AABB AnimatedModelBounds(Entity entity)
	{
		AABB bounds;
		const auto* animator = entity.TryGetComponent<AnimatorComponent>();
		const auto source = animator ? AssetManager::GetMesh(animator->Mesh) : nullptr;
		if (!source)
			return bounds;
		auto& scene = *entity.GetScene();
		std::vector<glm::mat4> rest;
		const auto* globals = scene.GetAnimationSystem() ? scene.GetAnimationSystem()->GetPose(entity) : nullptr;
		if (!globals)
		{
			if (!EvaluateAnimation(*source, nullptr, 0, rest))
				return bounds;
			globals = &rest;
		}
		const glm::mat4 root = scene.GetWorldTransform(entity);
		for (size_t i = 0; i < source->Nodes.size(); i++)
		{
			const auto& node = source->Nodes[i];
			if (!node.Active || node.MeshIndex < 0)
				continue;
			const auto& mesh = source->Meshes[node.MeshIndex];
			if (node.SkinIndex < 0)
				bounds.Expand(mesh.Bounds.Transformed(root * (*globals)[i]));
			else
			{
				const auto palette = BuildSkinPalette(source->Skins[node.SkinIndex], *globals);
				if (palette.empty())
					continue;
				for (uint32_t submesh : mesh.Submeshes)
					bounds.Expand(SkinBounds(*source, source->Submeshes[submesh], palette).Transformed(root));
			}
		}
		return bounds;
	}

	std::vector<glm::mat4> BuildSkinPalette(const MeshSkin& skin, const std::vector<glm::mat4>& globals)
	{
		std::vector<glm::mat4> result;
		result.reserve(skin.Joints.size());
		for (size_t i = 0; i < skin.Joints.size(); i++)
		{
			const glm::mat4 matrix = globals[skin.Joints[i]] * skin.InverseBindMatrices[i];
			for (int c = 0; c < 4; c++)
				for (int r = 0; r < 4; r++)
					if (!std::isfinite(matrix[c][r]))
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
