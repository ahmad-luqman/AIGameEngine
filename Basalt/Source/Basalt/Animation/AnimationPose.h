#pragma once

#include "Basalt/Asset/MeshSource.h"

namespace Basalt {

	struct AnimationMaskJoint;

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

	// Bits of the TRS channels a clip animates per node, so layers leave the other channels to the pose below.
	inline constexpr uint8_t AnimatedTranslation = 1, AnimatedRotation = 2, AnimatedScale = 4;
	std::vector<uint8_t> AnimatedChannels(const MeshSource& source, const AnimationClip& clip);
	// Per-node weights of a layer mask: empty gives every node 1; otherwise each node takes the Weight of the nearest
	// entry naming it or an ancestor (the first node of each name), and 0 under no entry. Entries with an empty Joint
	// are ignored; entries naming no node are appended to outMissing.
	std::vector<float> AnimationMaskWeights(const MeshSource& source, const std::vector<AnimationMaskJoint>& mask, std::vector<std::string>& outMissing);
	// Moves the animated channels of `target` toward `layer` by weights[node].
	void OverrideAnimationPose(AnimationLocalPose& target, const AnimationLocalPose& layer, const std::vector<float>& weights, const std::vector<uint8_t>& channels);
	// Adds `layer`'s difference from `reference` on the animated channels by weights[node], in each node's parent
	// space: translation offsets, rotation deltas applied before the target's rotation, and scale ratios.
	void AddAnimationPose(AnimationLocalPose& target, const AnimationLocalPose& layer, const AnimationLocalPose& reference, const std::vector<float>& weights, const std::vector<uint8_t>& channels);

	// Samples a clip into model-space node transforms. A null clip evaluates the authored rest pose.
	bool EvaluateAnimation(const MeshSource& source, const AnimationClip* clip, float time, std::vector<glm::mat4>& globals);
	// Returns empty for a non-finite result. Palette maps bind vertices directly into model space (the skinned mesh node transform is ignored).
	std::vector<glm::mat4> BuildSkinPalette(const MeshSkin& skin, const std::vector<glm::mat4>& globals);
	// Exact posed bounds used for camera and shadow culling; never uses stale bind-pose bounds.
	AABB SkinBounds(const MeshSource& source, const Submesh& submesh, const std::vector<glm::mat4>& palette);

}
