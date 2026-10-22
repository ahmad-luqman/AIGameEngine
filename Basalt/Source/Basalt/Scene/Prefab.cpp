#include "Basalt/Scene/Prefab.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Scene/ComponentRegistry.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/Scene/SceneSerializer.h"

#include <algorithm>
#include <charconv>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace Basalt {

	namespace {
		using json = nlohmann::json;
		using IdMap = std::unordered_map<uint64_t, uint64_t>;

		// The root's position and rotation place the instance in its scene: they are never overrides, Apply never
		// writes them and Revert keeps them. Its scale is an ordinary field.
		constexpr const char* PlacementFields[] = { "Translation", "Rotation" };

		uint64_t IdOf(Entity entity)
		{
			return static_cast<uint64_t>(entity.GetUUID());
		}

		std::string KeyOf(uint64_t id)
		{
			return std::to_string(id);
		}

		uint64_t ParseKey(const std::string& key)
		{
			uint64_t id = 0;
			const auto result = std::from_chars(key.data(), key.data() + key.size(), id);
			return result.ec == std::errc() && result.ptr == key.data() + key.size() ? id : 0;
		}

		bool IsId(const json& value)
		{
			return value.is_number_unsigned() || (value.is_number_integer() && value.get<int64_t>() >= 0);
		}

		std::string PointerToken(const std::string& key)
		{
			std::string token;
			for (char c : key)
			{
				if (c == '~')
					token += "~0";
				else if (c == '/')
					token += "~1";
				else
					token += c;
			}
			return token;
		}

		// Records keyed by source ID, plus the order the source lists them in (siblings keep it).
		struct Records
		{
			json ByKey = json::object();
			std::vector<std::string> Order;
		};

		Records ToRecords(const json& entities)
		{
			Records records;
			for (json entity : entities)
			{
				entity["Components"].erase("Prefab");
				const std::string key = KeyOf(entity["ID"].get<uint64_t>());
				records.Order.push_back(key);
				records.ByKey[key] = std::move(entity);
			}
			return records;
		}

		// The records in the first order that lists them; records neither lists come last, by key.
		std::vector<json> InOrder(const json& records, std::initializer_list<const std::vector<std::string>*> orders)
		{
			std::vector<json> ordered;
			std::unordered_set<std::string> placed;
			for (const auto* order : orders)
				for (const std::string& key : *order)
					if (records.contains(key) && placed.insert(key).second)
						ordered.push_back(records[key]);
			for (const auto& [key, record] : records.items())
				if (placed.insert(key).second)
					ordered.push_back(record);
			return ordered;
		}

		// Rewrites the record's ID, parent and declared entity fields through ids; other values are kept.
		void RemapReferences(json& record, const IdMap& ids)
		{
			auto remap = [&ids](json& value) {
				if (!IsId(value))
					return;
				if (auto it = ids.find(value.get<uint64_t>()); it != ids.end())
					value = it->second;
			};
			remap(record["ID"]);
			remap(record["Parent"]);
			json& components = record["Components"];
			for (const auto& info : ComponentRegistry::GetAll())
			{
				auto component = components.find(info.Name);
				if (component == components.end() || !component->is_object())
					continue;
				for (const auto& field : info.EntityFields)
					if (auto value = component->find(field); value != component->end())
						remap(*value);
			}
		}

		// A member as a source-space record: what a prefab file holds for it, as the serializer writes it.
		json SerializeMember(Entity entity, Entity root, const IdMap& toSource)
		{
			json components = json::object();
			for (const auto& info : ComponentRegistry::GetAll())
				if (info.Name != "Tag" && info.Name != "Prefab" && info.Has(entity))
					components[info.Name] = info.Serialize(entity);
			const Entity parent = entity.GetParent();
			json record = { { "ID", IdOf(entity) }, { "Name", entity.GetName() }, { "Parent", entity == root || !parent ? 0 : IdOf(parent) }, { "Components", std::move(components) } };
			RemapReferences(record, toSource);
			return record;
		}

		// Members never include another instance's root: a Members value that names one (a hand-edited scene) is
		// treated as an entity the instance deleted, so a merge can never overwrite or destroy another instance.
		bool IsOwnMember(Entity root, Entity entity)
		{
			return entity && (entity == root || !entity.HasComponent<PrefabComponent>());
		}

		std::string RootKey(const PrefabComponent& link, Entity root)
		{
			for (const auto& [key, value] : link.Members.items())
				if (IsId(value) && value.get<uint64_t>() == IdOf(root))
					return key;
			return {};
		}

		// The instance's members as records, depth first from the root. Members the instance deleted are absent.
		Records CaptureInstance(Entity root, const PrefabComponent& link)
		{
			IdMap toSource;
			for (const auto& [key, value] : link.Members.items())
				if (IsId(value))
					toSource[value.get<uint64_t>()] = ParseKey(key);
			Records records;
			std::function<void(Entity)> visit = [&](Entity entity) {
				const auto it = toSource.find(IdOf(entity));
				if (it != toSource.end() && IsOwnMember(root, entity) && !records.ByKey.contains(KeyOf(it->second)))
				{
					records.Order.push_back(KeyOf(it->second));
					records.ByKey[records.Order.back()] = SerializeMember(entity, root, toSource);
				}
				for (Entity child : entity.GetChildren())
					visit(child);
			};
			visit(root);
			// Members moved out of the instance's tree still belong to it.
			for (const auto& [instance, source] : toSource)
				if (!records.ByKey.contains(KeyOf(source)))
					if (Entity entity = root.GetScene()->GetEntityByUUID(UUID(instance)); IsOwnMember(root, entity))
						records.ByKey[KeyOf(source)] = SerializeMember(entity, root, toSource);
			return records;
		}

		// Three-way merge. Objects merge by key; any other value (numbers, strings, arrays such as vectors, lists and
		// script tables) is one authored value: changed in the instance, it wins over the source.
		json Merge(const json& base, const json& current, const json& source)
		{
			if (base == current)
				return source;
			if (!base.is_object() || !current.is_object() || !source.is_object())
				return current;
			json result = source;
			for (const auto& [key, old] : base.items())
			{
				const auto now = current.find(key);
				if (now == current.end())
					result.erase(key);
				else if (*now != old)
					result[key] = source.contains(key) ? Merge(old, *now, source[key]) : *now;
			}
			for (const auto& [key, value] : current.items())
				if (!base.contains(key))
					result[key] = value;
			return result;
		}

		// JSON-pointer paths where current differs from base. Removed objects (entities, components) report null.
		void CollectOverrides(const json& base, const json& current, const std::string& path, json& out)
		{
			if (base == current)
				return;
			if (!base.is_object() || !current.is_object())
			{
				out[path] = current;
				return;
			}
			for (const auto& [key, value] : base.items())
			{
				const std::string child = path + "/" + PointerToken(key);
				if (const auto now = current.find(key); now == current.end())
					out[child] = nullptr;
				else
					CollectOverrides(value, *now, child, out);
			}
			for (const auto& [key, value] : current.items())
				if (!base.contains(key))
					out[path + "/" + PointerToken(key)] = value;
		}

		json Overrides(const PrefabComponent& link, const json& current, const std::string& rootKey)
		{
			json overrides = json::object();
			if (link.Baseline.empty())
				return overrides;
			CollectOverrides(link.Baseline, current, "", overrides);
			for (const char* field : PlacementFields)
				overrides.erase("/" + rootKey + "/Components/Transform/" + field);
			return overrides;
		}

		// Copies the root's placement from one set of records into another.
		void KeepPlacement(json& records, const json& from, const std::string& rootKey)
		{
			if (!records.contains(rootKey) || !from.contains(rootKey))
				return;
			const json& source = from[rootKey]["Components"]["Transform"];
			json& target = records[rootKey]["Components"]["Transform"];
			for (const char* field : PlacementFields)
				if (source.contains(field))
					target[field] = source[field];
		}

		uint64_t NewInstanceId(Scene& scene, const std::unordered_set<uint64_t>& taken)
		{
			uint64_t id = 0;
			while (!id || taken.contains(id) || scene.GetEntityByUUID(UUID(id)))
				id = static_cast<uint64_t>(UUID());
			return id;
		}

		// Starts tracking an instance saved before overrides existed: the roots match, then children by name in
		// order. Everything that differs from the current source becomes an override, so adoption changes nothing;
		// source entities without a match count as deleted by the instance.
		void Adopt(Entity root, const json& sourceEntities, PrefabComponent& link)
		{
			std::unordered_map<uint64_t, std::vector<size_t>> children;
			for (size_t i = 1; i < sourceEntities.size(); ++i)
				children[sourceEntities[i]["Parent"].get<uint64_t>()].push_back(i);
			std::unordered_set<uint64_t> taken;
			std::function<void(uint64_t, Entity)> match = [&](uint64_t source, Entity instance) {
				link.Members[KeyOf(source)] = IdOf(instance);
				taken.insert(IdOf(instance));
				for (size_t index : children[source])
				{
					const json& child = sourceEntities[index];
					const auto name = child.find("Name");
					if (name == child.end() || !name->is_string())
						continue;
					for (Entity candidate : instance.GetChildren())
						if (!taken.contains(IdOf(candidate)) && candidate.GetName() == name->get_ref<const std::string&>() && !candidate.HasComponent<PrefabComponent>())
						{
							match(child["ID"].get<uint64_t>(), candidate);
							break;
						}
				}
			};
			match(sourceEntities[0]["ID"].get<uint64_t>(), root);
			for (const json& entity : sourceEntities)
				if (const std::string key = KeyOf(entity["ID"].get<uint64_t>()); !link.Members.contains(key))
				{
					const uint64_t id = NewInstanceId(*root.GetScene(), taken);
					taken.insert(id);
					link.Members[key] = id;
				}
			link.Baseline = ToRecords(sourceEntities).ByKey;
		}

		// Walks up the hierarchy the merged instance will have, from parent; false on a cycle through member or
		// a hierarchy deeper than the scene allows. depth receives the number of ancestors from parent up.
		bool WalkAncestors(Scene& scene, uint64_t member, uint64_t parent, const std::unordered_map<uint64_t, uint64_t>& parents, uint32_t& depth)
		{
			depth = 0;
			while (parent)
			{
				if (parent == member || ++depth > Scene::MaxHierarchyDepth)
					return false;
				if (auto it = parents.find(parent); it != parents.end())
					parent = it->second;
				else
				{
					const Entity entity = scene.GetEntityByUUID(UUID(parent));
					parent = entity && entity.GetParent() ? IdOf(entity.GetParent()) : 0;
				}
			}
			return true;
		}
	}

	bool Prefab::ReadFile(const std::filesystem::path& path, json& outEntities, std::string& outError)
	{
		const std::string name = FileSystem::PathToGenericUtf8(path);
		const auto text = FileSystem::ReadTextFile(path);
		if (!text)
		{
			outError = "cannot read prefab '" + name + "'";
			return false;
		}
		json data = ParseJson(*text);
		if (data.is_discarded() || !data.is_object() || !data.contains("Entities") || !data["Entities"].is_array() || data["Entities"].empty())
		{
			outError = "prefab '" + name + "' is not a valid prefab file";
			return false;
		}
		if (const auto format = data.find("Format"); format != data.end() && *format != "BasaltPrefab")
		{
			outError = "prefab '" + name + "' has 'Format' " + format->dump() + ", expected \"BasaltPrefab\"";
			return false;
		}
		json& entities = data["Entities"];
		std::unordered_set<uint64_t> ids;
		for (size_t i = 0; i < entities.size(); ++i)
		{
			json& entity = entities[i];
			const std::string context = "prefab '" + name + "' entity #" + std::to_string(i);
			if (!entity.is_object() || !entity.contains("ID") || !entity["ID"].is_number_unsigned() || entity["ID"] == 0)
			{
				outError = context + " needs a unique nonzero 'ID': instances track entities by it";
				return false;
			}
			if (!ids.insert(entity["ID"].get<uint64_t>()).second)
			{
				outError = context + " has duplicate ID " + entity["ID"].dump();
				return false;
			}
			const auto parent = entity.find("Parent");
			const bool isRoot = parent == entity.end() || *parent == 0;
			if (parent != entity.end() && !parent->is_number_unsigned())
			{
				outError = context + " has a 'Parent' that is not an entity ID";
				return false;
			}
			if (isRoot != (i == 0))
			{
				outError = "prefab '" + name + "' must have exactly one root entity, listed first";
				return false;
			}
			entity["Parent"] = isRoot ? json(0) : *parent;
			if (const auto components = entity.find("Components"); components != entity.end() && components->is_object())
				components->erase("Prefab");
		}
		outEntities = std::move(entities);
		return true;
	}

	bool Prefab::ReadSource(const std::filesystem::path& path, json& outEntities, std::string& outError)
	{
		json entities;
		if (!ReadFile(path, entities, outError))
			return false;
		// Deserializing keeps the source IDs, so writing the scratch entities back yields normalized source records.
		Scene scratch("Prefab source");
		if (!SceneSerializer::DeserializeScene(scratch, { { "Format", "BasaltScene" }, { "Version", SceneSerializer::FormatVersion }, { "Entities", entities } }, outError))
		{
			outError = "prefab '" + FileSystem::PathToGenericUtf8(path) + "': " + outError;
			return false;
		}
		outEntities = json::array();
		const Entity root = scratch.GetEntityByUUID(UUID(entities[0]["ID"].get<uint64_t>()));
		for (const json& entity : entities)
			outEntities.push_back(SerializeMember(scratch.GetEntityByUUID(UUID(entity["ID"].get<uint64_t>())), root, {}));
		return true;
	}

	void Prefab::Link(Entity root, const json& sourceEntities, const std::vector<Entity>& instances, const std::string& prefabPath)
	{
		PrefabComponent link;
		link.Prefab = prefabPath;
		IdMap toSource;
		for (size_t i = 0; i < instances.size(); ++i)
		{
			const uint64_t source = sourceEntities[i]["ID"].get<uint64_t>();
			link.Members[KeyOf(source)] = IdOf(instances[i]);
			toSource[IdOf(instances[i])] = source;
		}
		if (!root.GetScene()->IsRunning())
			for (Entity instance : instances)
				link.Baseline[KeyOf(toSource[IdOf(instance)])] = SerializeMember(instance, root, toSource);
		root.AddOrReplaceComponent<PrefabComponent>(std::move(link));
	}

	void Prefab::ResolveBaselineAssets(PrefabComponent& link)
	{
		const auto& project = Project::GetActive();
		if (!project)
			return;
		for (auto& record : link.Baseline)
		{
			if (!record.is_object() || !record.contains("Components") || !record["Components"].is_object())
				continue;
			for (const auto& info : ComponentRegistry::GetAll())
			{
				auto component = record["Components"].find(info.Name);
				if (component == record["Components"].end())
					continue;
				for (const auto& field : info.AssetFields)
					ForEachAssetValue(*component, field, [&project](json& value, const std::string&) {
						if (!value.is_object())
							return;
						std::string path, error;
						if (project->GetAssets().Decode(value, path, error, true))
							value = path;
						else
							BS_CORE_WARN("Prefab baseline asset reference: {}", error);
					});
			}
		}
	}

	void Prefab::CaptureOverrides(Entity root)
	{
		auto& link = root.GetComponent<PrefabComponent>();
		ResolveBaselineAssets(link);
		link.Overrides = link.Baseline.empty() ? json::object() : Overrides(link, CaptureInstance(root, link).ByKey, RootKey(link, root));
	}

	bool Prefab::Refresh(Entity root, const json& sourceEntities, RefreshMode mode, std::string& outError, const std::string& field)
	{
		Scene& scene = *root.GetScene();
		if (scene.IsRunning())
		{
			outError = "stop Play or Simulate before refreshing prefab instances";
			return false;
		}
		PrefabComponent link = root.GetComponent<PrefabComponent>();
		if (link.Members.empty())
			Adopt(root, sourceEntities, link);
		const std::string rootKey = KeyOf(sourceEntities[0]["ID"].get<uint64_t>());
		if (RootKey(link, root) != rootKey)
		{
			outError = "the prefab's root entity changed; unpack the instance or instantiate the prefab again";
			return false;
		}
		ResolveBaselineAssets(link);
		const Records next = ToRecords(sourceEntities);
		Records current = CaptureInstance(root, link);

		json merged;
		if (mode == RefreshMode::RevertAll)
		{
			merged = next.ByKey;
			KeepPlacement(merged, current.ByKey, rootKey);
		}
		else
		{
			if (mode == RefreshMode::RevertField)
			{
				if (!Overrides(link, current.ByKey, rootKey).contains(field))
				{
					outError = "'" + field + "' is not an override of this instance (prefab.overrides lists them)";
					return false;
				}
				// The path came from CollectOverrides, so it parses. Reverting restores the baseline there; the
				// merge below then takes the source's value.
				const json::json_pointer pointer(field);
				if (link.Baseline.contains(pointer))
					current.ByKey[pointer] = link.Baseline[pointer];
				else
					current.ByKey[pointer.parent_pointer()].erase(pointer.back());
			}
			merged = Merge(link.Baseline, current.ByKey, next.ByKey);
		}

		// Members keep their instance IDs, including tombstones of entities the instance deleted, so Revert can
		// restore them. Entities new to the instance get fresh IDs.
		std::unordered_set<uint64_t> taken;
		for (const auto& value : link.Members)
			if (IsId(value))
				taken.insert(value.get<uint64_t>());
		IdMap toInstance;
		for (const auto& [key, record] : merged.items())
		{
			auto member = link.Members.find(key);
			const bool foreign = member != link.Members.end() && IsId(*member) && scene.GetEntityByUUID(UUID(member->get<uint64_t>())) && !IsOwnMember(root, scene.GetEntityByUUID(UUID(member->get<uint64_t>())));
			if (member == link.Members.end() || !IsId(*member) || foreign)
			{
				link.Members[key] = NewInstanceId(scene, taken);
				member = link.Members.find(key);
				taken.insert(member->get<uint64_t>());
			}
			toInstance[ParseKey(key)] = member->get<uint64_t>();
		}
		std::unordered_set<uint64_t> previous;
		for (const auto& [key, value] : link.Members.items())
			if (IsId(value) && IsOwnMember(root, scene.GetEntityByUUID(UUID(value.get<uint64_t>()))))
				previous.insert(value.get<uint64_t>());
		// Keys that left both the source and the instance are no longer tracked.
		for (auto it = link.Members.begin(); it != link.Members.end();)
			it = !merged.contains(it.key()) && !next.ByKey.contains(it.key()) ? link.Members.erase(it) : std::next(it);

		const uint64_t rootId = IdOf(root);
		std::vector<json> records = InOrder(merged, { &next.Order, &current.Order });
		std::unordered_set<uint64_t> members;
		for (json& record : records)
		{
			RemapReferences(record, toInstance);
			members.insert(record["ID"].get<uint64_t>());
		}

		// Validate the members' data in isolation, then the hierarchy against the scene, before changing anything.
		json isolated = json::array();
		std::unordered_map<uint64_t, uint64_t> parents;
		for (json& record : records)
		{
			const uint64_t id = record["ID"].get<uint64_t>();
			if (id == rootId)
				record["Parent"] = root.GetParent() ? IdOf(root.GetParent()) : 0;
			parents[id] = record["Parent"].get<uint64_t>();
			json copy = record;
			if (!members.contains(parents[id]))
				copy["Parent"] = 0;
			isolated.push_back(std::move(copy));
		}
		// Local entities parented to a member the merge removes move to the root.
		std::vector<Entity> orphans;
		std::unordered_map<uint64_t, std::vector<Entity>> locals;
		std::unordered_map<uint64_t, uint32_t> heights;
		for (uint64_t id : previous)
		{
			const Entity member = scene.GetEntityByUUID(UUID(id));
			for (Entity child : member.GetChildren())
				if (!previous.contains(IdOf(child)) && !members.contains(IdOf(child)))
				{
					if (members.contains(id))
					{
						locals[id].push_back(child);
						heights[id] = std::max(heights[id], scene.GetSubtreeHeight(child) + 1);
					}
					else if (scene.IsDescendantOf(member, root))
					{
						orphans.push_back(child);
						parents[IdOf(child)] = rootId;
						heights[IdOf(child)] = scene.GetSubtreeHeight(child);
					}
				}
		}
		Scene scratch("Prefab merge validation");
		if (!SceneSerializer::DeserializeScene(scratch, { { "Format", "BasaltScene" }, { "Version", SceneSerializer::FormatVersion }, { "Entities", isolated } }, outError))
			return false;
		for (const auto& [id, parent] : parents)
		{
			uint32_t depth = 0;
			const Entity existing = scene.GetEntityByUUID(UUID(id));
			if (!WalkAncestors(scene, id, parent, parents, depth) || depth + heights[id] > Scene::MaxHierarchyDepth)
			{
				outError = "merging the prefab into '" + (existing ? existing.GetName() : scratch.GetEntityByUUID(UUID(id)).GetName()) + "' would create a hierarchy cycle or exceed " + std::to_string(Scene::MaxHierarchyDepth) + " levels";
				return false;
			}
		}

		// Nothing below reads unvalidated data. Removed members inside the instance are destroyed, after detaching
		// their children so those survive; members the instance moved elsewhere in the scene stay, untracked.
		std::vector<Entity> destroyed;
		for (uint64_t id : previous)
			if (Entity removed = scene.GetEntityByUUID(UUID(id)); !members.contains(id) && scene.IsDescendantOf(removed, root))
				destroyed.push_back(removed);
		for (Entity removed : destroyed)
			for (Entity child : removed.GetChildren())
				scene.SetParent(child, {}, false);
		for (Entity removed : destroyed)
			if (removed.IsValid())
				scene.DestroyEntity(removed);
		for (const json& record : records)
		{
			const uint64_t id = record["ID"].get<uint64_t>();
			Entity entity = scene.GetEntityByUUID(UUID(id));
			if (!entity)
				entity = scene.CreateEntityWithUUID(UUID(id), record["Name"].get<std::string>());
			for (const auto& info : ComponentRegistry::GetAll())
				if (!info.IsCore && info.Name != "Prefab" && info.Has(entity) && !record["Components"].contains(info.Name))
					info.Remove(entity);
			if (!SceneSerializer::DeserializeEntityComponents(entity, record, outError))
			{
				BS_CORE_ERROR("Prefab merge into '{}' failed after validation: {}", entity.GetName(), outError);
				return false;
			}
		}
		// Parent in source order, then local additions after their member siblings, so sibling order is stable.
		bool parented = true;
		auto parent = [&](Entity child, Entity newParent) {
			if (!scene.SetParent(child, newParent, false))
				parented = false;
		};
		for (const json& record : records)
			if (const uint64_t id = record["ID"].get<uint64_t>(); id != rootId)
				parent(scene.GetEntityByUUID(UUID(id)), scene.GetEntityByUUID(UUID(record["Parent"].get<uint64_t>())));
		for (const json& record : records)
			for (Entity local : locals[record["ID"].get<uint64_t>()])
				parent(local, scene.GetEntityByUUID(UUID(record["ID"].get<uint64_t>())));
		for (Entity orphan : orphans)
			parent(orphan, root);
		link.Baseline = next.ByKey;
		root.AddOrReplaceComponent<PrefabComponent>(std::move(link));
		CaptureOverrides(root);
		if (!parented)
		{
			outError = "the merged prefab hierarchy could not be applied completely";
			BS_CORE_ERROR("Prefab merge into '{}': {}", root.GetName(), outError);
			return false;
		}
		return true;
	}

	bool Prefab::Apply(Entity root, const std::filesystem::path& path, std::string& outError)
	{
		if (root.GetScene()->IsRunning())
		{
			outError = "stop Play or Simulate before applying prefab overrides";
			return false;
		}
		json source;
		if (!ReadSource(path, source, outError))
			return false;
		PrefabComponent link = root.GetComponent<PrefabComponent>();
		if (link.Members.empty())
			Adopt(root, source, link);
		const std::string rootKey = KeyOf(source[0]["ID"].get<uint64_t>());
		if (RootKey(link, root) != rootKey)
		{
			outError = "the prefab's root entity changed; unpack the instance or instantiate the prefab again";
			return false;
		}
		ResolveBaselineAssets(link);
		const Records next = ToRecords(source);
		const Records current = CaptureInstance(root, link);
		json merged = Merge(link.Baseline, current.ByKey, next.ByKey);
		KeepPlacement(merged, next.ByKey, rootKey);
		// Members the instance moved outside its own tree go back under their source parent, or the root.
		std::vector<json> entities = InOrder(merged, { &next.Order, &current.Order });
		for (json& record : entities)
		{
			const std::string key = KeyOf(record["ID"].get<uint64_t>());
			if (key == rootKey)
				continue;
			const std::string parent = KeyOf(record["Parent"].get<uint64_t>());
			if (!merged.contains(parent))
			{
				const bool sourceParentKept = next.ByKey.contains(key) && merged.contains(KeyOf(next.ByKey[key]["Parent"].get<uint64_t>()));
				record["Parent"] = sourceParentKept ? next.ByKey[key]["Parent"] : json(ParseKey(rootKey));
			}
		}
		Scene scratch("Apply prefab overrides");
		if (!SceneSerializer::DeserializeScene(scratch, { { "Format", "BasaltScene" }, { "Version", SceneSerializer::FormatVersion }, { "Entities", entities } }, outError))
			return false;
		return SceneSerializer::SavePrefab(scratch.GetEntityByUUID(UUID(ParseKey(rootKey))), path, outError);
	}

}
