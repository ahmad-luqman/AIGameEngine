#include "Basalt/Scene/SceneSerializer.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Scene/ComponentRegistry.h"
#include "Basalt/Scene/Scene.h"

#include <ranges>
#include <set>
#include <unordered_map>

namespace Basalt {

	namespace {

		using json = nlohmann::json;

		constexpr const char* s_TonemapperNames[] = { "None", "Reinhard", "ACES", "AgX" };

		json Vec3ToJson(const glm::vec3& v)
		{
			return json::array({ v.x, v.y, v.z });
		}

		// Strict reader for one JSON object: checks value types and ranges and reports unknown keys, so a
		// typo in a hand-written (or AI-written) file is an error instead of being silently ignored.
		class ObjectReader
		{
		public:
			ObjectReader(const json& data, std::string context)
				: m_Data(data)
				, m_Context(std::move(context))
			{
			}

			bool IsObject(std::string& outError) const
			{
				if (m_Data.is_object())
					return true;
				outError = m_Context + " must be an object";
				return false;
			}

			bool Float(const char* key, float& value, std::string& outError)
			{
				const json* element = Find(key);
				if (!element)
					return true;
				if (!element->is_number())
					return Fail(key, "must be a number", outError);
				value = element->get<float>();
				return true;
			}

			bool Bool(const char* key, bool& value, std::string& outError)
			{
				const json* element = Find(key);
				if (!element)
					return true;
				if (!element->is_boolean())
					return Fail(key, "must be true or false", outError);
				value = element->get<bool>();
				return true;
			}

			bool Integer(const char* key, int64_t& value, int64_t min, int64_t max, std::string& outError)
			{
				const json* element = Find(key);
				if (!element)
					return true;
				if (!element->is_number_integer())
					return Fail(key, "must be an integer", outError);
				const bool tooLarge = element->is_number_unsigned() && element->get<uint64_t>() > static_cast<uint64_t>(max);
				const int64_t number = tooLarge ? max : element->get<int64_t>();
				if (tooLarge || number < min || number > max)
					return Fail(key, "is out of range [" + std::to_string(min) + ", " + std::to_string(max) + "]", outError);
				value = number;
				return true;
			}

			bool Id(const char* key, uint64_t& value, std::string& outError)
			{
				const json* element = Find(key);
				if (!element)
					return true;
				if (!element->is_number_unsigned() && !(element->is_number_integer() && element->get<int64_t>() >= 0))
					return Fail(key, "must be a non-negative integer", outError);
				value = element->get<uint64_t>();
				return true;
			}

			bool String(const char* key, std::string& value, std::string& outError)
			{
				const json* element = Find(key);
				if (!element)
					return true;
				if (!element->is_string())
					return Fail(key, "must be a string", outError);
				value = element->get<std::string>();
				return true;
			}

			bool Vec3(const char* key, glm::vec3& value, std::string& outError)
			{
				const json* element = Find(key);
				if (!element)
					return true;
				if (!element->is_array() || element->size() != 3 || !(*element)[0].is_number() || !(*element)[1].is_number() || !(*element)[2].is_number())
					return Fail(key, "must be an array of 3 numbers", outError);
				value = { (*element)[0].get<float>(), (*element)[1].get<float>(), (*element)[2].get<float>() };
				return true;
			}

			// Marks a key as known and returns it (or null) without interpreting it.
			const json* Take(const char* key) { return Find(key); }

			bool Finish(std::string& outError) const
			{
				for (auto it = m_Data.begin(); it != m_Data.end(); ++it)
				{
					if (!m_Used.contains(it.key()))
					{
						outError = m_Context + ": unknown field '" + it.key() + "'";
						return false;
					}
				}
				return true;
			}

		private:
			const json* Find(const char* key)
			{
				m_Used.insert(key);
				auto it = m_Data.find(key);
				return it == m_Data.end() ? nullptr : &*it;
			}

			bool Fail(const char* key, const std::string& problem, std::string& outError) const
			{
				outError = m_Context + ": '" + key + "' " + problem;
				return false;
			}

			const json& m_Data;
			std::string m_Context;
			std::set<std::string> m_Used;
		};

		json SerializeRendererSettings(const RendererSettings& s)
		{
			return {
				{ "Exposure", s.Exposure },
				{ "Tonemapper", s_TonemapperNames[static_cast<int>(s.Tonemap)] },
				{ "SSAOEnabled", s.SSAOEnabled },
				{ "SSAORadius", s.SSAORadius },
				{ "SSAOIntensity", s.SSAOIntensity },
				{ "SSAOBias", s.SSAOBias },
				{ "ShadowsEnabled", s.ShadowsEnabled },
				{ "ShadowDistance", s.ShadowDistance },
				{ "ShadowBias", s.ShadowBias },
				{ "ShadowNormalBias", s.ShadowNormalBias },
				{ "CascadeSplitLambda", s.CascadeSplitLambda },
				{ "AmbientColor", Vec3ToJson(s.AmbientColor) },
			};
		}

		bool DeserializeRendererSettings(const json& data, RendererSettings& s, std::string& outError)
		{
			ObjectReader reader(data, "Renderer");
			if (!reader.IsObject(outError))
				return false;
			std::string tonemapper;
			if (!reader.String("Tonemapper", tonemapper, outError))
				return false;
			if (!tonemapper.empty())
			{
				bool found = false;
				for (int i = 0; i < 4; i++)
				{
					if (tonemapper == s_TonemapperNames[i])
					{
						s.Tonemap = static_cast<Tonemapper>(i);
						found = true;
					}
				}
				if (!found)
				{
					outError = "Renderer: unknown tonemapper '" + tonemapper + "' (valid: None, Reinhard, ACES, AgX)";
					return false;
				}
			}
			const bool ok = reader.Float("Exposure", s.Exposure, outError) && reader.Bool("SSAOEnabled", s.SSAOEnabled, outError) && reader.Float("SSAORadius", s.SSAORadius, outError) && reader.Float("SSAOIntensity", s.SSAOIntensity, outError) && reader.Float("SSAOBias", s.SSAOBias, outError) && reader.Bool("ShadowsEnabled", s.ShadowsEnabled, outError) && reader.Float("ShadowDistance", s.ShadowDistance, outError) && reader.Float("ShadowBias", s.ShadowBias, outError) && reader.Float("ShadowNormalBias", s.ShadowNormalBias, outError) && reader.Float("CascadeSplitLambda", s.CascadeSplitLambda, outError) && reader.Vec3("AmbientColor", s.AmbientColor, outError) && reader.Finish(outError);
			if (!ok)
				return false;
			if (s.Exposure <= 0.0f || s.ShadowDistance <= 0.0f || s.SSAORadius <= 0.0f || s.CascadeSplitLambda < 0.0f || s.CascadeSplitLambda > 1.0f)
			{
				outError = "Renderer: Exposure, ShadowDistance and SSAORadius must be positive; CascadeSplitLambda must be in [0, 1]";
				return false;
			}
			return true;
		}

		json SerializePhysicsSettings(const PhysicsSettings& s)
		{
			return { { "Gravity", Vec3ToJson(s.Gravity) }, { "FixedTimestep", s.FixedTimestep }, { "MaxStepsPerFrame", s.MaxStepsPerFrame } };
		}

		bool DeserializePhysicsSettings(const json& data, PhysicsSettings& s, std::string& outError)
		{
			ObjectReader reader(data, "Physics");
			if (!reader.IsObject(outError))
				return false;
			int64_t maxSteps = s.MaxStepsPerFrame;
			if (!reader.Vec3("Gravity", s.Gravity, outError) || !reader.Float("FixedTimestep", s.FixedTimestep, outError) || !reader.Integer("MaxStepsPerFrame", maxSteps, 1, 64, outError) || !reader.Finish(outError))
				return false;
			s.MaxStepsPerFrame = static_cast<uint32_t>(maxSteps);
			if (s.FixedTimestep <= 0.0f || s.FixedTimestep > 0.25f)
			{
				outError = "Physics: FixedTimestep must be in (0, 0.25] seconds";
				return false;
			}
			return true;
		}

		// Creates the entities of an "Entities" array. When remapIDs is set every entity gets a fresh UUID
		// and parent references are translated. Returns the created entities in array order.
		bool CreateEntities(Scene& scene, const json& entities, bool remapIDs, Entity rootParent, std::vector<Entity>& outCreated, std::string& outError)
		{
			if (!entities.is_array())
			{
				outError = "'Entities' must be an array";
				return false;
			}

			std::unordered_map<uint64_t, UUID> idMap;
			std::vector<std::pair<Entity, uint64_t>> parentLinks;

			for (size_t i = 0; i < entities.size(); i++)
			{
				const json& entityData = entities[i];
				const std::string context = "entity #" + std::to_string(i);
				if (!entityData.is_object())
				{
					outError = context + " must be an object";
					return false;
				}

				uint64_t id = 0;
				std::string name = "Entity";
				uint64_t parentID = 0;
				ObjectReader reader(entityData, context);
				if (!reader.Id("ID", id, outError) || !reader.String("Name", name, outError) || !reader.Id("Parent", parentID, outError))
					return false;
				reader.Take("Components");
				if (!reader.Finish(outError))
					return false;

				const UUID uuid = (remapIDs || id == 0) ? UUID() : UUID(id);
				Entity entity = scene.CreateEntityWithUUID(uuid, name);
				idMap[id != 0 ? id : static_cast<uint64_t>(entity.GetUUID())] = entity.GetUUID();
				outCreated.push_back(entity);

				if (!SceneSerializer::DeserializeEntityComponents(entity, entityData, outError))
				{
					outError = std::string(context).append(" ('").append(name).append("'): ").append(outError);
					return false;
				}
				parentLinks.emplace_back(entity, parentID);
			}

			// Parent after creation so forward references work; array order is preserved for siblings.
			for (auto& [entity, parentID] : parentLinks)
			{
				if (parentID == 0)
				{
					if (rootParent)
						scene.SetParent(entity, rootParent, false);
					continue;
				}
				auto it = idMap.find(parentID);
				Entity parent = it != idMap.end() ? scene.GetEntityByUUID(it->second) : Entity{};
				if (!parent)
				{
					outError = "entity '" + entity.GetName() + "' references unknown parent " + std::to_string(parentID);
					return false;
				}
				if (!scene.SetParent(entity, parent, false))
				{
					outError = "entity '" + entity.GetName() + "' has an invalid parent (cycle or self-reference)";
					return false;
				}
			}
			return true;
		}

		void SerializeTreeInto(Entity entity, json& entities)
		{
			entities.push_back(SceneSerializer::SerializeEntity(entity));
			for (Entity child : entity.GetChildren())
				SerializeTreeInto(child, entities);
		}

	}

	json SceneSerializer::SerializeEntity(Entity entity)
	{
		json data;
		data["ID"] = static_cast<uint64_t>(entity.GetUUID());
		data["Name"] = entity.GetName();
		Entity parent = entity.GetParent();
		data["Parent"] = parent ? static_cast<uint64_t>(parent.GetUUID()) : 0;

		json components = json::object();
		for (const ComponentInfo& info : ComponentRegistry::GetAll())
		{
			// The tag is already written as "Name".
			if (info.Name == "Tag" || !info.Has(entity))
				continue;
			components[info.Name] = info.Serialize(entity);
		}
		data["Components"] = std::move(components);
		return data;
	}

	bool SceneSerializer::DeserializeEntityComponents(Entity entity, const json& data, std::string& outError)
	{
		if (auto name = data.find("Name"); name != data.end())
		{
			if (!name->is_string())
			{
				outError = "'Name' must be a string";
				return false;
			}
			entity.GetComponent<TagComponent>().Tag = name->get<std::string>();
		}

		auto components = data.find("Components");
		if (components == data.end())
			return true;
		if (!components->is_object())
		{
			outError = "'Components' must be an object";
			return false;
		}

		for (auto it = components->begin(); it != components->end(); ++it)
		{
			if (!ComponentRegistry::AddOrPatch(entity, it.key(), it.value(), outError))
				return false;
		}
		return true;
	}

	json SceneSerializer::SerializeScene(Scene& scene)
	{
		json data;
		data["Format"] = "BasaltScene";
		data["Version"] = FormatVersion;
		data["Name"] = scene.GetName();
		data["Renderer"] = SerializeRendererSettings(scene.GetRendererSettings());
		data["Physics"] = SerializePhysicsSettings(scene.GetPhysicsSettings());

		json entities = json::array();
		for (Entity entity : scene.GetAllEntitiesOrdered())
			entities.push_back(SerializeEntity(entity));
		data["Entities"] = std::move(entities);
		return data;
	}

	bool SceneSerializer::DeserializeScene(Scene& scene, const json& data, std::string& outError)
	{
		if (!data.is_object())
		{
			outError = "scene data must be a JSON object";
			return false;
		}

		ObjectReader reader(data, "scene");
		int64_t version = FormatVersion;
		std::string format = "BasaltScene";
		std::string name = scene.GetName();
		if (!reader.Integer("Version", version, 1, INT32_MAX, outError) || !reader.String("Format", format, outError) || !reader.String("Name", name, outError))
			return false;
		if (format != "BasaltScene")
		{
			outError = "'Format' is '" + format + "', expected 'BasaltScene'";
			return false;
		}
		if (version > FormatVersion)
		{
			outError = "scene format version " + std::to_string(version) + " is newer than supported version " + std::to_string(FormatVersion);
			return false;
		}
		const json* renderer = reader.Take("Renderer");
		const json* physics = reader.Take("Physics");
		const json* entities = reader.Take("Entities");
		if (!reader.Finish(outError))
			return false;

		scene.SetName(name);
		if (renderer && !DeserializeRendererSettings(*renderer, scene.GetRendererSettings(), outError))
			return false;
		if (physics && !DeserializePhysicsSettings(*physics, scene.GetPhysicsSettings(), outError))
			return false;
		if (!entities)
			return true;

		std::vector<Entity> created;
		return CreateEntities(scene, *entities, false, {}, created, outError);
	}

	bool SceneSerializer::SaveScene(Scene& scene, const std::filesystem::path& path, std::string& outError)
	{
		const std::string text = SerializeScene(scene).dump(1, '\t');
		if (!FileSystem::WriteTextFile(path, text))
		{
			outError = "cannot write '" + path.string() + "'";
			return false;
		}
		return true;
	}

	Ref<Scene> SceneSerializer::LoadScene(const std::filesystem::path& path, std::string& outError)
	{
		const auto text = FileSystem::ReadTextFile(path);
		if (!text)
		{
			outError = "cannot read '" + path.string() + "'";
			return nullptr;
		}

		json data = json::parse(*text, nullptr, false);
		if (data.is_discarded())
		{
			outError = "'" + path.string() + "' is not valid JSON";
			return nullptr;
		}

		Ref<Scene> scene = CreateRef<Scene>(path.stem().string());
		if (!DeserializeScene(*scene, data, outError))
		{
			outError = path.filename().string() + ": " + outError;
			return nullptr;
		}
		return scene;
	}

	json SceneSerializer::SerializeEntityTree(Entity root)
	{
		json entities = json::array();
		SerializeTreeInto(root, entities);
		// The root becomes a root of the serialized tree regardless of its parent in the scene.
		if (!entities.empty())
			entities[0]["Parent"] = 0;
		return entities;
	}

	Entity SceneSerializer::DeserializeEntityTree(Scene& scene, const json& entities, Entity parent, std::string& outError)
	{
		std::vector<Entity> created;
		if (!CreateEntities(scene, entities, true, parent, created, outError))
		{
			for (Entity entity : std::views::reverse(created))
			{
				if (entity.IsValid())
					scene.DestroyEntity(entity);
			}
			return {};
		}
		if (created.empty())
		{
			outError = "entity tree is empty";
			return {};
		}
		return created.front();
	}

	bool SceneSerializer::SavePrefab(Entity root, const std::filesystem::path& path, std::string& outError)
	{
		json data;
		data["Format"] = "BasaltPrefab";
		data["Version"] = FormatVersion;
		data["Entities"] = SerializeEntityTree(root);
		// An instance's own prefab link is not part of the prefab.
		data["Entities"][0]["Components"].erase("Prefab");

		if (!FileSystem::WriteTextFile(path, data.dump(1, '\t')))
		{
			outError = "cannot write '" + path.string() + "'";
			return false;
		}
		return true;
	}

	Entity SceneSerializer::InstantiatePrefab(Scene& scene, const std::filesystem::path& absolutePath, const std::string& prefabPath, Entity parent, std::string& outError)
	{
		const auto text = FileSystem::ReadTextFile(absolutePath);
		if (!text)
		{
			outError = "cannot read prefab '" + prefabPath + "'";
			return {};
		}
		json data = json::parse(*text, nullptr, false);
		if (data.is_discarded() || !data.is_object() || !data.contains("Entities"))
		{
			outError = "prefab '" + prefabPath + "' is not a valid prefab file";
			return {};
		}

		Entity root = DeserializeEntityTree(scene, data["Entities"], parent, outError);
		if (!root)
		{
			outError = "prefab '" + prefabPath + "': " + outError;
			return {};
		}
		root.AddOrReplaceComponent<PrefabComponent>(PrefabComponent{ prefabPath });
		return root;
	}

}
