#include "Basalt/Scene/Prefab.h"

#include "Basalt/Asset/AssetRegistry.h"
#include "Basalt/Core/Assert.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Hash.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Scene/ComponentRegistry.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/Scene/SceneSerializer.h"

#include <glm/gtc/quaternion.hpp>
#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/ranges.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace Basalt {

	namespace {
		using json = nlohmann::json;
		using IdMap = std::unordered_map<uint64_t, uint64_t>;
		using IdSet = std::unordered_set<uint64_t>;

		// The root's position and rotation place the instance in its scene: they are never overrides, merges and
		// Revert keep the instance's, and Apply never writes them. Its scale is an ordinary field.
		constexpr const char* PlacementFields[] = { "Translation", "Rotation" };
		// Wraps a reference to an entity outside the instance whose UUID equals a source ID (older prefab files reuse
		// the authoring entities' UUIDs), so it is never mistaken for a reference to the instance's own entity.
		constexpr const char* ExternalKey = "External";
		// In a prefab file only: what a nested instance's record changes in its prefab's tree.
		constexpr const char* OverridesKey = "Overrides";
		constexpr const char* LinkComponent = "Prefab";
		constexpr int NestingVersion = 4;

		uint64_t IdOf(Entity entity)
		{
			return static_cast<uint64_t>(entity.GetUUID());
		}

		Entity EntityOf(Scene& scene, uint64_t id)
		{
			return id ? scene.GetEntityByUUID(UUID(id)) : Entity{};
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

		// Tracking holds unsigned IDs only (NormalizeTracking converts Lua's signed ones).
		bool IsId(const json& value)
		{
			return value.is_number_unsigned();
		}

		uint64_t RecordId(const json& record)
		{
			return record.at("ID").get<uint64_t>();
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

		// A record's name for messages; records read from outside may not have a string one.
		std::string NameOf(const json& record)
		{
			const auto name = record.is_object() ? record.find("Name") : record.end();
			return name != record.end() && name->is_string() ? name->get<std::string>() : std::string("<unnamed>");
		}

		bool IsExternal(const json& value)
		{
			return value.is_object() && value.size() == 1 && value.contains(ExternalKey) && IsId(value[ExternalKey]);
		}

		// Calls visit on a record's parent and on every entity field of its components.
		void ForEachReference(json& record, const std::function<void(json& value, bool isParent)>& visit)
		{
			visit(record["Parent"], true);
			auto components = record.find("Components");
			if (components == record.end() || !components->is_object())
				return;
			for (const auto& info : ComponentRegistry::GetAll())
			{
				auto component = components->find(info.Name);
				if (component == components->end() || !component->is_object())
					continue;
				for (const auto& field : info.EntityFields)
					if (auto value = component->find(field); value != component->end())
						visit(*value, false);
			}
		}

		void RenumberReferences(json& record, const IdMap& ids)
		{
			ForEachReference(record, [&](json& value, bool) {
				if (IsId(value))
					if (const auto it = ids.find(value.get<uint64_t>()); it != ids.end())
						value = it->second;
			});
		}

		// The ID a nested prefab's entity has in the file holding it. Derived, not stored, so overrides and references
		// to it stay valid however often that prefab changes, and two nested copies of one prefab never share an ID.
		uint64_t NestedId(uint64_t nestedRoot, uint64_t inner)
		{
			std::array<unsigned char, 16> bytes{};
			for (size_t i = 0; i < 8; ++i)
			{
				bytes[i] = static_cast<unsigned char>(nestedRoot >> (8 * i));
				bytes[8 + i] = static_cast<unsigned char>(inner >> (8 * i));
			}
			const uint64_t id = HashBytes64(bytes.data(), bytes.size());
			return id ? id : 1;
		}

		// A prefab's records as the tree of the nested root in another file: renumbered, the root in the nested record's
		// place, marked as an instance of prefab.
		void NestRecords(json& records, uint64_t nestedRoot, uint64_t parent, const std::string& prefab)
		{
			const uint64_t innerRoot = RecordId(records[0]);
			IdMap ids;
			for (const json& record : records)
				ids[RecordId(record)] = RecordId(record) == innerRoot ? nestedRoot : NestedId(nestedRoot, RecordId(record));
			for (json& record : records)
			{
				record["ID"] = ids.at(RecordId(record));
				RenumberReferences(record, ids);
			}
			records[0]["Parent"] = parent;
			if (!records[0].contains("Components"))
				records[0]["Components"] = json::object();
			records[0]["Components"][LinkComponent] = json{ { LinkComponent, prefab } };
		}

		// Instance space to source space: members become their source IDs; any other reference keeps its UUID, marked
		// external where it equals a source ID.
		void ToSourceSpace(json& record, const IdMap& toSource, const IdSet& sourceKeys)
		{
			record["ID"] = toSource.at(RecordId(record));
			ForEachReference(record, [&](json& value, bool) {
				if (!IsId(value) || value == 0)
					return;
				const uint64_t id = value.get<uint64_t>();
				if (const auto it = toSource.find(id); it != toSource.end())
					value = it->second;
				else if (sourceKeys.contains(id))
					value = json{ { ExternalKey, id } };
			});
		}

		// Source space to instance space. A reference to an entity the instance deleted keeps its reserved ID (reverting
		// the deletion restores the reference, and no reserved ID is ever reused); a parent among them, or any other source
		// entity the instance does not have, becomes the root or no reference. Never an unrelated entity that happens to
		// share the source ID.
		void ToInstanceSpace(json& record, const IdMap& toInstance, const IdMap& reserved, const IdSet& sourceKeys, uint64_t rootId)
		{
			record["ID"] = toInstance.at(RecordId(record));
			ForEachReference(record, [&](json& value, bool isParent) {
				if (IsExternal(value))
				{
					value = json(value[ExternalKey]);
					return;
				}
				if (!IsId(value) || value == 0)
					return;
				const uint64_t id = value.get<uint64_t>();
				if (const auto it = toInstance.find(id); it != toInstance.end())
					value = it->second;
				else if (const auto kept = reserved.find(id); kept != reserved.end() && !isParent)
					value = kept->second;
				else if (sourceKeys.contains(id))
					value = isParent ? rootId : 0;
			});
		}

		// A member as a source-space record: what a prefab file holds for it, as the serializer writes it.
		json SerializeMember(Entity entity, Entity root, const IdMap& toSource, const IdSet& sourceKeys)
		{
			json components = json::object();
			for (const auto& info : ComponentRegistry::GetAll())
				if (info.Name != "Tag" && info.Name != LinkComponent && info.Has(entity))
					components[info.Name] = info.Serialize(entity);
			// A nested instance's root says what it is an instance of, and the outer instance tracks the rest.
			if (const auto* link = entity.TryGetComponent<PrefabComponent>(); link && entity != root && !link->Prefab.empty())
				components[LinkComponent] = json{ { LinkComponent, link->Prefab } };
			const Entity parent = entity.GetParent();
			json record = { { "ID", IdOf(entity) }, { "Name", entity.GetName() }, { "Parent", entity == root || !parent ? 0 : IdOf(parent) }, { "Components", std::move(components) } };
			ToSourceSpace(record, toSource, sourceKeys);
			return record;
		}

		IdSet MemberIds(const PrefabComponent& link)
		{
			IdSet ids;
			for (const auto& value : link.Members)
				if (IsId(value))
					ids.insert(value.get<uint64_t>());
			return ids;
		}

		// A Members value belongs to this instance only when the entity is inside the instance's tree and every instance
		// root on the way up is a nested one this instance lists: a merge never touches an entity outside its instance (a
		// hand-edited Members value, or a member moved out of the tree, which then counts as deleted by the instance) or
		// anything inside an instance that tracks itself (its members, and its local additions, which it never lists).
		bool IsOwnMember(Entity root, Entity entity, const IdSet& members)
		{
			for (Entity current = entity; current; current = current.GetParent())
			{
				if (current == root)
					return true;
				if (const auto* link = current.TryGetComponent<PrefabComponent>(); link && (!link->Members.empty() || !members.contains(IdOf(current))))
					return false;
			}
			return false;
		}

		std::string RootKey(const PrefabComponent& link, Entity root)
		{
			for (const auto& [key, value] : link.Members.items())
				if (IsId(value) && value.get<uint64_t>() == IdOf(root))
					return key;
			return {};
		}

		// Records keyed by source ID. Order is a sibling-order hint: the source's order, or depth first from the root
		// for an instance. Records it omits (kept by a merge from the other side) come last.
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
				// The root's link is a variant's base: instances replace it with their own, and only nested roots keep theirs.
				if (records.Order.empty())
					entity["Components"].erase(LinkComponent);
				const std::string key = KeyOf(RecordId(entity));
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

		// Every ID that names a source entity of this instance, now or before.
		IdSet SourceKeys(const PrefabComponent& link, const json* next)
		{
			IdSet keys;
			for (const json* records : { &link.Members, &link.Baseline, next })
				if (records)
					for (const auto& [key, value] : records->items())
						keys.insert(ParseKey(key));
			keys.erase(0);
			return keys;
		}

		// The instance's members as records, depth first from the root. Members the instance deleted are absent.
		Records CaptureInstance(Entity root, const PrefabComponent& link, const IdSet& sourceKeys)
		{
			IdMap toSource;
			for (const auto& [key, value] : link.Members.items())
				if (IsId(value))
					toSource[value.get<uint64_t>()] = ParseKey(key);
			const IdSet members = MemberIds(link);
			Records records;
			std::function<void(Entity)> visit = [&](Entity entity) {
				if (const auto it = toSource.find(IdOf(entity)); it != toSource.end() && IsOwnMember(root, entity, members))
					if (const std::string key = KeyOf(it->second); !records.ByKey.contains(key))
					{
						records.Order.push_back(key);
						records.ByKey[key] = SerializeMember(entity, root, toSource, sourceKeys);
					}
				for (Entity child : entity.GetChildren())
					visit(child);
			};
			visit(root);
			return records;
		}

		bool IsEulerTriple(const json& value)
		{
			return value.is_array() && value.size() == 3 && std::all_of(value.begin(), value.end(), [](const json& v) { return v.is_number(); });
		}

		// Equality that tolerates what deserializing and serializing again changes: float rounding, and Euler angles
		// (degrees, as RotationField writes them) that name the same rotation, which near ±90° may be another triple.
		bool Equivalent(const json& a, const json& b, bool rotation = false)
		{
			if (a == b)
				return true;
			if (a.is_number() && b.is_number())
			{
				if (!a.is_number_float() && !b.is_number_float())
					return false;
				// Component values are floats: a round trip (degrees and radians, float to double) moves them by a few
				// float ULPs at most, so anything larger is an edit, however large the value.
				const double x = a.get<double>();
				const double y = b.get<double>();
				constexpr double Ulps = 4.0 * std::numeric_limits<float>::epsilon();
				return std::abs(x - y) <= std::max(Ulps * std::max(std::abs(x), std::abs(y)), 1e-7);
			}
			if (rotation && IsEulerTriple(a) && IsEulerTriple(b))
			{
				const auto toQuat = [](const json& euler) {
					const glm::vec3 degrees(euler[0].get<float>(), euler[1].get<float>(), euler[2].get<float>());
					return glm::normalize(glm::dquat(Math::QuatFromEuler(glm::radians(degrees))));
				};
				const double dot = std::min(1.0, std::abs(glm::dot(toQuat(a), toQuat(b))));
				return 2.0 * std::acos(dot) < 1e-4;
			}
			// Lua cannot tell an empty list from an empty table.
			if (a.empty() && b.empty() && (a.is_array() || a.is_object()) && (b.is_array() || b.is_object()))
				return true;
			if (a.type() != b.type() || a.size() != b.size())
				return false;
			if (a.is_array())
			{
				for (size_t i = 0; i < a.size(); ++i)
					if (!Equivalent(a[i], b[i]))
						return false;
				return true;
			}
			if (a.is_object())
			{
				for (const auto& [key, value] : a.items())
				{
					const auto other = b.find(key);
					if (other == b.end() || !Equivalent(value, *other, key == "Rotation"))
						return false;
				}
				return true;
			}
			return false;
		}

		// Three-way merge. Objects merge by key; any other value (numbers, strings, arrays such as vectors, colors and
		// component lists) is one authored value. Instance changes win: an instance deletion beats a source edit, and
		// an instance edit keeps a key (even a whole entity or component) the source deleted.
		json Merge(const json& base, const json& current, const json& source, bool rotation = false)
		{
			if (Equivalent(base, current, rotation))
				return source;
			if (!base.is_object() || !current.is_object() || !source.is_object())
				return current;
			json result = source;
			for (const auto& [key, old] : base.items())
			{
				const auto now = current.find(key);
				if (now == current.end())
					result.erase(key);
				else if (!Equivalent(old, *now, key == "Rotation"))
					result[key] = source.contains(key) ? Merge(old, *now, source[key], key == "Rotation") : *now;
			}
			for (const auto& [key, value] : current.items())
				if (!base.contains(key))
					result[key] = value;
			return result;
		}

		// What turns base into current, as a JSON merge patch: keys current lacks are null, objects recurse, and any other
		// value is written whole. Equivalent values are left out, so a file holds only real differences.
		json DiffObjects(const json& base, const json& current, bool rotation = false)
		{
			json patch = json::object();
			for (const auto& [key, value] : base.items())
			{
				const auto now = current.find(key);
				if (now == current.end())
					patch[key] = nullptr;
				else if (!Equivalent(value, *now, key == "Rotation"))
					patch[key] = value.is_object() && now->is_object() ? DiffObjects(value, *now, key == "Rotation") : *now;
			}
			for (const auto& [key, value] : current.items())
				if (!base.contains(key))
					patch[key] = value;
			return patch;
		}

		void ApplyPatch(json& target, const json& patch)
		{
			for (const auto& [key, value] : patch.items())
			{
				if (value.is_null())
					target.erase(key);
				else if (const auto existing = target.find(key); value.is_object() && existing != target.end() && existing->is_object())
					ApplyPatch(*existing, value);
				else
					target[key] = value;
			}
		}

		// JSON-pointer paths where current differs from base: keys current lacks report null (deleted entities,
		// removed components and fields), keys only current has report their value.
		void CollectOverrides(const json& base, const json& current, const std::string& path, json& out, bool rotation = false)
		{
			if (Equivalent(base, current, rotation))
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
					CollectOverrides(value, *now, child, out, key == "Rotation");
			}
			for (const auto& [key, value] : current.items())
				if (!base.contains(key))
					out[path + "/" + PointerToken(key)] = value;
		}

		json OverridesOf(const PrefabComponent& link, const json& current, const std::string& rootKey)
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

		// A fresh nonzero ID that no scene entity and no ID in taken has; it is added to taken.
		uint64_t ClaimNewId(Scene& scene, IdSet& taken)
		{
			uint64_t id = 0;
			while (!id || taken.contains(id) || scene.GetEntityByUUID(UUID(id)))
				id = static_cast<uint64_t>(UUID());
			taken.insert(id);
			return id;
		}

		// The path a record's Prefab component names; empty when it has none. A file's holds an asset reference.
		std::string LinkPath(const json& record)
		{
			const auto components = record.find("Components");
			if (components == record.end() || !components->is_object())
				return {};
			const auto link = components->find(LinkComponent);
			if (link == components->end() || !link->is_object())
				return {};
			const auto path = link->find(LinkComponent);
			return path != link->end() && path->is_string() ? Prefab::NormalizePath(path->get<std::string>()) : std::string();
		}

		// Matches source records to an entity tree: the roots, then children by name in order (each entity once).
		// Returns source ID -> entity UUID. Other instances' roots never match, unless the source has an instance of the
		// same prefab there.
		IdMap MatchByName(const json& sourceEntities, Entity root)
		{
			std::unordered_map<uint64_t, std::vector<size_t>> children;
			for (size_t i = 1; i < sourceEntities.size(); ++i)
				children[sourceEntities[i]["Parent"].get<uint64_t>()].push_back(i);
			IdMap matches;
			IdSet used;
			std::function<void(uint64_t, Entity)> match = [&](uint64_t source, Entity entity) {
				matches[source] = IdOf(entity);
				used.insert(IdOf(entity));
				for (size_t index : children[source])
				{
					const json& child = sourceEntities[index];
					const auto name = child.find("Name");
					if (name == child.end() || !name->is_string())
						continue;
					const std::string nested = LinkPath(child);
					for (Entity candidate : entity.GetChildren())
						if (const auto* link = candidate.TryGetComponent<PrefabComponent>(); !used.contains(IdOf(candidate)) && candidate.GetName() == name->get_ref<const std::string&>() && (!link || Prefab::NormalizePath(link->Prefab) == nested))
						{
							match(RecordId(child), candidate);
							break;
						}
				}
			};
			match(RecordId(sourceEntities[0]), root);
			return matches;
		}

		// Starts tracking an instance without tracking (saved before overrides existed, or created during Play):
		// everything that differs from the current source becomes an override, so adoption changes nothing. Source
		// entities without a match count as deleted by the instance.
		void Adopt(Entity root, const json& sourceEntities, PrefabComponent& link)
		{
			link.Members = json::object();
			IdSet taken;
			for (const auto& [source, entity] : MatchByName(sourceEntities, root))
			{
				link.Members[KeyOf(source)] = entity;
				taken.insert(entity);
			}
			for (const json& entity : sourceEntities)
				if (const std::string key = KeyOf(RecordId(entity)); !link.Members.contains(key))
					link.Members[key] = ClaimNewId(*root.GetScene(), taken);
			link.Baseline = ToRecords(sourceEntities).ByKey;
		}

		// The number of ancestors member will have, walking the merged hierarchy up from parent; nothing on a cycle
		// through member or past the scene's depth limit.
		std::optional<uint32_t> AncestorDepth(Scene& scene, uint64_t member, uint64_t parent, const IdMap& parents)
		{
			uint32_t depth = 0;
			while (parent)
			{
				if (parent == member || ++depth > Scene::MaxHierarchyDepth)
					return std::nullopt;
				if (const auto it = parents.find(parent); it != parents.end())
					parent = it->second;
				else
				{
					const Entity entity = EntityOf(scene, parent);
					parent = entity && entity.GetParent() ? IdOf(entity.GetParent()) : 0;
				}
			}
			return depth;
		}

		// What every merge starts from: the instance's tracking (adopted when missing), the new source and the
		// instance's current state, in source space.
		struct MergeInputs
		{
			PrefabComponent Link;
			bool Adopted = false;
			std::string RootKey;
			Records Next;
			Records Current;
			IdSet SourceKeys;
		};

		std::optional<MergeInputs> PrepareMerge(Entity root, const json& sourceEntities, std::string& outError)
		{
			MergeInputs in{ root.GetComponent<PrefabComponent>() };
			if (!Prefab::IsTracked(in.Link))
			{
				Adopt(root, sourceEntities, in.Link);
				in.Adopted = true;
			}
			in.RootKey = KeyOf(RecordId(sourceEntities[0]));
			if (const std::string tracked = RootKey(in.Link, root); tracked != in.RootKey)
			{
				outError = tracked.empty() ? "the instance's tracking does not include its root entity; unpack it or instantiate the prefab again"
										   : "the prefab's root entity changed (the file now holds another tree); unpack the instance or instantiate the prefab again";
				return std::nullopt;
			}
			in.Next = ToRecords(sourceEntities);
			in.SourceKeys = SourceKeys(in.Link, &in.Next.ByKey);
			in.Current = CaptureInstance(root, in.Link, in.SourceKeys);
			return in;
		}

		enum class MergeKind
		{
			Merge,
			RevertAll,
			RevertField
		};

		// The instance's records with one override undone: the baseline restored at its path, so the merge then takes the
		// source's value there. Nothing when the path is not an override.
		std::optional<json> WithoutOverride(const MergeInputs& in, const std::string& field, std::string& outError)
		{
			if (!OverridesOf(in.Link, in.Current.ByKey, in.RootKey).contains(field))
			{
				outError = "'" + field + "' is not an override of this instance (prefab.overrides lists them)";
				return std::nullopt;
			}
			// The path came from CollectOverrides, so it parses and its parent exists.
			json current = in.Current.ByKey;
			const json::json_pointer pointer(field);
			if (in.Link.Baseline.contains(pointer))
				current[pointer] = in.Link.Baseline[pointer];
			else
				current[pointer.parent_pointer()].erase(pointer.back());
			return current;
		}

		// The merged records in source space, or nothing when the field to revert is not an override.
		std::optional<json> MergedRecords(const MergeInputs& in, MergeKind kind, const std::string& field, std::string& outError)
		{
			json merged;
			if (kind == MergeKind::RevertAll)
				merged = in.Next.ByKey;
			else if (kind == MergeKind::RevertField)
			{
				const auto current = WithoutOverride(in, field, outError);
				if (!current)
					return std::nullopt;
				merged = Merge(in.Link.Baseline, *current, in.Next.ByKey);
			}
			else
				merged = Merge(in.Link.Baseline, in.Current.ByKey, in.Next.ByKey);
			KeepPlacement(merged, in.Current.ByKey, in.RootKey);
			return merged;
		}

		// Instance IDs of the merged records, and the reserved IDs of entities the instance deleted, by source ID.
		struct InstanceIds
		{
			IdMap Merged;
			IdMap Reserved;
		};

		// Members keep their instance IDs, including IDs of entities the instance deleted, so Revert recreates them
		// with the same IDs. Entities new to the instance get fresh ones. Keys that left both the source and the
		// instance stop being tracked.
		InstanceIds AssignInstanceIds(Scene& scene, Entity root, const json& merged, const Records& next, PrefabComponent& link)
		{
			IdSet taken;
			for (const auto& value : link.Members)
				if (IsId(value))
					taken.insert(value.get<uint64_t>());
			IdMap toInstance;
			for (const auto& [key, record] : merged.items())
			{
				const auto member = link.Members.find(key);
				const bool tracked = member != link.Members.end() && IsId(*member);
				uint64_t id = tracked ? member->get<uint64_t>() : 0;
				// Another instance's entity, named by a hand-edited Members value, is never claimed.
				if (const Entity existing = EntityOf(scene, id); !tracked || (existing && !IsOwnMember(root, existing, taken)))
				{
					id = ClaimNewId(scene, taken);
					link.Members[key] = id;
				}
				toInstance[ParseKey(key)] = id;
			}
			IdMap reserved;
			for (auto it = link.Members.begin(); it != link.Members.end();)
			{
				if (!merged.contains(it.key()) && !next.ByKey.contains(it.key()))
					it = link.Members.erase(it);
				else
				{
					if (!merged.contains(it.key()) && IsId(*it))
						reserved[ParseKey(it.key())] = it->get<uint64_t>();
					++it;
				}
			}
			return { std::move(toInstance), std::move(reserved) };
		}

		// The instance-space result of a merge, checked against the scene before anything changes.
		struct MergePlan
		{
			// Merged member records in sibling order, in instance space; the root's parent is its scene parent.
			std::vector<json> Records;
			IdSet Members;
			// Members before the merge that still exist, depth first from the root (a stable order for orphans).
			std::vector<uint64_t> Previous;
			// The final parent of every member and of each orphan.
			IdMap Parents;
			// Entities the instance added under a kept member, by member, in their order.
			std::unordered_map<uint64_t, std::vector<Entity>> Locals;
			// Entities the instance added under a member the merge removes; they move to the root.
			std::vector<Entity> Orphans;
			// Members the merge removes: destroyed.
			std::vector<Entity> Destroyed;
			// Members whose parent changes, parents before children.
			std::vector<uint64_t> Reparented;
		};

		bool PlanMerge(Scene& scene, Entity root, const json& merged, const MergeInputs& in, const InstanceIds& ids, MergePlan& plan, std::string& outError)
		{
			const uint64_t rootId = IdOf(root);
			// Current holds exactly the own members that exist, in tree order.
			for (const std::string& key : in.Current.Order)
				plan.Previous.push_back(in.Link.Members[key].get<uint64_t>());
			const IdSet previous(plan.Previous.begin(), plan.Previous.end());
			plan.Records = InOrder(merged, { &in.Next.Order, &in.Current.Order });
			for (json& record : plan.Records)
			{
				ToInstanceSpace(record, ids.Merged, ids.Reserved, in.SourceKeys, rootId);
				plan.Members.insert(RecordId(record));
			}

			// Validate the members' data in isolation; parents outside the members are the scene's business.
			json isolated = json::array();
			for (json& record : plan.Records)
			{
				const uint64_t id = RecordId(record);
				if (id == rootId)
					record["Parent"] = root.GetParent() ? IdOf(root.GetParent()) : 0;
				plan.Parents[id] = record["Parent"].get<uint64_t>();
				json copy = record;
				if (!plan.Members.contains(plan.Parents[id]))
					copy["Parent"] = 0;
				isolated.push_back(std::move(copy));
			}
			Scene scratch("Prefab merge validation");
			if (!SceneSerializer::DeserializeScene(scratch, SceneSerializer::SceneDocument(std::move(isolated)), outError))
				return false;

			std::unordered_map<uint64_t, uint32_t> heights;
			for (uint64_t id : plan.Previous)
			{
				const Entity member = EntityOf(scene, id);
				const bool kept = plan.Members.contains(id);
				if (!kept)
					plan.Destroyed.push_back(member);
				for (Entity child : member.GetChildren())
				{
					if (previous.contains(IdOf(child)) || plan.Members.contains(IdOf(child)))
						continue;
					if (kept)
					{
						plan.Locals[id].push_back(child);
						heights[id] = std::max(heights[id], scene.GetSubtreeHeight(child) + 1);
					}
					else
					{
						plan.Orphans.push_back(child);
						plan.Parents[IdOf(child)] = rootId;
						heights[IdOf(child)] = scene.GetSubtreeHeight(child);
					}
				}
			}

			// The final hierarchy must be acyclic and within the depth limit. Parenting the moved members in depth order
			// after detaching them then never passes through an invalid state.
			std::vector<std::pair<uint32_t, uint64_t>> moved;
			for (const auto& [id, parent] : plan.Parents)
			{
				const Entity existing = EntityOf(scene, id);
				const uint32_t depth = AncestorDepth(scene, id, parent, plan.Parents).value_or(Scene::MaxHierarchyDepth + 1);
				if (depth + heights[id] > Scene::MaxHierarchyDepth)
				{
					const std::string name = existing ? existing.GetName() : scratch.GetEntityByUUID(UUID(id)).GetName();
					outError = fmt::format("merging the prefab into '{}' would create a hierarchy cycle or exceed {} levels", name, Scene::MaxHierarchyDepth);
					return false;
				}
				const uint64_t currentParent = existing && existing.GetParent() ? IdOf(existing.GetParent()) : 0;
				if (id != rootId && plan.Members.contains(id) && (!existing || currentParent != parent))
					moved.emplace_back(depth, id);
			}
			std::sort(moved.begin(), moved.end());
			for (const auto& [depth, id] : moved)
				plan.Reparented.push_back(id);
			return true;
		}

		// Applies a validated plan. Only internal errors fail here (all of them are returned), and the scene is then
		// partly merged.
		bool ApplyPlan(Scene& scene, Entity root, const MergePlan& plan, std::string& outError)
		{
			const uint64_t rootId = IdOf(root);
			std::vector<std::string> errors;
			auto parent = [&](Entity child, Entity newParent) {
				if (!scene.SetParent(child, newParent, false))
					errors.push_back(fmt::format("could not parent '{}' to '{}'", child.GetName(), newParent ? newParent.GetName() : std::string("the scene root")));
			};
			// Detach first, so destroying a removed member does not destroy its children (kept members and additions).
			for (Entity removed : plan.Destroyed)
				for (Entity child : removed.GetChildren())
					parent(child, {});
			for (Entity removed : plan.Destroyed)
				if (removed.IsValid())
					scene.DestroyEntity(removed);
			for (uint64_t id : plan.Reparented)
				if (Entity entity = EntityOf(scene, id))
					parent(entity, {});

			for (const json& record : plan.Records)
			{
				const uint64_t id = RecordId(record);
				Entity entity = EntityOf(scene, id);
				if (!entity)
					entity = scene.CreateEntityWithUUID(UUID(id), record["Name"].get<std::string>());
				// The root's Prefab component is this instance's link, which records never hold.
				for (const auto& info : ComponentRegistry::GetAll())
					if (!info.IsCore && (info.Name != LinkComponent || entity != root) && info.Has(entity) && !record["Components"].contains(info.Name))
						info.Remove(entity);
				if (std::string error; !SceneSerializer::DeserializeEntityComponents(entity, record, error))
					errors.push_back(fmt::format("'{}': {}", entity.GetName(), error));
			}
			for (uint64_t id : plan.Reparented)
				parent(EntityOf(scene, id), EntityOf(scene, plan.Parents.at(id)));
			// Parenting to the same parent again appends, which settles sibling order: members in source order, then the
			// entities the instance added, in theirs. The hierarchy does not change, so this cannot fail.
			for (const json& record : plan.Records)
				if (const uint64_t id = RecordId(record); id != rootId && plan.Members.contains(plan.Parents.at(id)))
					parent(EntityOf(scene, id), EntityOf(scene, plan.Parents.at(id)));
			for (const json& record : plan.Records)
				if (const auto locals = plan.Locals.find(RecordId(record)); locals != plan.Locals.end())
					for (Entity local : locals->second)
						parent(local, EntityOf(scene, RecordId(record)));
			for (Entity orphan : plan.Orphans)
				parent(orphan, root);
			outError = fmt::format("{}", fmt::join(errors, "; "));
			return errors.empty();
		}

		Prefab::RefreshResult MergeInto(Entity root, const json& sourceEntities, MergeKind kind, const std::string& field, std::string& outError)
		{
			Scene& scene = *root.GetScene();
			if (scene.IsRunning())
			{
				outError = "stop Play or Simulate before refreshing prefab instances";
				return Prefab::RefreshResult::Rejected;
			}
			auto prepared = PrepareMerge(root, sourceEntities, outError);
			if (!prepared)
				return Prefab::RefreshResult::Rejected;
			MergeInputs& in = *prepared;
			// The source has not changed since the last merge, so the instance already holds it.
			if (kind == MergeKind::Merge && !in.Adopted && Equivalent(in.Link.Baseline, in.Next.ByKey))
				return Prefab::RefreshResult::Unchanged;
			auto mergedRecords = MergedRecords(in, kind, field, outError);
			if (!mergedRecords)
				return Prefab::RefreshResult::Rejected;
			const json& merged = *mergedRecords;
			// A copy: PlanMerge reads the members as they were before AssignInstanceIds.
			PrefabComponent link = in.Link;
			const InstanceIds ids = AssignInstanceIds(scene, root, merged, in.Next, link);
			MergePlan plan;
			if (!PlanMerge(scene, root, merged, in, ids, plan, outError))
				return Prefab::RefreshResult::Rejected;
			const bool complete = ApplyPlan(scene, root, plan, outError);
			// A partial merge keeps the old baseline (with the members it created), so merging again finishes it: what
			// applied now equals the source, and what did not still equals the baseline and takes the source's value.
			link.Baseline = complete ? in.Next.ByKey : in.Link.Baseline;
			root.AddOrReplaceComponent<PrefabComponent>(std::move(link));
			if (!complete)
			{
				BS_CORE_ERROR("Prefab merge into '{}' failed after validation: {}", root.GetName(), outError);
				return Prefab::RefreshResult::Partial;
			}
			return Prefab::RefreshResult::Merged;
		}

		// One read of a prefab file through everything it nests.
		struct Expansion
		{
			Prefab::ReadReport& Report;
			// The files being expanded, outermost first: FileKey and name.
			std::vector<std::pair<std::string, std::string>> Chain;
			// Each file expands once per read, however often it is nested.
			std::unordered_map<std::string, json> Files;
			size_t Entities = 0;
		};

		json* ReadExpanded(const std::filesystem::path& path, const std::string& name, Expansion& expansion, std::string& outError);

		// IDs, parents and the single root, listed first. A missing parent becomes 0.
		bool CheckRecords(json& entities, const std::string& name, std::string& outError)
		{
			IdSet ids;
			for (size_t i = 0; i < entities.size(); ++i)
			{
				json& entity = entities[i];
				const std::string context = "prefab '" + name + "' entity #" + std::to_string(i);
				if (!entity.is_object() || !entity.contains("ID") || !IsId(entity["ID"]) || entity["ID"] == 0)
				{
					outError = context + " needs a unique nonzero 'ID': instances track entities by it";
					return false;
				}
				if (!ids.insert(RecordId(entity)).second)
				{
					outError = context + " has duplicate ID " + entity["ID"].dump();
					return false;
				}
				const auto parent = entity.find("Parent");
				if (parent != entity.end() && !IsId(*parent))
				{
					outError = context + " has a 'Parent' that is not an entity ID";
					return false;
				}
				const bool isRoot = parent == entity.end() || *parent == 0;
				if (isRoot != (i == 0))
				{
					outError = "prefab '" + name + "' must have exactly one root entity, listed first";
					return false;
				}
				entity["Parent"] = isRoot ? json(uint64_t{ 0 }) : json(*parent);
				if (const auto components = entity.find("Components"); components != entity.end() && !components->is_object())
				{
					outError = context + " has 'Components' that is not an object";
					return false;
				}
			}
			return true;
		}

		bool ParseRecords(std::string_view text, const std::string& name, json& outEntities, std::string& outError)
		{
			json data = ParseJson(text);
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
			if (const auto version = data.find("Version"); version != data.end() && (!version->is_number_integer() || version->get<int64_t>() > SceneSerializer::FormatVersion))
			{
				outError = "prefab '" + name + "' has format version " + version->dump() + ", newer than the supported version " + std::to_string(SceneSerializer::FormatVersion);
				return false;
			}
			if (!CheckRecords(data["Entities"], name, outError))
				return false;
			// Before version 4 a prefab could not nest: a Prefab component in such a file is no link, only left over.
			if (const auto version = data.find("Version"); version != data.end() && version->get<int64_t>() < NestingVersion)
				for (json& entity : data["Entities"])
					if (const auto components = entity.find("Components"); components != entity.end())
						components->erase(LinkComponent);
			outEntities = std::move(data["Entities"]);
			return true;
		}

		// The prefab a record's link names, as a path; empty when it names none. Links come from files that may be hand
		// edited: they name prefabs inside the project, and an unresolved UUID says so.
		bool NamedPrefab(const json& value, const std::string& context, std::string& outPath, std::string& outError)
		{
			const auto& project = Project::GetActive();
			if (value.is_string())
				outPath = Prefab::NormalizePath(value.get<std::string>());
			else if (std::string error; !project || !project->GetAssets().Decode(value, outPath, error, true))
			{
				outError = context + ": " + (project ? error : "UUID asset references require an active project");
				return false;
			}
			if (outPath.empty())
				return true;
			if (AssetRegistry::IsUnresolvedKey(outPath))
			{
				outError = context + ": " + (project ? project->GetAssets().DescribeUnresolved(outPath) : "unresolved asset UUID");
				return false;
			}
			const std::filesystem::path path = FileSystem::PathFromUtf8(outPath);
			if (path.is_absolute() || path.has_root_path() || *path.begin() == ".." || FileSystem::GetLowercaseExtension(path) != ".bprefab")
			{
				outError = context + " must name a .bprefab file inside the project, got '" + outPath + "'";
				return false;
			}
			return true;
		}

		// A nested record's tree, with its overrides, appended to flat.
		bool ExpandNested(const json& record, const std::string& prefab, const std::string& name, Expansion& expansion, json& flat, std::string& outError)
		{
			const json* source = ReadExpanded(Project::ResolvePath(prefab), prefab, expansion, outError);
			if (!source)
				return false;
			expansion.Entities += source->size();
			if (expansion.Entities > Prefab::MaxExpandedEntities)
			{
				outError = fmt::format("prefab '{}' expands past {} entities", name, Prefab::MaxExpandedEntities);
				return false;
			}
			const uint64_t parent = record["Parent"].get<uint64_t>();
			json records = *source;
			NestRecords(records, RecordId(record), parent, prefab);
			const json& link = record["Components"][LinkComponent];
			IdSet deleted;
			if (const auto overrides = link.find(OverridesKey); overrides != link.end())
			{
				if (!overrides->is_object())
				{
					outError = fmt::format("prefab '{}' entity {}: 'Overrides' must be an object", name, RecordId(record));
					return false;
				}
				std::unordered_map<std::string, size_t> indices;
				for (size_t i = 0; i < records.size(); ++i)
					indices[KeyOf(RecordId(records[i]))] = i;
				for (const auto& [key, patch] : overrides->items())
				{
					const auto index = indices.find(key);
					// The nested prefab dropped this entity, and that wins: overrides alone could not rebuild it.
					if (index == indices.end())
					{
						if (!patch.is_null())
							expansion.Report.Warnings.push_back(fmt::format("prefab '{}': the overrides of entity {} do nothing, '{}' no longer has it (saving the prefab drops them)", name, key, prefab));
						continue;
					}
					if (patch.is_null() && index->second != 0)
					{
						deleted.insert(ParseKey(key));
						continue;
					}
					if (!patch.is_object())
					{
						outError = fmt::format("prefab '{}': the override of entity {} must be an object, or null for an entity other than the root", name, key);
						return false;
					}
					json& target = records[index->second];
					const json id = target["ID"];
					ApplyPatch(target, patch);
					target["ID"] = id;
				}
			}
			records[0]["Parent"] = parent;
			for (json& inner : records)
				if (!deleted.contains(RecordId(inner)))
					flat.push_back(std::move(inner));
			return true;
		}

		// Replaces nested records with the trees they stand for.
		bool ExpandRecords(json& entities, const std::string& name, Expansion& expansion, std::string& outError)
		{
			json flat = json::array();
			bool nested = false;
			for (size_t i = 0; i < entities.size(); ++i)
			{
				json& record = entities[i];
				std::string prefab;
				const auto components = record.find("Components");
				if (components != record.end())
				{
					const auto link = components->find(LinkComponent);
					if (link != components->end() && link->is_object() && link->contains(LinkComponent) && !NamedPrefab((*link)[LinkComponent], fmt::format("prefab '{}' entity #{}", name, i), prefab, outError))
					{
						// A deleted prefab's UUID no longer resolves, but its instances must still hear that it changed.
						if (const auto unresolved = AssetRegistry::ParseUnresolvedKey(prefab))
							expansion.Report.Files.insert(Prefab::FileKey(Project::ResolvePath(Prefab::NormalizePath(unresolved->second))));
						return false;
					}
					if (prefab.empty())
						components->erase(LinkComponent);
				}
				if (prefab.empty())
				{
					flat.push_back(std::move(record));
					continue;
				}
				nested = true;
				if (expansion.Chain.size() == 1)
				{
					expansion.Report.Nested.push_back({ RecordId(record), prefab });
					if (i == 0)
						expansion.Report.Base = prefab;
				}
				if (!ExpandNested(record, prefab, name, expansion, flat, outError))
					return false;
			}
			if (nested)
			{
				// An entity added under one the nested prefab dropped stays, at the root.
				IdSet ids;
				for (const json& record : flat)
					ids.insert(RecordId(record));
				for (json& record : flat)
					if (const auto parent = record.find("Parent"); parent != record.end() && IsId(*parent) && *parent != 0 && !ids.contains(parent->get<uint64_t>()))
					{
						expansion.Report.Warnings.push_back(fmt::format("prefab '{}': the parent of entity {} is gone, so it moved to the root", name, RecordId(record)));
						*parent = RecordId(flat[0]);
					}
				if (!CheckRecords(flat, name, outError))
					return false;
			}
			entities = std::move(flat);
			return true;
		}

		json* ExpandText(std::string_view text, const std::string& key, const std::string& name, Expansion& expansion, std::string& outError)
		{
			json entities;
			if (!ParseRecords(text, name, entities, outError))
				return nullptr;
			expansion.Chain.emplace_back(key, name);
			const bool expanded = ExpandRecords(entities, name, expansion, outError);
			expansion.Chain.pop_back();
			return expanded ? &expansion.Files.insert_or_assign(key, std::move(entities)).first->second : nullptr;
		}

		json* ReadExpanded(const std::filesystem::path& path, const std::string& name, Expansion& expansion, std::string& outError)
		{
			const std::string key = Prefab::FileKey(path);
			expansion.Report.Files.insert(key);
			if (const auto known = expansion.Files.find(key); known != expansion.Files.end())
				return &known->second;
			const auto& chain = expansion.Chain;
			if (std::any_of(chain.begin(), chain.end(), [&](const auto& file) { return file.first == key; }))
			{
				std::vector<std::string> names;
				names.reserve(chain.size());
				for (const auto& file : chain)
					names.push_back(file.second);
				outError = fmt::format("prefab cycle: {} -> {}", fmt::join(names, " -> "), name);
				return nullptr;
			}
			if (chain.size() >= Prefab::MaxNestingDepth)
			{
				outError = fmt::format("prefab '{}' is nested deeper than {} levels", name, Prefab::MaxNestingDepth);
				return nullptr;
			}
			std::string readError;
			const auto text = FileSystem::ReadTextFile(path, &readError);
			if (!text)
			{
				outError = "cannot read prefab '" + name + "' (" + readError + ")";
				return nullptr;
			}
			return ExpandText(*text, key, name, expansion, outError);
		}

		// A nested instance inside a tree about to be saved: its entities, by their IDs in its prefab.
		struct NestedTree
		{
			uint64_t Root = 0;
			IdMap Members;
		};

		// The outermost nested instances of serialized records, leaving out those in skipped (and what they hold is
		// looked at on its own). One without tracking is matched to its prefab by name, or reported when that cannot be read.
		std::vector<NestedTree> FindNestedTrees(Scene& scene, const json& entities, IdSet skipped, std::vector<std::string>& outWarnings)
		{
			std::vector<NestedTree> trees;
			for (const json& record : entities)
			{
				const uint64_t id = RecordId(record);
				const Entity entity = EntityOf(scene, id);
				const auto* link = entity ? entity.TryGetComponent<PrefabComponent>() : nullptr;
				if (skipped.contains(id) || !link || link->Prefab.empty())
					continue;
				NestedTree tree{ id, {} };
				if (!link->Members.empty())
				{
					const IdSet members = MemberIds(*link);
					for (const auto& [key, value] : link->Members.items())
						if (IsId(value) && IsOwnMember(entity, EntityOf(scene, value.get<uint64_t>()), members))
							tree.Members[ParseKey(key)] = value.get<uint64_t>();
				}
				else
				{
					json source;
					std::string error;
					if (Prefab::ReadSource(Project::ResolvePath(link->Prefab), source, error))
						tree.Members = MatchByName(source, entity);
					else
						outWarnings.push_back(fmt::format("'{}' keeps its entities' IDs: {}", entity.GetName(), error));
				}
				for (const auto& [key, member] : tree.Members)
					if (member != id)
						skipped.insert(member);
				trees.push_back(std::move(tree));
			}
			return trees;
		}

		void RenumberTree(json& entities, const IdMap& ids)
		{
			for (json& record : entities)
			{
				record["ID"] = ids.at(RecordId(record));
				RenumberReferences(record, ids);
			}
		}
	}

	std::string Prefab::FileKey(const std::filesystem::path& absolutePath)
	{
		return FileSystem::PathToGenericUtf8(absolutePath.lexically_normal());
	}

	bool Prefab::ReadFile(const std::filesystem::path& path, json& outEntities, std::string& outError, ReadReport* outReport)
	{
		ReadReport report;
		Expansion expansion{ outReport ? *outReport : report };
		json* entities = ReadExpanded(path, FileSystem::PathToGenericUtf8(path), expansion, outError);
		if (!entities)
			return false;
		outEntities = std::move(*entities);
		return true;
	}

	bool Prefab::ReadText(std::string_view text, const std::string& name, json& outEntities, std::string& outError, ReadReport* outReport)
	{
		ReadReport report;
		Expansion expansion{ outReport ? *outReport : report };
		json* entities = ExpandText(text, name, name, expansion, outError);
		if (!entities)
			return false;
		outEntities = std::move(*entities);
		return true;
	}

	bool Prefab::ReadSource(const std::filesystem::path& path, json& outEntities, std::string& outError, ReadReport* outReport)
	{
		return ReadFile(path, outEntities, outError, outReport) && Normalize(outEntities, FileSystem::PathToGenericUtf8(path), outError);
	}

	bool Prefab::Normalize(json& entities, const std::string& name, std::string& outError)
	{
		// Deserializing keeps the source IDs, so writing the scratch entities back yields normalized source records.
		Scene scratch("Prefab source");
		if (!SceneSerializer::DeserializeScene(scratch, SceneSerializer::SceneDocument(entities), outError))
		{
			outError = "prefab '" + name + "': " + outError;
			return false;
		}
		IdMap identity;
		for (const json& entity : entities)
			identity[RecordId(entity)] = RecordId(entity);
		const Entity root = EntityOf(scratch, RecordId(entities[0]));
		const std::string base = LinkPath(entities[0]);
		json normalized = json::array();
		for (const json& entity : entities)
			normalized.push_back(SerializeMember(EntityOf(scratch, RecordId(entity)), root, identity, {}));
		if (!base.empty())
			normalized[0]["Components"][LinkComponent] = json{ { LinkComponent, base } };
		entities = std::move(normalized);
		return true;
	}

	bool Prefab::ToFileEntities(json entities, const std::filesystem::path& path, bool variant, json& outEntities, std::string& outError)
	{
		// Tracking belongs to scenes: a record's link names its prefab and nothing else.
		for (json& record : entities)
			if (auto link = record["Components"].find(LinkComponent); link != record["Components"].end())
				*link = json{ { LinkComponent, link->value(LinkComponent, std::string()) } };
		if (!variant)
			entities[0]["Components"].erase(LinkComponent);

		struct Instance
		{
			size_t Index = 0;
			std::string Prefab;
			// Keyed by the entities' IDs in this file.
			json Expected = json::object();
		};
		std::vector<Instance> instances;
		const std::string target = FileKey(path);
		for (size_t i = 0; i < entities.size(); ++i)
		{
			const std::string prefab = LinkPath(entities[i]);
			if (prefab.empty())
				continue;
			json inner;
			ReadReport report;
			if (!ReadSource(Project::ResolvePath(prefab), inner, outError, &report))
				return false;
			if (report.Files.contains(target))
			{
				outError = fmt::format("'{}' is nested in '{}' and holds that prefab itself", prefab, FileSystem::PathToGenericUtf8(path));
				return false;
			}
			NestRecords(inner, RecordId(entities[i]), entities[i]["Parent"].get<uint64_t>(), prefab);
			Instance instance{ i, prefab };
			for (json& record : inner)
			{
				const std::string key = KeyOf(RecordId(record));
				instance.Expected[key] = std::move(record);
			}
			instances.push_back(std::move(instance));
		}
		// What the instances hold below their roots: that includes the roots of instances nested deeper, which are part of
		// what the outer one's overrides describe.
		std::unordered_set<std::string> held;
		for (const Instance& instance : instances)
			for (const auto& [key, record] : instance.Expected.items())
				if (key != KeyOf(RecordId(entities[instance.Index])))
					held.insert(key);

		json current = json::object();
		for (const json& record : entities)
			current[KeyOf(RecordId(record))] = record;
		std::unordered_map<size_t, const Instance*> byIndex;
		for (const Instance& instance : instances)
			byIndex[instance.Index] = &instance;

		outEntities = json::array();
		for (size_t i = 0; i < entities.size(); ++i)
		{
			const json& record = entities[i];
			const std::string key = KeyOf(RecordId(record));
			if (held.contains(key))
				continue;
			const auto found = byIndex.find(i);
			if (found == byIndex.end())
			{
				outEntities.push_back(record);
				continue;
			}
			const Instance& instance = *found->second;
			json mine = json::object();
			for (const auto& [id, expected] : instance.Expected.items())
				if (const auto it = current.find(id); it != current.end())
					mine[id] = *it;
			json overrides = DiffObjects(instance.Expected, mine);
			// The root's place in this prefab is its own, as in a scene: moving the nested prefab's root never moves it.
			if (i != 0)
				if (const auto transform = record["Components"].find("Transform"); transform != record["Components"].end())
					for (const char* field : PlacementFields)
						if (const auto value = transform->find(field); value != transform->end())
							overrides[key]["Components"]["Transform"][field] = *value;
			json link = { { LinkComponent, instance.Prefab } };
			if (!overrides.empty())
				link[OverridesKey] = std::move(overrides);
			outEntities.push_back({ { "ID", record["ID"] }, { "Parent", record["Parent"] }, { "Components", { { LinkComponent, std::move(link) } } } });
		}
		return true;
	}

	bool Prefab::SaveTree(Entity root, const std::filesystem::path& path, bool variant, std::vector<std::string>& outWarnings, std::string& outError)
	{
		json entities = SceneSerializer::SerializeEntityTree(root);
		IdMap fileIds;
		for (const json& record : entities)
			fileIds[RecordId(record)] = RecordId(record);
		IdSet skipped;
		if (!variant)
			skipped.insert(IdOf(root));
		for (const NestedTree& tree : FindNestedTrees(*root.GetScene(), entities, skipped, outWarnings))
			for (const auto& [source, entity] : tree.Members)
				if (entity != tree.Root)
					fileIds[entity] = NestedId(tree.Root, source);
		RenumberTree(entities, fileIds);
		return SceneSerializer::SavePrefabEntities(std::move(entities), path, outError, variant);
	}

	bool Prefab::CreateVariant(const std::string& basePath, const std::filesystem::path& path, std::string& outError)
	{
		json entities;
		if (!ReadSource(Project::ResolvePath(basePath), entities, outError))
			return false;
		UUID root;
		while (!root.IsValid())
			root = UUID();
		NestRecords(entities, static_cast<uint64_t>(root), 0, NormalizePath(basePath));
		return SceneSerializer::SavePrefabEntities(std::move(entities), path, outError, true);
	}

	void Prefab::TrackNestedInstances(Scene& scene, std::vector<std::string>& outWarnings)
	{
		std::function<void(Entity)> visit = [&](Entity entity) {
			if (!entity.HasComponent<PrefabComponent>())
			{
				for (Entity child : entity.GetChildren())
					visit(child);
				return;
			}
			auto& link = entity.GetComponent<PrefabComponent>();
			json source;
			if (std::string error; !ReadSource(Project::ResolvePath(link.Prefab), source, error))
			{
				outWarnings.push_back(fmt::format("'{}' is not tracked: {}", entity.GetName(), error));
				return;
			}
			const uint64_t sourceRoot = RecordId(source[0]);
			for (const json& record : source)
				link.Members[KeyOf(RecordId(record))] = RecordId(record) == sourceRoot ? IdOf(entity) : NestedId(IdOf(entity), RecordId(record));
			link.Baseline = ToRecords(source).ByKey;
		};
		for (Entity root : scene.GetRootEntities())
			visit(root);
	}

	void Prefab::AdoptNestedInstances(Entity root, std::vector<std::string>& outWarnings)
	{
		std::function<void(Entity)> visit = [&](Entity entity) {
			const auto* link = entity.TryGetComponent<PrefabComponent>();
			if (!link)
			{
				for (Entity child : entity.GetChildren())
					visit(child);
				return;
			}
			if (!link->Members.empty())
				return;
			json source;
			std::string error;
			if (!ReadSource(Project::ResolvePath(link->Prefab), source, error) || Failed(Refresh(entity, source, RefreshMode::Merge, error)))
				outWarnings.push_back(fmt::format("'{}' stays untracked: {}", entity.GetName(), error));
		};
		for (Entity child : root.GetChildren())
			visit(child);
	}

	void Prefab::AssignSourceIds(Entity root, const std::string& prefabPath, json& entities, bool& inOutVariant, std::vector<std::string>& outWarnings)
	{
		Scene& scene = *root.GetScene();
		IdMap sourceOf;
		IdSet taken;
		// An instance of this very file keeps its members' source IDs.
		const auto* link = root.TryGetComponent<PrefabComponent>();
		const bool isInstance = link && NormalizePath(link->Prefab) == NormalizePath(prefabPath);
		if (isInstance)
		{
			const IdSet members = MemberIds(*link);
			for (const auto& [key, value] : link->Members.items())
				if (IsId(value) && IsOwnMember(root, EntityOf(scene, value.get<uint64_t>()), members))
				{
					sourceOf[value.get<uint64_t>()] = ParseKey(key);
					taken.insert(ParseKey(key));
				}
		}
		const auto absolute = Project::ResolvePath(prefabPath);
		std::error_code exists;
		json existing;
		bool readExisting = false;
		if (std::filesystem::exists(absolute, exists))
		{
			std::string error;
			readExisting = ReadFile(absolute, existing, error);
			if (!readExisting)
				outWarnings.push_back("the existing prefab's entity IDs could not be kept (" + error + "); its instances will need to be instantiated again");
		}
		// Saving an instance of a variant writes the variant.
		if (isInstance && readExisting && !LinkPath(existing[0]).empty())
		{
			inOutVariant = true;
			entities[0]["Components"][LinkComponent] = existing[0]["Components"][LinkComponent];
		}
		// Nested instances take the IDs their root derives, whatever matches below.
		IdSet skipped;
		for (const auto& [entity, source] : sourceOf)
			skipped.insert(entity);
		if (!inOutVariant)
			skipped.insert(IdOf(root));
		const std::vector<NestedTree> trees = FindNestedTrees(scene, entities, skipped, outWarnings);
		IdSet derived;
		for (const NestedTree& tree : trees)
			for (const auto& [source, entity] : tree.Members)
				if (entity != tree.Root)
					derived.insert(entity);
		// The rest match the existing file by name (untracked instances and unrelated trees entirely).
		if (readExisting)
			for (const auto& [source, entity] : MatchByName(existing, root))
				if (!sourceOf.contains(entity) && !taken.contains(source) && !derived.contains(entity))
				{
					sourceOf[entity] = source;
					taken.insert(source);
				}
		for (const json& record : entities)
			if (!sourceOf.contains(RecordId(record)) && !derived.contains(RecordId(record)))
				sourceOf[RecordId(record)] = ClaimNewId(scene, taken);
		for (const NestedTree& tree : trees)
			for (const auto& [source, entity] : tree.Members)
				if (entity != tree.Root)
					sourceOf[entity] = NestedId(sourceOf.at(tree.Root), source);
		RenumberTree(entities, sourceOf);
	}

	Entity Prefab::EnclosingInstance(Entity entity)
	{
		for (Entity ancestor = entity.GetParent(); ancestor; ancestor = ancestor.GetParent())
			if (const auto* link = ancestor.TryGetComponent<PrefabComponent>(); link && !link->Members.empty())
				if (const IdSet members = MemberIds(*link); members.contains(IdOf(entity)) && IsOwnMember(ancestor, entity, members))
					return ancestor;
		return {};
	}

	bool Prefab::IsTracked(const PrefabComponent& link)
	{
		return !link.Members.empty() && !link.Baseline.empty();
	}

	std::string Prefab::NormalizePath(const std::string& path)
	{
		// Unresolved asset keys ("asset-uuid://...") are opaque: normalizing would fold their "//".
		if (path.empty() || AssetRegistry::IsUnresolvedKey(path))
			return path;
		return FileSystem::NormalizeGeneric(path);
	}

	void Prefab::Link(const std::vector<Entity>& instances, const json& sourceEntities, const std::string& prefabPath)
	{
		BS_CORE_ASSERT(!instances.empty() && instances.size() == sourceEntities.size(), "every source entity needs its instance");
		Entity root = instances.front();
		// A tree prefab.create links may hold instances that tracked themselves until now.
		for (size_t i = 1; i < instances.size(); ++i)
			if (auto* nested = instances[i].TryGetComponent<PrefabComponent>())
			{
				nested->Members = json::object();
				nested->Baseline = json::object();
			}
		PrefabComponent link;
		link.Prefab = NormalizePath(prefabPath);
		IdMap toSource;
		IdSet sourceKeys;
		for (size_t i = 0; i < instances.size(); ++i)
		{
			const uint64_t source = RecordId(sourceEntities[i]);
			link.Members[KeyOf(source)] = IdOf(instances[i]);
			toSource[IdOf(instances[i])] = source;
			sourceKeys.insert(source);
		}
		if (!root.GetScene()->IsRunning())
			for (size_t i = 0; i < instances.size(); ++i)
				link.Baseline[KeyOf(RecordId(sourceEntities[i]))] = SerializeMember(instances[i], root, toSource, sourceKeys);
		root.AddOrReplaceComponent<PrefabComponent>(std::move(link));
	}

	bool Prefab::NormalizeTracking(PrefabComponent& link, std::string& outError)
	{
		// Lua hands 64-bit IDs back as signed integers (UUIDs of 2^63 and above become negative).
		const auto unsign = [](json& value) {
			if (value.is_number_integer() && !value.is_number_unsigned())
				value = static_cast<uint64_t>(value.get<int64_t>());
		};
		for (auto& value : link.Members)
			unsign(value);
		for (auto& record : link.Baseline)
		{
			if (!record.is_object())
				continue;
			if (record.contains("ID"))
				unsign(record["ID"]);
			// ForEachReference would insert a missing Parent; malformed records are left for Validate to reject.
			if (record.contains("Parent"))
				ForEachReference(record, [&](json& value, bool) { unsign(value); });
		}
		// Baselines compare with serialized instance components, which hold asset paths.
		const auto& project = Project::GetActive();
		return ForEachBaselineAssetValue(link.Baseline, [&](json& value, const json& record, const std::string& label) {
			if (!value.is_object())
				return true;
			std::string path, error;
			if (!project)
				error = "UUID asset references require an active project";
			else if (project->GetAssets().Decode(value, path, error, true))
			{
				value = path;
				return true;
			}
			outError = "Baseline '" + NameOf(record) + "' " + label + ": " + error;
			return false;
		});
	}

	bool Prefab::ForEachBaselineAssetValue(json& baseline, const std::function<bool(json& value, const json& record, const std::string& label)>& visit)
	{
		if (!baseline.is_object())
			return true;
		for (auto& record : baseline)
		{
			if (!record.is_object() || !record.contains("Components") || !record["Components"].is_object())
				continue;
			for (const auto& info : ComponentRegistry::GetAll())
			{
				auto component = record["Components"].find(info.Name);
				if (component == record["Components"].end() || !component->is_object())
					continue;
				for (const auto& field : info.AssetFields)
				{
					bool stopped = false;
					ForEachAssetValue(*component, field, [&](json& value, const std::string& label) {
						if (!stopped && !visit(value, record, info.Name + "." + label))
							stopped = true;
					});
					if (stopped)
						return false;
				}
			}
		}
		return true;
	}

	json Prefab::ComputeOverrides(Entity root)
	{
		const auto& link = root.GetComponent<PrefabComponent>();
		if (link.Baseline.empty())
			return json::object();
		return OverridesOf(link, CaptureInstance(root, link, SourceKeys(link, nullptr)).ByKey, RootKey(link, root));
	}

	std::vector<std::string> Prefab::SplitOverridePath(const std::string& path)
	{
		std::vector<std::string> tokens;
		for (size_t start = 1; start <= path.size();)
		{
			const size_t end = std::min(path.find('/', start), path.size());
			std::string token = path.substr(start, end - start);
			for (size_t at = token.find('~'); at != std::string::npos && at + 1 < token.size(); at = token.find('~', at + 1))
				token.replace(at, 2, token[at + 1] == '1' ? "/" : "~");
			tokens.push_back(std::move(token));
			start = end + 1;
		}
		return tokens;
	}

	bool Prefab::IsOutdated(const PrefabComponent& link, const json& sourceEntities)
	{
		return IsTracked(link) && !Equivalent(link.Baseline, ToRecords(sourceEntities).ByKey);
	}

	Prefab::RefreshResult Prefab::Refresh(Entity root, const json& sourceEntities, RefreshMode mode, std::string& outError)
	{
		return MergeInto(root, sourceEntities, mode == RefreshMode::RevertAll ? MergeKind::RevertAll : MergeKind::Merge, {}, outError);
	}

	Prefab::RefreshResult Prefab::RevertOverride(Entity root, const json& sourceEntities, const std::string& path, std::string& outError)
	{
		return MergeInto(root, sourceEntities, MergeKind::RevertField, path, outError);
	}

	bool Prefab::Apply(Entity root, const std::filesystem::path& path, std::vector<std::string>& outWarnings, std::string& outError)
	{
		if (root.GetScene()->IsRunning())
		{
			outError = "stop Play or Simulate before applying prefab overrides";
			return false;
		}
		json source;
		if (!ReadSource(path, source, outError))
			return false;
		auto prepared = PrepareMerge(root, source, outError);
		if (!prepared)
			return false;
		MergeInputs& in = *prepared;
		json merged = Merge(in.Link.Baseline, in.Current.ByKey, in.Next.ByKey);
		KeepPlacement(merged, in.Next.ByKey, in.RootKey);
		// Adoption matches by name, so a source entity it could not match was renamed or never there, not deleted: the
		// prefab keeps it.
		if (in.Adopted)
			for (const std::string& key : in.Next.Order)
				if (!in.Current.ByKey.contains(key) && !merged.contains(key))
				{
					merged[key] = in.Next.ByKey[key];
					outWarnings.push_back(fmt::format("'{}' has no entity of that name in this instance (it was not tracked before); the prefab keeps it", NameOf(in.Next.ByKey[key])));
				}
		const auto inPrefab = [&](const json& id) { return IsId(id) && merged.contains(KeyOf(id.get<uint64_t>())); };
		for (auto& [key, record] : merged.items())
		{
			// Members whose parent will not be in the prefab (under an entity the instance added) go back under their
			// source parent, or the root.
			if (key != in.RootKey && !inPrefab(record["Parent"]))
			{
				const auto sourceRecord = in.Next.ByKey.find(key);
				const bool sourceParentKept = sourceRecord != in.Next.ByKey.end() && inPrefab((*sourceRecord)["Parent"]);
				record["Parent"] = sourceParentKept ? (*sourceRecord)["Parent"] : json(ParseKey(in.RootKey));
			}
			// A reference to an entity outside the prefab (one the instance added, or a scene entity) means nothing in
			// other scenes: the prefab keeps its value there.
			for (auto& [component, fields] : record["Components"].items())
			{
				const auto* info = ComponentRegistry::Find(component);
				if (!info || !fields.is_object())
					continue;
				for (const auto& field : info->EntityFields)
				{
					const auto value = fields.find(field);
					if (value == fields.end() || *value == 0 || inPrefab(*value))
						continue;
					const json::json_pointer at("/" + PointerToken(key) + "/Components/" + PointerToken(component) + "/" + PointerToken(field));
					const json previous = in.Next.ByKey.contains(at) ? in.Next.ByKey[at] : json(0);
					if (*value == previous)
						continue;
					outWarnings.push_back(fmt::format("'{}' {}.{} references an entity outside the prefab; the prefab keeps its value", NameOf(record), component, field));
					*value = previous;
				}
			}
		}
		Scene scratch("Apply prefab overrides");
		if (!SceneSerializer::DeserializeScene(scratch, SceneSerializer::SceneDocument(InOrder(merged, { &in.Next.Order, &in.Current.Order })), outError))
			return false;
		json entities = SceneSerializer::SerializeEntityTree(EntityOf(scratch, ParseKey(in.RootKey)));
		// A variant stays one: instances keep their own link where the file has its base.
		const std::string base = LinkPath(source[0]);
		if (!base.empty())
			entities[0]["Components"][LinkComponent] = json{ { LinkComponent, base } };
		return SceneSerializer::SavePrefabEntities(std::move(entities), path, outError, !base.empty());
	}

}
