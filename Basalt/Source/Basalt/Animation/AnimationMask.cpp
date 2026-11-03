#include "Basalt/Animation/AnimationMask.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"

#include <spdlog/fmt/fmt.h>

#include <unordered_set>

namespace Basalt {

	std::optional<AnimationMask> AnimationMask::FromJson(const nlohmann::json& data, std::string& outError)
	{
		outError.clear();
		if (!data.is_object() || !RejectUnknownKeys(data, { "Joints" }, "animation mask", outError))
		{
			if (outError.empty())
				outError = "an animation mask must be an object";
			return std::nullopt;
		}
		// contains/at rather than find: GCC cannot see that a found iterator is not null.
		if (!data.contains("Joints") || !data.at("Joints").is_array() || data.at("Joints").size() > AnimationLayer::MaxMaskJoints)
		{
			outError = fmt::format("Joints must be an array with at most {} entries", AnimationLayer::MaxMaskJoints);
			return std::nullopt;
		}
		AnimationMask mask;
		std::unordered_set<std::string> names;
		const nlohmann::json& joints = data.at("Joints");
		for (size_t i = 0; i < joints.size(); i++)
		{
			const nlohmann::json& entry = joints[i];
			const std::string location = fmt::format("Joints[{}]", i);
			if (!entry.is_object() || !RejectUnknownKeys(entry, { "Joint", "Weight" }, location, outError))
			{
				if (outError.empty())
					outError = location + " must be an object";
				return std::nullopt;
			}
			if (!entry.contains("Joint") || !entry.at("Joint").is_string() || entry.at("Joint").get_ref<const std::string&>().empty())
			{
				outError = location + ".Joint must be a nonempty string";
				return std::nullopt;
			}
			AnimationMaskJoint parsed{ .Joint = entry.at("Joint").get<std::string>() };
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

	nlohmann::json AnimationMask::ToJson() const
	{
		nlohmann::json joints = nlohmann::json::array();
		for (const AnimationMaskJoint& joint : Joints)
			joints.push_back({ { "Joint", joint.Joint }, { "Weight", joint.Weight } });
		return { { "Joints", std::move(joints) } };
	}

	bool AnimationMask::IsMaskFile(const std::string& path)
	{
		return FileSystem::GetLowercaseExtension(FileSystem::PathFromUtf8(path)) == Extension;
	}

}
