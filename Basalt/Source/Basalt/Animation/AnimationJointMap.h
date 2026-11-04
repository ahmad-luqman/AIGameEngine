#pragma once

#include "Basalt/Scene/Components.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Basalt {

	// Joint pairs shared between clip libraries (a .bjointmap file named by AnimationClipLibrary::JointMap): the same
	// entries as an inline AnimationClipLibrary::Joints, except that every entry names both joints (a file has no
	// inspector's unnamed additions).
	struct AnimationJointMap
	{
		static constexpr const char* Extension = ".bjointmap";

		std::vector<AnimationRetargetJoint> Joints;

		// Strict, bounded parser of {"Joints": [{"Source": "mixamorig:Hips", "Target": "pelvis"}, ...]}: at most
		// AnimationClipLibrary::MaxJoints entries with nonempty names, each source and each target listed once.
		static std::optional<AnimationJointMap> FromJson(const nlohmann::json& data, std::string& outError);
		// ParseJson, then FromJson.
		static std::optional<AnimationJointMap> FromText(std::string_view text, std::string& outError);
		nlohmann::json ToJson() const;
		// Whether a project-relative path names a joint map file (by its extension, in any letter case).
		static bool IsJointMapFile(const std::string& path);
	};

	// The joint pairs a clip library matches first: its inline Joints, then the pairs of its loaded JointMap (`map`, may be
	// null) whose source and target no inline entry names, so an inline entry overrides the shared map for its joints.
	std::vector<AnimationRetargetJoint> CombineJointPairs(const AnimationClipLibrary& library, const AnimationJointMap* map);

}
