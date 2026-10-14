#pragma once

#include "Basalt/Asset/MeshSource.h"

namespace Basalt {

	class Entity;
	// World bounds for editor picking/focus/selection, using the running pose or edit-mode rest pose.
	AABB AnimatedModelBounds(Entity entity);

	// Local TRS pose, blended before composing the hierarchy to preserve joint lengths.
	struct AnimationLocalPose
	{
		std::vector<glm::vec3> Translations;
		std::vector<glm::quat> Rotations;
		std::vector<glm::vec3> Scales;
	};
	AnimationLocalPose SampleAnimation(const MeshSource& source, const AnimationClip* clip, float time);
	void BlendAnimationPose(AnimationLocalPose& target, const AnimationLocalPose& from, float weight);
	bool ComposeAnimationPose(const MeshSource& source, const AnimationLocalPose& pose, std::vector<glm::mat4>& globals);

	// Samples a clip into model-space node transforms. A null clip evaluates the authored rest pose.
	bool EvaluateAnimation(const MeshSource& source, const AnimationClip* clip, float time, std::vector<glm::mat4>& globals);
	// Palette maps bind vertices directly into model space (the skinned mesh node transform is ignored).
	std::vector<glm::mat4> BuildSkinPalette(const MeshSkin& skin, const std::vector<glm::mat4>& globals);
	// Exact posed bounds used for camera and shadow culling; never uses stale bind-pose bounds.
	AABB SkinBounds(const MeshSource& source, const Submesh& submesh, const std::vector<glm::mat4>& palette);

}
