#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Core/Timestep.h"
#include "Basalt/Core/UUID.h"
#include "Basalt/Physics/PhysicsWorld.h"
#include "Basalt/UI/UIWidgetEvent.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <span>
#include <string>
#include <unordered_set>
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
	//   function Player:OnCollisionBegin(other, contact) end
	//   function Player:OnCollisionEnd(other) end  -- likewise OnTriggerEnter, OnTriggerExit (no contact)
	//   function Player:OnJointBreak(other) end    -- other is the far body; nil for a joint to the world
	//   function Player:OnGamepadConnected(index) end  -- also for gamepads connected before the instance started;
	//                                                  -- likewise OnGamepadDisconnected
	//   return Player
	//
	// Each entity gets an instance table (self) whose fields start as the property values, with
	// self.Entity referring to its entity, plus engine methods (self:StartCoroutine, ...). A runtime error
	// disables the failing instance and is recorded (GetErrors) instead of stopping the game.
	//
	// The engine also owns the session's timers and coroutines (ScriptTasks), events (ScriptEvents) and save
	// data (SaveData, loaded on first use). Timers, coroutines and event handlers belong to the instance whose
	// code created them and end with it; Update runs the due ones after every OnUpdate. The full Lua API is
	// documented in Docs/ScriptingAPI.md.
	class ScriptEngine
	{
	public:
		explicit ScriptEngine(Scene* scene);
		~ScriptEngine();

		ScriptEngine(const ScriptEngine&) = delete;
		ScriptEngine& operator=(const ScriptEngine&) = delete;

		// Instantiates every ScriptComponent, then calls OnCreate on each in hierarchy order (all instances
		// exist before the first OnCreate runs).
		void Start();
		// Creates the instances of scripts added since the last frame, calls OnGamepadConnected/Disconnected for the
		// gamepads whose connection changed since the last Update (the first one announces those already connected;
		// a gamepad replaced at its index is a disconnect and a connect), the widget callbacks of `uiEvents`
		// (UIInteraction) and OnUpdate, then runs the due timers and coroutines.
		void Update(Timestep ts, std::span<const UIWidgetEvent> uiEvents);
		void LateUpdate(Timestep ts);
		// Calls OnDestroy on every live instance, then ends the session's timers, coroutines and event handlers.
		void Stop();

		void OnEntityDestroyed(Entity entity);
		// Calls the event's callback on both entities, each with the other and the contact (Point, Normal
		// pointing away from the other entity, Impulse) as a table, or nil when there is none (see ContactInfo).
		void OnContactEvent(ContactEventType type, Entity a, Entity b, const std::optional<ContactInfo>& contact);
		// Calls OnJointBreak on the joint's body and on the connected entity (if any), each with the other,
		// and once on the entity holding the JointComponent when it is neither (with the connected entity, or
		// nil for the world).
		void OnJointBroken(Entity holder, Entity body, Entity connected);

		// Creates the entity's script instance now (and calls OnCreate) if it has a ScriptComponent and no
		// instance yet. Used after spawning entities so they are initialized immediately.
		void EnsureInstance(Entity entity);
		// Like EnsureInstance for a group (e.g. a spawned prefab): all instances are created first, then
		// OnCreate is called on each in order.
		void EnsureInstances(const std::vector<Entity>& entities);
		bool HasInstance(Entity entity) const;
		// A field of the entity's script instance (self.<name>) converted to JSON.
		std::optional<nlohmann::json> GetInstanceField(Entity entity, const std::string& name) const;

		// Runs a chunk of Lua in the scene's script state (editor console / automation). The result of the
		// last expression (if any) is returned as a string.
		bool ExecuteString(const std::string& code, std::string& outResult);

		// Records a script error (logged, and returned by GetErrors, e.g. play.step's scriptErrors).
		void ReportError(const std::string& message);
		const std::vector<std::string>& GetErrors() const { return m_Errors; }
		void ClearErrors() { m_Errors.clear(); }

		// Loads a script in an isolated Lua state and returns its Properties defaults (editor inspector).
		static std::optional<nlohmann::json> LoadScriptProperties(const std::string& scriptPath, std::string& outError);
		// Compiles a script without running it (syntax errors only). Modules loaded through require may return
		// anything, so this is the check that applies to every script file.
		static bool CheckSyntax(const std::string& scriptPath, std::string& outError);

	private:
		// Creates the instance (properties applied) without calling OnCreate. False if none was created.
		bool CreateInstance(Entity entity);
		void DispatchGamepadConnections();
		// Calls a widget's callback on its target. A callback that cannot be called (no target entity, no script,
		// no such method) is a script error, reported once per widget, method and problem; a widget or target that an
		// earlier callback this frame destroyed is skipped.
		void DispatchUIEvent(const UIWidgetEvent& event);

	private:
		struct Impl;
		Scope<Impl> m_Impl;
		Scene* m_Scene = nullptr;
		std::vector<std::string> m_Errors;
		std::unordered_set<std::string> m_ReportedUICallbacks;

		friend struct ScriptEngineAccess;
	};

}
