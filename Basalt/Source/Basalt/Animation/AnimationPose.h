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
	// The first node named `name`, or -1.
	int32_t FindNode(const MeshSource& source, const std::string& name);
	// Whether `ancestor` is a strict ancestor of `node`.
	bool IsAncestor(const MeshSource& source, uint32_t ancestor, uint32_t node);

	// Bits of the TRS channels a clip animates per node, so layers leave the other channels to the pose below.
	namespace AnimatedChannel {
		inline constexpr uint8_t Translation = 1, Rotation = 2, Scale = 4;
	}
	std::vector<uint8_t> AnimatedChannels(const MeshSource& source, const AnimationClip& clip);
	// Per-node weights of a layer mask: a mask without named entries gives every node 1; otherwise each node takes the
	// Weight (clamped to 0..1, non-finite: 0) of the nearest entry naming it or an ancestor (the first node of each
	// name), and 0 under no entry. Entries with an empty Joint are ignored; entries naming no node are appended to
	// outMissing.
	std::vector<float> AnimationMaskWeights(const MeshSource& source, const std::vector<AnimationMaskJoint>& mask, std::vector<std::string>& outMissing);
	// Which nodes a layer moves and how much (before the layer's weight), one entry per node.
	struct AnimationLayerMask
	{
		// 0..1, from AnimationMaskWeights.
		std::vector<float> Weights;
		// AnimatedChannel bits of the layer clip's tracks, from AnimatedChannels.
		std::vector<uint8_t> Channels;
	};
	// Moves the masked, animated channels of `target` toward `layer` by weight * mask weight.
	void OverrideAnimationPose(AnimationLocalPose& target, const AnimationLocalPose& layer, const AnimationLayerMask& mask, float weight);
	// Adds `layer`'s difference from `reference` to the masked, animated channels by weight * mask weight, in each
	// node's parent space: translation offsets, scale ratios, and rotation deltas (layer * inverse(reference))
	// multiplied on the left of the target's rotation, i.e. turned in the parent's frame.
	void AddAnimationPose(AnimationLocalPose& target, const AnimationLocalPose& layer, const AnimationLocalPose& reference, const AnimationLayerMask& mask, float weight);

	// Samples a clip into model-space node transforms. A null clip evaluates the authored rest pose.
	bool EvaluateAnimation(const MeshSource& source, const AnimationClip* clip, float time, std::vector<glm::mat4>& globals);
	// Returns empty for a non-finite result. Palette maps bind vertices directly into model space (the skinned mesh node transform is ignored).
	std::vector<glm::mat4> BuildSkinPalette(const MeshSkin& skin, const std::vector<glm::mat4>& globals);
	// Whether every skin's palette is finite for these globals (the shader cannot draw anything else).
	bool SkinPalettesFinite(const MeshSource& source, const std::vector<glm::mat4>& globals);
	// Conservative posed bounds in O(influencing joints), using immutable import-time caches.
	AABB CachedSkinBounds(const MeshSource& source, const Submesh& submesh, const std::vector<glm::mat4>& palette);
	// Exact posed bounds for reference geometry and verification.
	AABB SkinBounds(const MeshSource& source, const Submesh& submesh, const std::vector<glm::mat4>& palette);

}
