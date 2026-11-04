#include "Basalt/Animation/AnimationJointMap.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <unordered_set>

namespace Basalt {

	std::optional<AnimationJointMap> AnimationJointMap::FromJson(const nlohmann::json& data, std::string& outError)
	{
		outError.clear();
		if (!data.is_object() || !RejectUnknownKeys(data, { "Joints" }, "joint map", outError))
		{
			if (outError.empty())
				outError = "a joint map must be an object";
			return std::nullopt;
		}
		// contains/at rather than find: GCC cannot see that a found iterator is not null.
		if (!data.contains("Joints") || !data.at("Joints").is_array() || data.at("Joints").size() > AnimationClipLibrary::MaxJoints)
		{
			outError = fmt::format("Joints must be an array with at most {} entries", AnimationClipLibrary::MaxJoints);
			return std::nullopt;
		}
		AnimationJointMap map;
		std::unordered_set<std::string> sources, targets;
		const nlohmann::json& joints = data.at("Joints");
		for (size_t i = 0; i < joints.size(); i++)
		{
			const nlohmann::json& entry = joints[i];
			const std::string location = fmt::format("Joints[{}]", i);
			if (!entry.is_object() || !RejectUnknownKeys(entry, { "Source", "Target" }, location, outError))
			{
				if (outError.empty())
					outError = location + " must be an object";
				return std::nullopt;
			}
			AnimationRetargetJoint parsed;
			for (const auto& [key, value] : { std::pair{ "Source", &parsed.Source }, std::pair{ "Target", &parsed.Target } })
			{
				if (!entry.contains(key) || !entry.at(key).is_string() || entry.at(key).get_ref<const std::string&>().empty())
				{
					outError = fmt::format("{}.{} must be a nonempty string", location, key);
					return std::nullopt;
				}
				*value = entry.at(key).get<std::string>();
			}
			if (!sources.insert(parsed.Source).second)
			{
				outError = fmt::format("{}.Source '{}' is listed twice", location, parsed.Source);
				return std::nullopt;
			}
			if (!targets.insert(parsed.Target).second)
			{
				outError = fmt::format("{}.Target '{}' is listed twice", location, parsed.Target);
				return std::nullopt;
			}
			map.Joints.push_back(std::move(parsed));
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
			joints.push_back({ { "Source", joint.Source }, { "Target", joint.Target } });
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
		const auto named = [&](const std::string& joint, std::string AnimationRetargetJoint::* side) {
			return std::ranges::any_of(library.Joints, [&](const AnimationRetargetJoint& entry) { return entry.*side == joint; });
		};
		for (const AnimationRetargetJoint& pair : map->Joints)
			if (!named(pair.Source, &AnimationRetargetJoint::Source) && !named(pair.Target, &AnimationRetargetJoint::Target))
				pairs.push_back(pair);
		return pairs;
	}

}
