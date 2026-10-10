#include "Basalt/Scripting/ScriptEngine.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/Scripting/LuaJson.h"
#include "Basalt/Scripting/ScriptGlue.h"

#include <sol/sol.hpp>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace Basalt {

	namespace {

		// C++ exceptions thrown by bindings become ordinary Lua errors; sol2's default handler would also
		// print them to stderr, duplicating what the engine already reports.
		int QuietExceptionHandler(lua_State* state, sol::optional<const std::exception&>, std::string_view description)
		{
			return sol::stack::push(state, description);
		}

		void OpenSafeLibraries(sol::state& lua)
		{
			lua.set_exception_handler(&QuietExceptionHandler);
			lua.open_libraries(sol::lib::base, sol::lib::package, sol::lib::string, sol::lib::math, sol::lib::table, sol::lib::utf8, sol::lib::coroutine);
			// Scripts load code only through require (resolved inside the project).
			lua["dofile"] = sol::lua_nil;
			lua["loadfile"] = sol::lua_nil;

			const std::filesystem::path root = Project::ResolvePath(".");
			const std::string base = root.lexically_normal().generic_string();
			lua["package"]["path"] = base + "/?.lua;" + base + "/?/init.lua";
			lua["package"]["cpath"] = "";
		}

		// Loads a script file and returns its class table.
		std::optional<sol::table> LoadScriptClass(sol::state& lua, const std::string& scriptPath, std::string& outError)
		{
			const auto source = FileSystem::ReadTextFile(Project::ResolvePath(scriptPath));
			if (!source)
			{
				outError = "cannot read script '" + scriptPath + "'";
				return std::nullopt;
			}

			sol::load_result chunk = lua.load(*source, "@" + scriptPath);
			if (!chunk.valid())
			{
				const sol::error error = chunk;
				outError = error.what();
				return std::nullopt;
			}

			sol::protected_function function = chunk;
			sol::protected_function_result result = function();
			if (!result.valid())
			{
				const sol::error error = result;
				outError = error.what();
				return std::nullopt;
			}
			if (result.get_type() != sol::type::table)
			{
				outError = scriptPath + ": script must return a table (its class)";
				return std::nullopt;
			}
			return result.get<sol::table>();
		}

		sol::object ConvertProperty(const sol::state_view& lua, const sol::object& defaultValue, const nlohmann::json& value)
		{
			// Keep vector-typed properties as vectors when overridden from JSON arrays of numbers.
			float components[4] = {};
			bool numeric = value.is_array() && value.size() <= 4;
			for (size_t i = 0; numeric && i < value.size(); i++)
				numeric = JsonToFloat(value[i], components[i]);
			if (numeric && value.size() == 3 && defaultValue.is<glm::vec3>())
				return sol::make_object(lua, glm::vec3(components[0], components[1], components[2]));
			if (numeric && value.size() == 2 && defaultValue.is<glm::vec2>())
				return sol::make_object(lua, glm::vec2(components[0], components[1]));
			if (numeric && value.size() == 4 && defaultValue.is<glm::vec4>())
				return sol::make_object(lua, glm::vec4(components[0], components[1], components[2], components[3]));
			return JsonToLua(lua, value);
		}

	}

	struct ScriptEngine::Impl
	{
		struct Instance
		{
			std::string ScriptPath;
			sol::table Self;
			bool Failed = false;
			// OnCreate has been called. OnDestroy is only called on created instances.
			bool Created = false;
		};

		// Declared before Lua so it outlives lua_close(): __gc metamethods may still call bindings.
		ScriptContext Context;
		sol::state Lua;
		std::unordered_map<std::string, sol::table> Classes;
		std::unordered_set<std::string> FailedClasses;
		std::unordered_map<UUID, Instance> Instances;
		// Creation order, for deterministic callback order.
		std::vector<UUID> Order;
		std::unordered_set<UUID> Pending;
		bool Started = false;

		void OnScriptComponentChanged(entt::registry& registry, entt::entity entity)
		{
			if (const auto* id = registry.try_get<IDComponent>(entity))
				Pending.insert(id->ID);
		}
	};

	struct ScriptEngineAccess
	{
		static sol::object GetInstance(ScriptEngine& engine, UUID id)
		{
			auto it = engine.m_Impl->Instances.find(id);
			if (it == engine.m_Impl->Instances.end() || it->second.Failed)
				return sol::lua_nil;
			return it->second.Self;
		}

		// Calls self:<name>(args...) if defined. Returns false (after reporting) if the call raised an error.
		template<typename... Args>
		static bool Invoke(ScriptEngine& engine, const sol::table& self, const char* name, Args&&... args)
		{
			const sol::object callback = self[name];
			if (callback.get_type() != sol::type::function)
				return true;
			sol::protected_function function = callback.as<sol::protected_function>();
			const sol::protected_function_result result = function(self, std::forward<Args>(args)...);
			if (result.valid())
				return true;
			const sol::error error = result;
			engine.ReportError(std::string(error.what()) + " (in " + name + ")");
			return false;
		}

		// Calls a callback on a live instance. Disables the instance on error.
		template<typename... Args>
		static void Call(ScriptEngine& engine, UUID id, const char* name, Args&&... args)
		{
			auto it = engine.m_Impl->Instances.find(id);
			if (it == engine.m_Impl->Instances.end() || it->second.Failed)
				return;

			// Copy the table: the callback may destroy this instance (e.g. by removing its component).
			const sol::table self = it->second.Self;
			if (Invoke(engine, self, name, std::forward<Args>(args)...))
				return;
			auto failed = engine.m_Impl->Instances.find(id);
			if (failed != engine.m_Impl->Instances.end())
				failed->second.Failed = true;
		}

		// Removes the instance first, then calls OnDestroy on it, so the callback cannot re-enter teardown.
		static void Teardown(ScriptEngine& engine, UUID id)
		{
			ScriptEngine::Impl& impl = *engine.m_Impl;
			auto it = impl.Instances.find(id);
			if (it == impl.Instances.end())
				return;
			ScriptEngine::Impl::Instance instance = std::move(it->second);
			impl.Instances.erase(it);
			impl.Order.erase(std::remove(impl.Order.begin(), impl.Order.end(), id), impl.Order.end());
			if (instance.Created && !instance.Failed)
				Invoke(engine, instance.Self, "OnDestroy");
		}

		// Calls OnCreate once on an instance that exists, has not been created, and whose entity still has
		// its script and is not about to be destroyed.
		static void StartInstance(ScriptEngine& engine, UUID id)
		{
			ScriptEngine::Impl& impl = *engine.m_Impl;
			auto it = impl.Instances.find(id);
			if (it == impl.Instances.end() || it->second.Created || it->second.Failed)
				return;
			Entity entity = engine.m_Scene->GetEntityByUUID(id);
			if (!entity || engine.m_Scene->IsEntityPendingDestruction(entity) || !entity.HasComponent<ScriptComponent>())
				return;
			it->second.Created = true;
			Call(engine, id, "OnCreate");
		}
	};

	ScriptEngine::ScriptEngine(Scene* scene)
		: m_Impl(CreateScope<Impl>())
		, m_Scene(scene)
	{
		OpenSafeLibraries(m_Impl->Lua);
		m_Impl->Context.SceneContext = scene;
		m_Impl->Context.Engine = this;
		m_Impl->Context.GetInstance = [this](UUID id) { return ScriptEngineAccess::GetInstance(*this, id); };
		RegisterScriptBindings(m_Impl->Lua, m_Impl->Context);

		entt::registry& registry = scene->GetRegistry();
		registry.on_construct<ScriptComponent>().connect<&Impl::OnScriptComponentChanged>(*m_Impl);
		registry.on_update<ScriptComponent>().connect<&Impl::OnScriptComponentChanged>(*m_Impl);
		registry.on_destroy<ScriptComponent>().connect<&Impl::OnScriptComponentChanged>(*m_Impl);
	}

	ScriptEngine::~ScriptEngine()
	{
		entt::registry& registry = m_Scene->GetRegistry();
		registry.on_construct<ScriptComponent>().disconnect<&Impl::OnScriptComponentChanged>(*m_Impl);
		registry.on_update<ScriptComponent>().disconnect<&Impl::OnScriptComponentChanged>(*m_Impl);
		registry.on_destroy<ScriptComponent>().disconnect<&Impl::OnScriptComponentChanged>(*m_Impl);

		// Lua references must be released before the state is closed.
		m_Impl->Instances.clear();
		m_Impl->Classes.clear();
	}

	void ScriptEngine::ReportError(const std::string& message)
	{
		BS_CORE_ERROR("Script error: {}", message);
		m_Errors.push_back(message);
	}

	void ScriptEngine::EnsureInstance(Entity entity)
	{
		if (CreateInstance(entity))
			ScriptEngineAccess::StartInstance(*this, entity.GetUUID());
	}

	void ScriptEngine::EnsureInstances(const std::vector<Entity>& entities)
	{
		// Two phases so every new instance exists (and is reachable through GetScript) before any OnCreate.
		std::vector<UUID> created;
		for (Entity entity : entities)
		{
			if (entity && entity.HasComponent<ScriptComponent>() && CreateInstance(entity))
				created.push_back(entity.GetUUID());
		}
		for (UUID uuid : created)
			ScriptEngineAccess::StartInstance(*this, uuid);
	}

	bool ScriptEngine::CreateInstance(Entity entity)
	{
		Impl& impl = *m_Impl;
		if (!entity)
			return false;
		const UUID uuid = entity.GetUUID();
		impl.Pending.erase(uuid);

		// Copy the component: Lua run below (OnDestroy, the script chunk) may remove or move it in storage.
		const auto* liveComponent = entity.TryGetComponent<ScriptComponent>();
		if (!liveComponent)
		{
			ScriptEngineAccess::Teardown(*this, uuid);
			return false;
		}
		const ScriptComponent componentCopy = *liveComponent;
		const ScriptComponent* component = &componentCopy;

		auto existing = impl.Instances.find(uuid);
		if (existing != impl.Instances.end())
		{
			if (existing->second.ScriptPath == component->Script)
				return false;
			// Script changed: tear down the old instance.
			ScriptEngineAccess::Teardown(*this, uuid);
		}
		if (component->Script.empty() || !entity.IsValid())
			return false;

		// Load (or reuse) the class.
		auto classIt = impl.Classes.find(component->Script);
		if (classIt == impl.Classes.end())
		{
			if (impl.FailedClasses.contains(component->Script))
				return false;
			std::string error;
			auto scriptClass = LoadScriptClass(impl.Lua, component->Script, error);
			if (!scriptClass)
			{
				impl.FailedClasses.insert(component->Script);
				ReportError(error);
				return false;
			}
			classIt = impl.Classes.emplace(component->Script, *scriptClass).first;
		}
		const sol::table& scriptClass = classIt->second;

		// Instance: fields from Properties (with per-entity overrides), methods via the class metatable.
		sol::table self = impl.Lua.create_table();
		sol::table metatable = impl.Lua.create_table();
		metatable[sol::meta_function::index] = scriptClass;
		self[sol::metatable_key] = metatable;
		self["Entity"] = ScriptEntity{ uuid, m_Scene };

		const sol::object properties = scriptClass["Properties"];
		if (properties.get_type() == sol::type::table)
		{
			for (const auto& [key, defaultValue] : properties.as<sol::table>())
			{
				if (!key.is<std::string>())
					continue;
				const std::string name = key.as<std::string>();
				auto overrideIt = component->Properties.find(name);
				if (overrideIt != component->Properties.end())
				{
					try
					{
						self[name] = ConvertProperty(impl.Lua, defaultValue, *overrideIt);
					}
					catch (const std::exception& e)
					{
						ReportError(component->Script + ": invalid value for property '" + name + "': " + e.what());
						self[name] = defaultValue;
					}
				}
				else if (defaultValue.is<glm::vec3>())
				{
					// Copy vectors so instances never share (and mutate) the class default.
					self[name] = glm::vec3(defaultValue.as<glm::vec3>());
				}
				else
				{
					self[name] = defaultValue;
				}
			}
		}

		Impl::Instance instance;
		instance.ScriptPath = component->Script;
		instance.Self = self;
		impl.Instances[uuid] = std::move(instance);
		impl.Order.push_back(uuid);

		return true;
	}

	bool ScriptEngine::HasInstance(Entity entity) const
	{
		return entity && m_Impl->Instances.contains(entity.GetUUID());
	}

	std::optional<nlohmann::json> ScriptEngine::GetInstanceField(Entity entity, const std::string& name) const
	{
		if (!entity)
			return std::nullopt;
		auto it = m_Impl->Instances.find(entity.GetUUID());
		if (it == m_Impl->Instances.end())
			return std::nullopt;
		try
		{
			return LuaToJson(it->second.Self[name].get<sol::object>());
		}
		catch (const std::exception&)
		{
			return std::nullopt;
		}
	}

	void ScriptEngine::Start()
	{
		Impl& impl = *m_Impl;
		impl.Started = true;
		impl.Pending.clear();
		// Every instance exists before the first OnCreate runs (scripts can reach each other in OnCreate).
		EnsureInstances(m_Scene->GetAllEntitiesOrdered());
	}

	void ScriptEngine::Update(Timestep ts)
	{
		Impl& impl = *m_Impl;
		impl.Context.DeltaTime = ts.GetSeconds();

		// Components added or changed since the last frame (outside of the Lua API).
		if (!impl.Pending.empty())
		{
			std::vector<UUID> pending(impl.Pending.begin(), impl.Pending.end());
			impl.Pending.clear();
			std::sort(pending.begin(), pending.end(), [](UUID a, UUID b) { return static_cast<uint64_t>(a) < static_cast<uint64_t>(b); });
			for (UUID uuid : pending)
				EnsureInstance(m_Scene->GetEntityByUUID(uuid));
		}

		const std::vector<UUID> order = impl.Order;
		for (UUID uuid : order)
		{
			Entity entity = m_Scene->GetEntityByUUID(uuid);
			if (entity && !m_Scene->IsEntityPendingDestruction(entity))
				ScriptEngineAccess::Call(*this, uuid, "OnUpdate", ts.GetSeconds());
		}
	}

	void ScriptEngine::LateUpdate(Timestep ts)
	{
		const std::vector<UUID> order = m_Impl->Order;
		for (UUID uuid : order)
		{
			Entity entity = m_Scene->GetEntityByUUID(uuid);
			if (entity && !m_Scene->IsEntityPendingDestruction(entity))
				ScriptEngineAccess::Call(*this, uuid, "OnLateUpdate", ts.GetSeconds());
		}
	}

	void ScriptEngine::Stop()
	{
		Impl& impl = *m_Impl;
		const std::vector<UUID> order = impl.Order;
		for (UUID uuid : order)
			ScriptEngineAccess::Teardown(*this, uuid);
		impl.Instances.clear();
		impl.Order.clear();
		impl.Started = false;
	}

	void ScriptEngine::OnEntityDestroyed(Entity entity)
	{
		Impl& impl = *m_Impl;
		const UUID uuid = entity.GetUUID();
		impl.Pending.erase(uuid);
		ScriptEngineAccess::Teardown(*this, uuid);
	}

	void ScriptEngine::OnContactEvent(ContactEventType type, Entity a, Entity b, const std::optional<ContactInfo>& contact)
	{
		const char* callback = "OnCollisionBegin";
		switch (type)
		{
			case ContactEventType::CollisionBegin:
				callback = "OnCollisionBegin";
				break;
			case ContactEventType::CollisionEnd:
				callback = "OnCollisionEnd";
				break;
			case ContactEventType::TriggerEnter:
				callback = "OnTriggerEnter";
				break;
			case ContactEventType::TriggerExit:
				callback = "OnTriggerExit";
				break;
		}

		sol::state& lua = m_Impl->Lua;
		auto makeContact = [&lua](const ContactInfo& info) -> sol::object {
			sol::table table = lua.create_table();
			table["Point"] = info.Point;
			table["Normal"] = info.Normal;
			table["Impulse"] = info.Impulse;
			return table;
		};
		// Each side gets its own table: a callback may modify the one it receives.
		const sol::object contactA = contact ? makeContact(*contact) : sol::object(sol::lua_nil);
		ScriptEngineAccess::Call(*this, a.GetUUID(), callback, ScriptEntity{ b.GetUUID(), m_Scene }, contactA);
		if (a && b)
		{
			const sol::object contactB = contact ? makeContact(contact->Flipped()) : sol::object(sol::lua_nil);
			ScriptEngineAccess::Call(*this, b.GetUUID(), callback, ScriptEntity{ a.GetUUID(), m_Scene }, contactB);
		}
	}

	void ScriptEngine::OnJointBroken(Entity holder, Entity body, Entity connected)
	{
		// Captured up front: a callback may destroy any of these entities.
		const UUID holderID = holder.GetUUID();
		const UUID bodyID = body ? body.GetUUID() : holderID;
		const UUID connectedID = connected ? connected.GetUUID() : UUID(0);
		auto notify = [this](UUID target, UUID other) {
			if (other != 0)
				ScriptEngineAccess::Call(*this, target, "OnJointBreak", ScriptEntity{ other, m_Scene });
			else
				ScriptEngineAccess::Call(*this, target, "OnJointBreak", sol::lua_nil);
		};
		notify(bodyID, connectedID);
		if (connectedID != 0)
			notify(connectedID, bodyID);
		// A separate joint entity hears about its own joint too, with the far side like its body, unless it is
		// one of the two bodies and was notified above.
		if (holderID != bodyID && holderID != connectedID)
			notify(holderID, connectedID);
	}

	bool ScriptEngine::ExecuteString(const std::string& code, std::string& outResult)
	{
		sol::state& lua = m_Impl->Lua;
		// Try as an expression first so "Scene.GetEntityCount()" returns its value.
		sol::load_result chunk = lua.load("return " + code, "=console");
		if (!chunk.valid())
			chunk = lua.load(code, "=console");
		if (!chunk.valid())
		{
			const sol::error error = chunk;
			outResult = error.what();
			return false;
		}

		sol::protected_function function = chunk;
		sol::protected_function_result result = function();
		if (!result.valid())
		{
			const sol::error error = result;
			outResult = error.what();
			return false;
		}

		outResult.clear();
		if (result.return_count() > 0)
		{
			const sol::protected_function toString = lua["tostring"];
			sol::protected_function_result text = toString(result.get<sol::object>(0));
			outResult = text.valid() ? text.get<std::string>() : "?";
		}
		return true;
	}

	std::optional<nlohmann::json> ScriptEngine::LoadScriptProperties(const std::string& scriptPath, std::string& outError)
	{
		ScriptContext context;
		sol::state lua;
		OpenSafeLibraries(lua);
		RegisterScriptBindings(lua, context);

		auto scriptClass = LoadScriptClass(lua, scriptPath, outError);
		if (!scriptClass)
			return std::nullopt;

		const sol::object properties = (*scriptClass)["Properties"];
		if (properties.get_type() != sol::type::table)
			return nlohmann::json::object();
		try
		{
			nlohmann::json json = LuaToJson(properties);
			return json.is_object() ? json : nlohmann::json::object();
		}
		catch (const std::exception& e)
		{
			outError = scriptPath + ": invalid Properties table: " + e.what();
			return std::nullopt;
		}
	}

}
