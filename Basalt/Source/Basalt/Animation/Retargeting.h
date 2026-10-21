#pragma once

#include "Basalt/Asset/MeshSource.h"

#include <string>
#include <string_view>
#include <vector>

namespace Basalt {

	struct AnimationRetargetJoint;

	// How the nodes of a clip library's skeleton (the source) drive an Animator's model (the target).
	struct RetargetMap
	{
		// Per target node: the source node driving it, or -1.
		std::vector<int32_t> SourceOf;
		// The pair that carries translation (the humanoid hips, else the mapped target node nearest the top).
		int32_t TargetRoot = -1;
		int32_t SourceRoot = -1;
		// Target over source leg length (the root's rest height above the lowest jointly mapped joint), which scales the
		// root's displacement; 1 when either skeleton has no such height.
		float RootScale = 1.0f;
		// Per target node: how it was matched ("joint map", "name" or "humanoid"), for diagnostics.
		std::vector<const char*> Reasons;

		size_t MappedCount() const;
	};

	namespace Retargeting {
		// A joint name without "namespace:" and "Armature|" prefixes (Mixamo's "mixamorig:Hips" is "Hips").
		std::string_view StripNamespace(std::string_view joint);
		// The canonical humanoid bone a joint name stands for ("Hips", "Spine", "Chest", "UpperChest", "Neck", "Head",
		// then Left/Right + "Shoulder", "UpperArm", "LowerArm", "Hand", "UpperLeg", "LowerLeg", "Foot", "Toes"), across
		// Mixamo, Unreal, Unity, Blender (Rigify), VRM and 3ds Max biped naming; empty when it is none of them.
		std::string HumanoidBone(std::string_view joint);

		// Matches the skeletons: the explicit pairs first, then equal names after StripNamespace, then equal humanoid
		// bones. Each source and target node is used once. Pairs naming a missing joint are reported in outWarnings.
		RetargetMap BuildMap(const MeshSource& target, const MeshSource& source, const std::vector<AnimationRetargetJoint>& joints, std::vector<std::string>& outWarnings);

		// Bakes a source clip onto the target's nodes. Each mapped joint turns in model space as its source turns from
		// the source's rest pose (so differing joint axes and rest orientations cancel); the root also moves by the
		// source root's displacement times RootScale, and every other joint keeps the target's rest translation (its limb
		// length). Scale tracks carry the ratio to the source's rest scale. Weights tracks move to the target nodes of the
		// same name, matched by morph target name. Tracks are sampled at the source's key times (plus 30 Hz for cubic
		// clips) and interpolate linearly (stepped when every source track steps). False with outError when the result
		// exceeds the decoded animation budget.
		bool BakeClip(const MeshSource& target, const MeshSource& source, const RetargetMap& map, const AnimationClip& clip, AnimationClip& outClip, std::string& outError);
	}

}
