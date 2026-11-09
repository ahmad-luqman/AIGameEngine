#include "Breccia/Animation/AnimationMask.h"

#include "Breccia/Core/FileSystem.h"
#include "Breccia/Core/JsonUtils.h"

#include <spdlog/fmt/fmt.h>

#include <unordered_set>

namespace Breccia {

	std::optional<AnimationMask> AnimationMask::FromJson(const nlohmann::json& data, std::string& outError)
	{
		const nlohmann::json* joints = ReadEntryArray(data, "Joints", "animation mask", "an animation mask must be an object", AnimationLayer::MaxMaskJoints, outError);
		if (!joints)
			return std::nullopt;
		AnimationMask mask;
		std::unordered_set<std::string> names;
		for (size_t i = 0; i < joints->size(); i++)
		{
			const nlohmann::json& entry = (*joints)[i];
			const std::string location = fmt::format("Joints[{}]", i);
			if (!RequireEntryObject(entry, { "Joint", "Weight" }, location, outError))
				return std::nullopt;
			const std::string* joint = ReadNonemptyString(entry, "Joint", location, outError);
			if (!joint)
				return std::nullopt;
			AnimationMaskJoint parsed{ .Joint = *joint };
			if (!names.insert(parsed.Joint).second)
			{
				outError = fmt::format("{}.Joint '{}' is listed twice", location, parsed.Joint);
				return std::nullopt;
			}
			if (entry.contains("Weight") && (!JsonToFloat(entry.at("Weight"), parsed.Weight) || parsed.Weight < 0.0f || parsed.Weight > 1.0f))
			{
				outError = location + ".Weight must be a number between 0 and 1";
				return std::nullopt;
			}
			mask.Joints.push_back(std::move(parsed));
		}
		return mask;
	}

	std::optional<AnimationMask> AnimationMask::FromText(std::string_view text, std::string& outError)
	{
		const nlohmann::json data = ParseJson(text, outError);
		return data.is_discarded() ? std::nullopt : FromJson(data, outError);
	}

	nlohmann::json AnimationMask::ToJson() const
	{
		nlohmann::json joints = nlohmann::json::array();
		for (const AnimationMaskJoint& joint : Joints)
			joints.push_back({ { "Joint", joint.Joint }, { "Weight", joint.Weight } });
		return { { "Joints", std::move(joints) } };
	}

	const std::vector<AnimationMaskJoint>* SelectLayerMask(const AnimationLayer& layer, const AnimationMask* asset)
	{
		if (layer.MaskAsset.empty())
			return &layer.Mask;
		return asset ? &asset->Joints : nullptr;
	}

	bool AnimationMask::IsMaskFile(const std::string& path)
	{
		return FileSystem::GetLowercaseExtension(FileSystem::PathFromUtf8(path)) == Extension;
	}

}
