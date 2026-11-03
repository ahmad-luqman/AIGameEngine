#pragma once

#include "Basalt/Scene/Components.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	// A layer mask shared between Animators (a .banimmask file named by AnimationLayer::MaskAsset): the same entries as
	// an inline AnimationLayer::Mask, so each joint and its subtree take its Weight unless a deeper entry names one of
	// its descendants.
	struct AnimationMask
	{
		static constexpr const char* Extension = ".banimmask";

		std::vector<AnimationMaskJoint> Joints;

		// Strict, bounded parser of {"Joints": [{"Joint": "Spine", "Weight": 1}, ...]}: at most
		// AnimationLayer::MaxMaskJoints entries with unique, nonempty joint names and weights in 0..1 (Weight defaults to 1).
		static std::optional<AnimationMask> FromJson(const nlohmann::json& data, std::string& outError);
		nlohmann::json ToJson() const;
		// Whether a project-relative path names a mask file (by its extension, in any letter case).
		static bool IsMaskFile(const std::string& path);
	};

}
