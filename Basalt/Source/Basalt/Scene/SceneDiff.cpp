#include "Basalt/Scene/SceneDiff.h"

#include "Basalt/Core/Assert.h"

#include <spdlog/fmt/fmt.h>

#include <cmath>
#include <set>
#include <unordered_map>

namespace Basalt {

	namespace {

		using json = nlohmann::json;

		std::string Join(const std::string& path, const std::string& key)
		{
			return path.empty() ? key : path + "." + key;
		}

		// Records every difference between a and b under path: objects recurse key by key (a missing key is null),
		// anything else is one change.
		void DiffValues(const json& a, const json& b, const std::string& path, double tolerance, json& outChanges)
		{
			if (a.is_object() && b.is_object())
			{
				std::set<std::string> keys;
				for (const auto& [key, value] : a.items())
					keys.insert(key);
				for (const auto& [key, value] : b.items())
					keys.insert(key);
				for (const std::string& key : keys)
				{
					const auto inA = a.find(key);
					const auto inB = b.find(key);
					DiffValues(inA == a.end() ? json() : *inA, inB == b.end() ? json() : *inB, Join(path, key), tolerance, outChanges);
				}
				return;
			}
			if (!JsonNearlyEqual(a, b, tolerance))
				outChanges.push_back({ { "path", path }, { "from", a }, { "to", b } });
		}

		struct IndexedEntity
		{
			uint64_t ID = 0;
			const json* Data = nullptr;
		};

		bool IndexEntities(const json& scene, const char* side, std::vector<IndexedEntity>& outOrder, std::unordered_map<uint64_t, const json*>& outById, std::string& outError)
		{
			if (!scene.is_object())
			{
				outError = fmt::format("the {} scene is not a JSON object", side);
				return false;
			}
			const auto entities = scene.find("Entities");
			if (entities == scene.end() || entities->is_null())
				return true;
			if (!entities->is_array())
			{
				outError = fmt::format("the {} scene's Entities is not an array", side);
				return false;
			}
			for (size_t i = 0; i < entities->size(); i++)
			{
				const json& entity = (*entities)[i];
				const auto id = entity.is_object() ? entity.find("ID") : entity.end();
				const bool numeric = id != entity.end() && (id->is_number_unsigned() || (id->is_number_integer() && id->get<int64_t>() >= 0));
				if (!numeric)
				{
					outError = fmt::format("entity #{} of the {} scene has no numeric ID", i, side);
					return false;
				}
				const uint64_t value = id->get<uint64_t>();
				if (!outById.emplace(value, &entity).second)
				{
					outError = fmt::format("the {} scene has two entities with ID {}", side, value);
					return false;
				}
				outOrder.push_back({ value, &entity });
			}
			return true;
		}

		json EntityName(const json& entity)
		{
			const auto name = entity.find("Name");
			return name != entity.end() && name->is_string() ? *name : json("");
		}

		json EntityRef(const IndexedEntity& entity)
		{
			return { { "id", entity.ID }, { "name", EntityName(*entity.Data) } };
		}

		// The entity's own fields compared as one object, with each component's fields directly under its name
		// (component names and entity keys such as Name and Parent never coincide).
		json Flatten(const json& entity)
		{
			json flat = json::object();
			for (const auto& [key, value] : entity.items())
			{
				if (key == "ID")
					continue;
				if (key == "Components" && value.is_object())
				{
					for (const auto& [component, data] : value.items())
						flat[component] = data;
					continue;
				}
				flat[key] = value;
			}
			return flat;
		}

		std::string Brief(const json& value)
		{
			static constexpr size_t MaxLength = 80;
			std::string text = value.dump(-1, ' ', false, json::error_handler_t::replace);
			if (text.size() <= MaxLength)
				return text;
			// Cut before a UTF-8 continuation byte would split a character.
			size_t cut = MaxLength - 3;
			while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
				cut--;
			return text.substr(0, cut) + "...";
		}

		std::string Mismatch(const std::string& path, const json& actual, const json& expected)
		{
			return fmt::format("{} is {}, expected {}", path, Brief(actual), Brief(expected));
		}

		// The IDs of `order` the other side has too, in order.
		std::vector<uint64_t> SharedIds(const std::vector<IndexedEntity>& order, const std::unordered_map<uint64_t, const json*>& other)
		{
			std::vector<uint64_t> ids;
			for (const IndexedEntity& entity : order)
			{
				if (other.contains(entity.ID))
					ids.push_back(entity.ID);
			}
			return ids;
		}

		// Accessors that accept any JSON: DescribeSceneDiff's input need not be a DiffScenes result.
		const json& ArrayAt(const json& object, const char* key)
		{
			static const json s_Empty = json::array();
			const auto it = object.is_object() ? object.find(key) : object.end();
			return it != object.end() && it->is_array() ? *it : s_Empty;
		}

		json ValueAt(const json& object, const char* key)
		{
			return object.is_object() ? object.value(key, json()) : json();
		}

		std::string StringAt(const json& object, const char* key)
		{
			const json value = ValueAt(object, key);
			return value.is_string() ? value.get<std::string>() : std::string();
		}

	}

	bool JsonNearlyEqual(const json& a, const json& b, double tolerance)
	{
		BS_CORE_ASSERT(tolerance >= 0.0, "tolerance must not be negative");
		if (a.is_number() && b.is_number())
		{
			// Integers compare exactly: IDs are 64-bit and lose precision as doubles.
			if (!a.is_number_float() && !b.is_number_float())
				return a == b;
			return std::abs(a.get<double>() - b.get<double>()) <= tolerance;
		}
		if (a.is_array() && b.is_array())
		{
			if (a.size() != b.size())
				return false;
			for (size_t i = 0; i < a.size(); i++)
			{
				if (!JsonNearlyEqual(a[i], b[i], tolerance))
					return false;
			}
			return true;
		}
		if (a.is_object() && b.is_object())
		{
			if (a.size() != b.size())
				return false;
			for (const auto& [key, value] : a.items())
			{
				const auto other = b.find(key);
				if (other == b.end() || !JsonNearlyEqual(value, *other, tolerance))
					return false;
			}
			return true;
		}
		return a == b;
	}

	void MatchJsonSubset(const json& actual, const json& expected, double tolerance, const std::string& path, std::vector<std::string>& outMismatches)
	{
		if (expected.is_object() && actual.is_object())
		{
			for (const auto& [key, value] : expected.items())
			{
				const auto it = actual.find(key);
				if (it == actual.end())
					outMismatches.push_back(Join(path, key) + " is missing");
				else
					MatchJsonSubset(*it, value, tolerance, Join(path, key), outMismatches);
			}
			return;
		}
		if (expected.is_array() && actual.is_array() && expected.size() == actual.size())
		{
			// Element paths would hide which vector differs: report the whole array instead.
			std::vector<std::string> elements;
			for (size_t i = 0; i < expected.size(); i++)
				MatchJsonSubset(actual[i], expected[i], tolerance, path, elements);
			if (!elements.empty())
				outMismatches.push_back(Mismatch(path, actual, expected));
			return;
		}
		if (!JsonNearlyEqual(actual, expected, tolerance))
			outMismatches.push_back(Mismatch(path, actual, expected));
	}

	std::optional<json> DiffScenes(const json& from, const json& to, const SceneDiffOptions& options, std::string& outError)
	{
		std::vector<IndexedEntity> fromOrder, toOrder;
		std::unordered_map<uint64_t, const json*> fromById, toById;
		if (!IndexEntities(from, "first", fromOrder, fromById, outError) || !IndexEntities(to, "second", toOrder, toById, outError))
			return std::nullopt;

		json settings = json::array();
		const auto sceneSettings = [](const json& scene) {
			json result = json::object();
			for (const auto& [key, value] : scene.items())
			{
				if (key != "Entities" && key != "Format" && key != "Version")
					result[key] = value;
			}
			return result;
		};
		DiffValues(sceneSettings(from), sceneSettings(to), "", options.Tolerance, settings);

		json added = json::array();
		json removed = json::array();
		json changed = json::array();
		for (const IndexedEntity& entity : fromOrder)
		{
			if (!toById.contains(entity.ID))
				removed.push_back(EntityRef(entity));
		}
		for (const IndexedEntity& entity : toOrder)
		{
			const auto before = fromById.find(entity.ID);
			if (before == fromById.end())
			{
				added.push_back(EntityRef(entity));
				continue;
			}
			json changes = json::array();
			DiffValues(Flatten(*before->second), Flatten(*entity.Data), "", options.Tolerance, changes);
			if (!changes.empty())
			{
				json entry = EntityRef(entity);
				entry["changes"] = std::move(changes);
				changed.push_back(std::move(entry));
			}
		}
		const bool reordered = SharedIds(fromOrder, toById) != SharedIds(toOrder, fromById);
		const bool equal = settings.empty() && added.empty() && removed.empty() && changed.empty() && !reordered;
		return json{ { "equal", equal }, { "settings", std::move(settings) }, { "added", std::move(added) }, { "removed", std::move(removed) }, { "changed", std::move(changed) }, { "reordered", reordered } };
	}

	std::string DescribeSceneDiff(const json& diff, size_t maxLines)
	{
		std::vector<std::string> lines;
		for (const json& change : ArrayAt(diff, "settings"))
			lines.push_back(fmt::format("scene {}: {} -> {}", StringAt(change, "path"), Brief(ValueAt(change, "from")), Brief(ValueAt(change, "to"))));
		for (const json& entity : ArrayAt(diff, "removed"))
			lines.push_back(fmt::format("removed '{}' ({})", StringAt(entity, "name"), Brief(ValueAt(entity, "id"))));
		for (const json& entity : ArrayAt(diff, "added"))
			lines.push_back(fmt::format("added '{}' ({})", StringAt(entity, "name"), Brief(ValueAt(entity, "id"))));
		for (const json& entity : ArrayAt(diff, "changed"))
		{
			for (const json& change : ArrayAt(entity, "changes"))
				lines.push_back(fmt::format("'{}' {}: {} -> {}", StringAt(entity, "name"), StringAt(change, "path"), Brief(ValueAt(change, "from")), Brief(ValueAt(change, "to"))));
		}
		if (ValueAt(diff, "reordered") == true)
			lines.emplace_back("the entities are in a different order");
		std::string result;
		for (size_t i = 0; i < lines.size() && i < maxLines; i++)
			result += (i ? "\n" : "") + lines[i];
		if (lines.size() > maxLines)
			result += fmt::format("\n({} more)", lines.size() - maxLines);
		return result;
	}

}
