#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Core/Timestep.h"
#include "Basalt/Core/UUID.h"
#include "Basalt/Physics/PhysicsWorld.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	class Entity;
	class Scene;

	// Runs Lua behaviours (ScriptComponent) for one playing scene. A script file returns a class table:
	//
	//   local Player = {}
	//   Player.Properties = { Speed = 5.0 }        -- defaults, overridable per entity
	//   function Player:OnCreate() end
	//   function Player:OnUpdate(dt) end
	//   function Player:OnLateUpdate(dt) end      -- after physics
	//   function Player:OnDestroy() end
	//   function Player:OnCollisionBegin(other) end  -- also OnCollisionEnd, OnTriggerEnter, OnTriggerExit
	//   return Player
	//
	// Each entity gets an instance table (self) whose fields start as the property values, with
	// self.Entity referring to its entity. A runtime error disables the failing instance and is recorded
	// (GetErrors) instead of stopping the game. The full Lua API is documented in Docs/ScriptingAPI.md.
	class ScriptEngine
	{
	public:
		explicit ScriptEngine(Scene* scene);
		~ScriptEngine();

		ScriptEngine(const ScriptEngine&) = delete;
		ScriptEngine& operator=(const ScriptEngine&) = delete;

		// Instantiates every ScriptComponent and calls OnCreate, in hierarchy order.
		void Start();
		void Update(Timestep ts);
		void LateUpdate(Timestep ts);
		// Calls OnDestroy on every live instance.
		void Stop();

		void OnEntityDestroyed(Entity entity);
		void OnContactEvent(ContactEventType type, Entity a, Entity b);

		// Creates the entity's script instance now (and calls OnCreate) if it has a ScriptComponent and no
		// instance yet. Used after spawning entities so they are initialized immediately.
		void EnsureInstance(Entity entity);
		bool HasInstance(Entity entity) const;
		// A field of the entity's script instance (self.<name>) converted to JSON.
		std::optional<nlohmann::json> GetInstanceField(Entity entity, const std::string& name) const;

		// Runs a chunk of Lua in the scene's script state (editor console / automation). The result of the
		// last expression (if any) is returned as a string.
		bool ExecuteString(const std::string& code, std::string& outResult);

		const std::vector<std::string>& GetErrors() const { return m_Errors; }
		void ClearErrors() { m_Errors.clear(); }

		// Loads a script in an isolated Lua state and returns its Properties defaults (editor inspector).
		static std::optional<nlohmann::json> LoadScriptProperties(const std::string& scriptPath, std::string& outError);

	private:
		void ReportError(const std::string& message);

	private:
		struct Impl;
		Scope<Impl> m_Impl;
		Scene* m_Scene = nullptr;
		std::vector<std::string> m_Errors;

		friend struct ScriptEngineAccess;
	};

}
