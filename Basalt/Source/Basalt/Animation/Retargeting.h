#pragma once

#include "Basalt/Asset/MeshSource.h"

#include <string>
#include <string_view>
#include <vector>

namespace Basalt {

	struct AnimationRetargetJoint;

	// How a target node found its source joint.
	enum class RetargetMatch : uint8_t
	{
		None = 0,
		JointMap, // An AnimationClipLibrary::Joints entry.
		Name,     // Equal names after Retargeting::StripNamespace.
		Humanoid  // The same Retargeting::HumanoidBone.
	};
	// "joint map", "name", "humanoid" (or "none"), for diagnostics.
	const char* ToString(RetargetMatch match);

	// Model-space rest positions and rotations per node of one skeleton.
	struct RestPose
	{
		std::vector<glm::vec3> Positions;
		std::vector<glm::quat> Rotations;
	};

	// How the nodes of a clip library's skeleton (the source) drive an Animator's model (the target), with both rest
	// poses. Only meaningful for the pair it was built from (BakeClip checks the sizes).
	struct RetargetMap
	{
		// Per target node: the source node driving it, or -1, and how it was matched.
		std::vector<int32_t> SourceOf;
		std::vector<RetargetMatch> Matches;
		// The pair that carries translation: the first mapped target node named as the humanoid hips (whatever drives
		// it), else the mapped target node nearest the top.
		int32_t TargetRoot = -1;
		int32_t SourceRoot = -1;
		// Target over source leg length (the root's rest height above the lowest mapped joint), which scales the root's
		// displacement; 1 when either skeleton has no such height (BuildMap warns when the root is the humanoid hips).
		float RootScale = 1.0f;
		// Each skeleton's rest pose; both empty when either is not finite.
		RestPose TargetRest, SourceRest;

		size_t MappedCount() const;
	};

	namespace Retargeting {
		// Decoded floats one baked clip may hold (16M, the importer's budget for a whole model).
		inline constexpr size_t MaxClipBakedFloats = size_t{ 16 } * 1024 * 1024;

		// A joint name without "namespace:" and "Armature|" prefixes (Mixamo's "mixamorig:Hips" is "Hips").
		std::string_view StripNamespace(std::string_view joint);
		// The canonical humanoid bone a joint name stands for ("Hips", "Spine", "Chest", "UpperChest", "Neck", "Head",
		// then Left/Right + "Shoulder", "UpperArm", "LowerArm", "Hand", "UpperLeg", "LowerLeg", "Foot", "Toes"), across
		// Mixamo, Unreal, Unity, Blender (Rigify deform bones, numbered spine included), VRM and 3ds Max biped naming;
		// empty when it is none of them.
		std::string HumanoidBone(std::string_view joint);

		// Matches the skeletons over all their nodes: the explicit pairs first, then equal names after StripNamespace,
		// then equal humanoid bones; each source and target node is used once. outWarnings gets explicit pairs that name
		// a missing joint, are half filled or reuse a paired joint, paired joints given as matrices, rest poses that are
		// not finite or have joints whose rest transform is not affine, and humanoid hips whose leg length cannot be measured.
		RetargetMap BuildMap(const MeshSource& target, const MeshSource& source, const std::vector<AnimationRetargetJoint>& joints, std::vector<std::string>& outWarnings);

		// Whether baking `clip` fits MaxClipBakedFloats; the reason otherwise. Cheap (no allocation) for clips well inside
		// it.
		bool CheckBakeBudget(const MeshSource& target, const MeshSource& source, const RetargetMap& map, const AnimationClip& clip, std::string& outError);

		// Bakes a source clip onto the target's nodes. Each mapped joint turns in model space as its source turns from
		// the source's rest pose (so differing joint axes and rest orientations cancel); the root also moves by the
		// source root's displacement times RootScale, and every other joint keeps the target's rest translation (its limb
		// length). Scale tracks carry the ratio to the source's rest scale. Joint tracks are sampled at the source's key
		// times (plus a 30 Hz grid when any track is cubic) and interpolate linearly (stepped when every source track
		// steps). Weights tracks keep their keys and interpolation and move to the target nodes of the same name
		// (ignoring namespaces), matched by morph target name; ones that match nothing, or only nodes an earlier track
		// drives, are named in outWarnings. False with outError for a map built for other skeletons, a track that does not
		// fit the source's nodes, a non-finite rest or sampled source pose, or a result past the budget.
		bool BakeClip(const MeshSource& target, const MeshSource& source, const RetargetMap& map, const AnimationClip& clip, AnimationClip& outClip, std::vector<std::string>& outWarnings, std::string& outError);
	}

}
