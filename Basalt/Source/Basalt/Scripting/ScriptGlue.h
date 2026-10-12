#pragma once

#include "Basalt/Core/UUID.h"
#include "Basalt/Math/Random.h"

#include <sol/sol.hpp>

#include <functional>
#include <string>

namespace Basalt {

	class Entity;
	class SaveData;
	class Scene;
	class ScriptEngine;
	class ScriptEvents;
	class ScriptTasks;

	// Lua-side entity handle. Stores the UUID, so it stays safe to hold after the entity is destroyed:
	// every method checks that the entity still exists and raises a Lua error otherwise.
	struct ScriptEntity
	{
		UUID ID = 0;
		Scene* SceneContext = nullptr;

		Entity Resolve() const;
		Entity ResolveOrThrow() const;
		bool operator==(const ScriptEntity& other) const { return ID == other.ID && SceneContext == other.SceneContext; }
	};

	// State shared between the bindings and the ScriptEngine.
	struct ScriptContext
	{
		// Null when scripts are loaded outside a running scene (property discovery).
		Scene* SceneContext = nullptr;
		ScriptEngine* Engine = nullptr;
		// Returns the script instance table of an entity, or nil.
		std::function<sol::object(UUID)> GetInstance;
		float DeltaTime = 0.0f;
		// Backs Math.Random* and Lua's math.random: seeded and identical on every platform.
		Random Rng;
		// The session's starting seed (Scene::GetRandomSeed), which math.randomseed() returns to.
		uint32_t Seed = Random::DefaultSeed;

		// The script instance (entity UUID) whose code is running: an instance callback, or one of its timers,
		// coroutines or event handlers. Timers, coroutines and event handlers created now belong to it. 0 for
		// code outside any instance (console, lua.exec), whose tasks belong to the play session.
		UUID CurrentOwner = 0;
		// The play session's timers/coroutines and events; null outside a running scene.
		ScriptTasks* Tasks = nullptr;
		ScriptEvents* Events = nullptr;
		// Whether an owner can still run code: true for 0 (the session), otherwise its instance exists, has
		// started (OnCreate), has not failed, and its entity is not about to be destroyed.
		std::function<bool(UUID)> IsOwnerActive;
		// Reports an error raised by an owner's timer, coroutine or event handler and disables the owning
		// instance (cancelling everything it owns), like an error in any other callback.
		std::function<void(UUID owner, const std::string& message)> FailOwner;
		// The play session's save data (Lua Storage), loaded on first use. Throws std::runtime_error when it
		// cannot be located (no active project, no user data directory). Unset outside a running scene.
		std::function<SaveData&()> GetStorage;

		// Starts the random sequence of a session: the seed and the generator change together.
		void ResetRandom(uint32_t seed)
		{
			Seed = seed;
			Rng.Seed(seed);
		}
	};

	// Makes `owner` the context's current owner for the lifetime of the scope (see ScriptContext::CurrentOwner).
	// Only wraps protected calls (pcall, lua_resume), so a Lua error never skips the restore.
	class ScriptOwnerScope
	{
	public:
		ScriptOwnerScope(ScriptContext& context, UUID owner)
			: m_Context(context)
			, m_Previous(context.CurrentOwner)
		{
			context.CurrentOwner = owner;
		}
		~ScriptOwnerScope() { m_Context.CurrentOwner = m_Previous; }

		ScriptOwnerScope(const ScriptOwnerScope&) = delete;
		ScriptOwnerScope& operator=(const ScriptOwnerScope&) = delete;

	private:
		ScriptContext& m_Context;
		UUID m_Previous;
	};

	// Registers the complete Basalt Lua API (see Docs/ScriptingAPI.md) into the state.
	void RegisterScriptBindings(sol::state& lua, ScriptContext& context);
	// The metatable shared by a script class's instances: keys resolve in the class (and its own metatable
	// chain), then in the methods every instance has (StartCoroutine, ...). Needs RegisterScriptBindings.
	sol::table MakeInstanceMetatable(const sol::state_view& lua, const sol::table& scriptClass);

}
