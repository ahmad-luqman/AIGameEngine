#include "Basalt/Animation/AnimationJointMap.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <unordered_set>

namespace Basalt {

	std::optional<AnimationJointMap> AnimationJointMap::FromJson(const nlohmann::json& data, std::string& outError)
	{
		const nlohmann::json* joints = ReadEntryArray(data, "Joints", "joint map", "a joint map must be an object", AnimationClipLibrary::MaxJoints, outError);
		if (!joints)
			return std::nullopt;
		AnimationJointMap map;
		std::unordered_set<std::string> sources, targets;
		for (size_t i = 0; i < joints->size(); i++)
		{
			const nlohmann::json& entry = (*joints)[i];
			const std::string location = fmt::format("Joints[{}]", i);
			if (!RequireEntryObject(entry, { "Source", "Target", "Offset" }, location, outError))
				return std::nullopt;
			const std::string* source = ReadNonemptyString(entry, "Source", location, outError);
			if (!source)
				return std::nullopt;
			const std::string* target = ReadNonemptyString(entry, "Target", location, outError);
			if (!target)
				return std::nullopt;
			if (!sources.insert(*source).second)
			{
				outError = fmt::format("{}.Source '{}' is listed twice", location, *source);
				return std::nullopt;
			}
			if (!targets.insert(*target).second)
			{
				outError = fmt::format("{}.Target '{}' is listed twice", location, *target);
				return std::nullopt;
			}
			glm::vec3 offset(0);
			if (const auto value = entry.find("Offset"); value != entry.end())
			{
				if (!value->is_array() || value->size() != 3 || !JsonToFloat((*value)[0], offset.x) || !JsonToFloat((*value)[1], offset.y) || !JsonToFloat((*value)[2], offset.z))
				{
					outError = location + ".Offset must be three numbers";
					return std::nullopt;
				}
			}
			map.Joints.push_back({ .Source = *source, .Target = *target, .Offset = offset });
		}
		return map;
	}

	std::optional<AnimationJointMap> AnimationJointMap::FromText(std::string_view text, std::string& outError)
	{
		const nlohmann::json data = ParseJson(text, outError);
		return data.is_discarded() ? std::nullopt : FromJson(data, outError);
	}

	nlohmann::json AnimationJointMap::ToJson() const
	{
		nlohmann::json joints = nlohmann::json::array();
		for (const AnimationRetargetJoint& joint : Joints)
		{
			nlohmann::json entry = { { "Source", joint.Source }, { "Target", joint.Target } };
			if (joint.Offset != glm::vec3(0))
				entry["Offset"] = { joint.Offset.x, joint.Offset.y, joint.Offset.z };
			joints.push_back(std::move(entry));
		}
		return { { "Joints", std::move(joints) } };
	}

	bool AnimationJointMap::IsJointMapFile(const std::string& path)
	{
		return FileSystem::GetLowercaseExtension(FileSystem::PathFromUtf8(path)) == Extension;
	}

	std::vector<AnimationRetargetJoint> CombineJointPairs(const AnimationClipLibrary& library, const AnimationJointMap* map)
	{
		std::vector<AnimationRetargetJoint> pairs = library.Joints;
		if (!map)
			return pairs;
		// Only complete entries override: a half-named one (an inspector addition being filled in) is skipped by BuildMap,
		// so letting it hide the map's pair would leave that joint paired by neither.
		const auto named = [&](const std::string& joint, std::string AnimationRetargetJoint::* side) {
			return std::ranges::any_of(library.Joints, [&](const AnimationRetargetJoint& entry) { return !entry.Source.empty() && !entry.Target.empty() && entry.*side == joint; });
		};
		for (const AnimationRetargetJoint& pair : map->Joints)
			if (!named(pair.Source, &AnimationRetargetJoint::Source) && !named(pair.Target, &AnimationRetargetJoint::Target))
				pairs.push_back(pair);
		return pairs;
	}

}
