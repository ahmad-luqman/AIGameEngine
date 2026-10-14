#pragma once

#include "Basalt/Asset/MeshSource.h"

namespace Basalt {

	// Local transform of one sampled node.
	struct AnimationNodePose
	{
		glm::vec3 Translation = glm::vec3(0);
		glm::quat Rotation = glm::quat(1, 0, 0, 0);
		glm::vec3 Scale = glm::vec3(1);
	};
	// Local TRS pose, blended before composing the hierarchy to preserve joint lengths.
	struct AnimationLocalPose
	{
		std::vector<AnimationNodePose> Nodes;
	};
	AnimationLocalPose SampleAnimation(const MeshSource& source, const AnimationClip* clip, float time);
	// Weight 0 selects from; weight 1 selects target. Both poses must cover the same nodes.
	void BlendAnimationPose(AnimationLocalPose& target, const AnimationLocalPose& from, float weight);
	bool ComposeAnimationPose(const MeshSource& source, const AnimationLocalPose& pose, std::vector<glm::mat4>& globals);

	// Samples a clip into model-space node transforms. A null clip evaluates the authored rest pose.
	bool EvaluateAnimation(const MeshSource& source, const AnimationClip* clip, float time, std::vector<glm::mat4>& globals);
	// Returns empty for a non-finite result. Palette maps bind vertices directly into model space (the skinned mesh node transform is ignored).
	std::vector<glm::mat4> BuildSkinPalette(const MeshSkin& skin, const std::vector<glm::mat4>& globals);
	// Exact posed bounds used for camera and shadow culling; never uses stale bind-pose bounds.
	AABB SkinBounds(const MeshSource& source, const Submesh& submesh, const std::vector<glm::mat4>& palette);

}
