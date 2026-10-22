#include "Basalt/Scene/Prefab.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Scene/ComponentRegistry.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/Scene/SceneSerializer.h"

#include <glm/gtc/quaternion.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
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

		std::string NormalPath(const std::string& path)
		{
			return FileSystem::PathToGenericUtf8(FileSystem::PathFromUtf8(path).lexically_normal());
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

		// Source space to instance space. A reference to a source entity the instance does not have becomes the root
		// (for a parent) or no reference: it must never resolve to an unrelated entity that shares the source ID.
		void ToInstanceSpace(json& record, const IdMap& toInstance, const IdSet& sourceKeys, uint64_t rootId)
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
				else if (sourceKeys.contains(id))
					value = isParent ? rootId : 0;
			});
		}

		// A member as a source-space record: what a prefab file holds for it, as the serializer writes it.
		json SerializeMember(Entity entity, Entity root, const IdMap& toSource, const IdSet& sourceKeys)
		{
			json components = json::object();
			for (const auto& info : ComponentRegistry::GetAll())
				if (info.Name != "Tag" && info.Name != "Prefab" && info.Has(entity))
					components[info.Name] = info.Serialize(entity);
			const Entity parent = entity.GetParent();
			json record = { { "ID", IdOf(entity) }, { "Name", entity.GetName() }, { "Parent", entity == root || !parent ? 0 : IdOf(parent) }, { "Components", std::move(components) } };
			ToSourceSpace(record, toSource, sourceKeys);
			return record;
		}

		// A Members value belongs to this instance unless the entity is another instance's root or another instance
		// lists it as its own (a hand-edited scene): a merge never overwrites or destroys another instance's entities.
		bool IsOwnMember(Entity root, Entity entity)
		{
			if (!entity)
				return false;
			if (entity == root)
				return true;
			if (entity.HasComponent<PrefabComponent>())
				return false;
			for (Entity ancestor = entity.GetParent(); ancestor; ancestor = ancestor.GetParent())
			{
				if (ancestor == root)
					return true;
				if (const auto* other = ancestor.TryGetComponent<PrefabComponent>())
					for (const auto& value : other->Members)
						if (IsId(value) && value.get<uint64_t>() == IdOf(entity))
							return false;
			}
			return true;
		}

		std::string RootKey(const PrefabComponent& link, Entity root)
		{
			for (const auto& [key, value] : link.Members.items())
				if (IsId(value) && value.get<uint64_t>() == IdOf(root))
					return key;
			return {};
		}

		// Records keyed by source ID. Order is a sibling-order hint: the source's order, or depth first from the root
		// for an instance. It may omit records (members moved out of the instance's tree), which then come last.
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
			Records records;
			std::function<void(Entity)> visit = [&](Entity entity) {
				if (const auto it = toSource.find(IdOf(entity)); it != toSource.end() && IsOwnMember(root, entity))
					if (const std::string key = KeyOf(it->second); !records.ByKey.contains(key))
					{
						records.Order.push_back(key);
						records.ByKey[key] = SerializeMember(entity, root, toSource, sourceKeys);
					}
				for (Entity child : entity.GetChildren())
					visit(child);
			};
			visit(root);
			// Members moved out of the instance's tree still belong to it.
			for (const auto& [instance, source] : toSource)
				if (!records.ByKey.contains(KeyOf(source)))
					if (Entity entity = EntityOf(*root.GetScene(), instance); IsOwnMember(root, entity))
						records.ByKey[KeyOf(source)] = SerializeMember(entity, root, toSource, sourceKeys);
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
				const double x = a.get<double>();
				const double y = b.get<double>();
				return std::abs(x - y) <= 1e-6 * std::max({ 1.0, std::abs(x), std::abs(y) });
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

		// Matches source records to an entity tree: the roots, then children by name in order (each entity once).
		// Returns source ID -> entity UUID. Other instances' roots never match.
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
					for (Entity candidate : entity.GetChildren())
						if (!used.contains(IdOf(candidate)) && candidate.GetName() == name->get_ref<const std::string&>() && !candidate.HasComponent<PrefabComponent>())
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

		json SceneDocument(json entities)
		{
			return { { "Format", "BasaltScene" }, { "Version", SceneSerializer::FormatVersion }, { "Entities", std::move(entities) } };
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
			if (in.Link.Members.empty() || in.Link.Baseline.empty())
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

		// The merged records in source space, or nothing when the field to revert is not an override.
		std::optional<json> MergedRecords(MergeInputs& in, MergeKind kind, const std::string& field, std::string& outError)
		{
			json merged;
			if (kind == MergeKind::RevertAll)
				merged = in.Next.ByKey;
			else
			{
				if (kind == MergeKind::RevertField)
				{
					if (!OverridesOf(in.Link, in.Current.ByKey, in.RootKey).contains(field))
					{
						outError = "'" + field + "' is not an override of this instance (prefab.overrides lists them)";
						return std::nullopt;
					}
					// The path came from CollectOverrides, so it parses and its parent exists. Reverting restores the
					// baseline there; the merge then takes the source's value.
					const json::json_pointer pointer(field);
					if (in.Link.Baseline.contains(pointer))
						in.Current.ByKey[pointer] = in.Link.Baseline[pointer];
					else
						in.Current.ByKey[pointer.parent_pointer()].erase(pointer.back());
				}
				merged = Merge(in.Link.Baseline, in.Current.ByKey, in.Next.ByKey);
			}
			KeepPlacement(merged, in.Current.ByKey, in.RootKey);
			return merged;
		}

		// Members keep their instance IDs, including IDs of entities the instance deleted, so Revert recreates them
		// with the same IDs. Entities new to the instance get fresh ones. Keys that left both the source and the
		// instance stop being tracked.
		IdMap AssignInstanceIds(Scene& scene, Entity root, const json& merged, const Records& next, PrefabComponent& link)
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
				if (const Entity existing = EntityOf(scene, id); !tracked || (existing && !IsOwnMember(root, existing)))
				{
					id = ClaimNewId(scene, taken);
					link.Members[key] = id;
				}
				toInstance[ParseKey(key)] = id;
			}
			for (auto it = link.Members.begin(); it != link.Members.end();)
			{
				if (!merged.contains(it.key()) && !next.ByKey.contains(it.key()))
					it = link.Members.erase(it);
				else
					++it;
			}
			return toInstance;
		}

		// The instance-space result of a merge, checked against the scene before anything changes.
		struct MergePlan
		{
			// Merged member records in sibling order, in instance space; the root's parent is its scene parent.
			std::vector<json> Records;
			IdSet Members;
			// Members before the merge that still exist.
			IdSet Previous;
			// The final parent of every member and of each orphan.
			IdMap Parents;
			// Entities the instance added under a kept member, by member, in their order.
			std::unordered_map<uint64_t, std::vector<Entity>> Locals;
			// Entities the instance added under a member the merge removes; they move to the root.
			std::vector<Entity> Orphans;
			// Members the merge removes inside the instance: destroyed (members moved elsewhere stay, untracked).
			std::vector<Entity> Destroyed;
			// Members whose parent changes, parents before children.
			std::vector<uint64_t> Reparented;
		};

		bool PlanMerge(Scene& scene, Entity root, const json& merged, const MergeInputs& in, const IdMap& toInstance, MergePlan& plan, std::string& outError)
		{
			const uint64_t rootId = IdOf(root);
			for (const auto& [key, value] : in.Link.Members.items())
				if (IsId(value) && IsOwnMember(root, EntityOf(scene, value.get<uint64_t>())))
					plan.Previous.insert(value.get<uint64_t>());
			plan.Records = InOrder(merged, { &in.Next.Order, &in.Current.Order });
			for (json& record : plan.Records)
			{
				ToInstanceSpace(record, toInstance, in.SourceKeys, rootId);
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
			if (!SceneSerializer::DeserializeScene(scratch, SceneDocument(std::move(isolated)), outError))
				return false;

			std::unordered_map<uint64_t, uint32_t> heights;
			for (uint64_t id : plan.Previous)
			{
				const Entity member = EntityOf(scene, id);
				const bool kept = plan.Members.contains(id);
				const bool inside = scene.IsDescendantOf(member, root);
				if (!kept && inside)
					plan.Destroyed.push_back(member);
				for (Entity child : member.GetChildren())
				{
					if (plan.Previous.contains(IdOf(child)) || plan.Members.contains(IdOf(child)))
						continue;
					if (kept)
					{
						plan.Locals[id].push_back(child);
						heights[id] = std::max(heights[id], scene.GetSubtreeHeight(child) + 1);
					}
					else if (inside)
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

		// Applies a validated plan. Only internal errors fail here, and the scene is then partly merged.
		bool ApplyPlan(Scene& scene, Entity root, const MergePlan& plan, std::string& outError)
		{
			const uint64_t rootId = IdOf(root);
			bool complete = true;
			auto parent = [&](Entity child, Entity newParent) {
				if (!scene.SetParent(child, newParent, false))
				{
					outError = "could not parent '" + child.GetName() + "' to '" + (newParent ? newParent.GetName() : std::string("the scene root")) + "'";
					complete = false;
				}
			};
			// Detach first so destroying a removed member keeps the children it had.
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
				for (const auto& info : ComponentRegistry::GetAll())
					if (!info.IsCore && info.Name != "Prefab" && info.Has(entity) && !record["Components"].contains(info.Name))
						info.Remove(entity);
				if (!SceneSerializer::DeserializeEntityComponents(entity, record, outError))
				{
					outError = fmt::format("'{}': {}", entity.GetName(), outError);
					complete = false;
				}
			}
			for (uint64_t id : plan.Reparented)
				parent(EntityOf(scene, id), EntityOf(scene, plan.Parents.at(id)));
			// Parenting to the same parent again appends, which settles sibling order: members in source order, then the
			// entities the instance added, in theirs. The hierarchy does not change, so this cannot fail.
			for (const json& record : plan.Records)
				if (const uint64_t id = RecordId(record); id != rootId)
					parent(EntityOf(scene, id), EntityOf(scene, plan.Parents.at(id)));
			for (const json& record : plan.Records)
				if (const auto locals = plan.Locals.find(RecordId(record)); locals != plan.Locals.end())
					for (Entity local : locals->second)
						parent(local, EntityOf(scene, RecordId(record)));
			for (Entity orphan : plan.Orphans)
				parent(orphan, root);
			return complete;
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
			PrefabComponent link = in.Link;
			const IdMap toInstance = AssignInstanceIds(scene, root, merged, in.Next, link);
			MergePlan plan;
			if (!PlanMerge(scene, root, merged, in, toInstance, plan, outError))
				return Prefab::RefreshResult::Rejected;
			const bool complete = ApplyPlan(scene, root, plan, outError);
			link.Baseline = in.Next.ByKey;
			root.AddOrReplaceComponent<PrefabComponent>(std::move(link));
			if (!complete)
			{
				BS_CORE_ERROR("Prefab merge into '{}' failed after validation: {}", root.GetName(), outError);
				return Prefab::RefreshResult::Partial;
			}
			return Prefab::RefreshResult::Merged;
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
		if (const auto version = data.find("Version"); version != data.end() && (!version->is_number_integer() || version->get<int64_t>() > SceneSerializer::FormatVersion))
		{
			outError = "prefab '" + name + "' has format version " + version->dump() + ", newer than the supported version " + std::to_string(SceneSerializer::FormatVersion);
			return false;
		}
		json& entities = data["Entities"];
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
			entity["Parent"] = isRoot ? json(0) : json(*parent);
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
		if (!SceneSerializer::DeserializeScene(scratch, SceneDocument(entities), outError))
		{
			outError = "prefab '" + FileSystem::PathToGenericUtf8(path) + "': " + outError;
			return false;
		}
		IdMap identity;
		for (const json& entity : entities)
			identity[RecordId(entity)] = RecordId(entity);
		const Entity root = EntityOf(scratch, RecordId(entities[0]));
		outEntities = json::array();
		for (const json& entity : entities)
			outEntities.push_back(SerializeMember(EntityOf(scratch, RecordId(entity)), root, identity, {}));
		return true;
	}

	void Prefab::AssignSourceIds(Entity root, const std::string& prefabPath, json& entities)
	{
		Scene& scene = *root.GetScene();
		IdMap sourceOf;
		json existing;
		std::string error;
		const auto* link = root.TryGetComponent<PrefabComponent>();
		if (link && NormalPath(link->Prefab) == NormalPath(prefabPath))
		{
			for (const auto& [key, value] : link->Members.items())
				if (IsId(value) && IsOwnMember(root, EntityOf(scene, value.get<uint64_t>())))
					sourceOf[value.get<uint64_t>()] = ParseKey(key);
		}
		else if (ReadFile(Project::ResolvePath(prefabPath), existing, error))
		{
			for (const auto& [source, entity] : MatchByName(existing, root))
				sourceOf[entity] = source;
		}
		IdSet taken;
		for (const auto& [entity, source] : sourceOf)
			taken.insert(source);
		for (const json& record : entities)
			if (!sourceOf.contains(RecordId(record)))
				sourceOf[RecordId(record)] = ClaimNewId(scene, taken);
		for (json& record : entities)
		{
			record["ID"] = sourceOf.at(RecordId(record));
			ForEachReference(record, [&](json& value, bool) {
				if (IsId(value))
					if (const auto it = sourceOf.find(value.get<uint64_t>()); it != sourceOf.end())
						value = it->second;
			});
		}
	}

	void Prefab::Link(const std::vector<Entity>& instances, const json& sourceEntities, const std::string& prefabPath)
	{
		BS_CORE_ASSERT(!instances.empty() && instances.size() == sourceEntities.size(), "every source entity needs its instance");
		Entity root = instances.front();
		PrefabComponent link;
		link.Prefab = NormalPath(prefabPath);
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
			outError = "Baseline '" + record.value("Name", std::string("<unnamed>")) + "' " + label + ": " + error;
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
		return !link.Members.empty() && !link.Baseline.empty() && !Equivalent(link.Baseline, ToRecords(sourceEntities).ByKey);
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
		for (auto& [key, record] : merged.items())
		{
			// Members the instance moved outside its tree go back under their source parent, or the root.
			const json& parentValue = record["Parent"];
			if (key != in.RootKey && !(IsId(parentValue) && merged.contains(KeyOf(parentValue.get<uint64_t>()))))
			{
				const auto sourceRecord = in.Next.ByKey.find(key);
				const bool sourceParentKept = sourceRecord != in.Next.ByKey.end() && merged.contains(KeyOf((*sourceRecord)["Parent"].get<uint64_t>()));
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
					if (value == fields.end() || (IsId(*value) && (*value == 0 || merged.contains(KeyOf(value->get<uint64_t>())))))
						continue;
					const json::json_pointer at("/" + PointerToken(key) + "/Components/" + PointerToken(component) + "/" + PointerToken(field));
					const json previous = in.Next.ByKey.contains(at) ? in.Next.ByKey[at] : json(0);
					if (*value == previous)
						continue;
					outWarnings.push_back(fmt::format("'{}' {}.{} references an entity outside the prefab; the prefab keeps its value", record.value("Name", key), component, field));
					*value = previous;
				}
			}
		}
		Scene scratch("Apply prefab overrides");
		if (!SceneSerializer::DeserializeScene(scratch, SceneDocument(InOrder(merged, { &in.Next.Order, &in.Current.Order })), outError))
			return false;
		return SceneSerializer::SavePrefab(EntityOf(scratch, ParseKey(in.RootKey)), path, outError);
	}

}
