#pragma once

#include "Basalt/Core/UUID.h"
#include "Basalt/Math/Random.h"
#include "Basalt/UI/UIGeometry.h"

#include <sol/sol.hpp>

#include <string>
#include <vector>

namespace Basalt {

	class Entity;
	class SaveData;
	class Scene;
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

	// The running play session as the bindings see it (implemented by ScriptEngine). Owners are entity UUIDs of
	// script instances; owner 0 is the play session itself (console and lua.exec code, top-level script code).
	class ScriptHost
	{
	public:
		// The script instance table of an entity, or nil (none, or it failed).
		virtual sol::object GetInstance(UUID id) = 0;
		// Creates and starts the script instances of new entities now (see ScriptEngine::EnsureInstances).
		virtual void EnsureInstances(const std::vector<Entity>& entities) = 0;

		// Whether an owner can still run code: always for 0 and the top level of loaded scripts and modules, otherwise
		// its instance exists, has started (OnCreate), has not failed, and its entity is not about to be destroyed.
		virtual bool IsOwnerActive(UUID owner) const = 0;
		// The owner is not an instance: the play session (0) or the top level of a script or module. A failing timer or
		// handler of such an owner ends alone; nothing else is disabled.
		virtual bool IsSharedOwner(UUID owner) const = 0;
		// Reports an error raised in an owner's timer, coroutine or event handler (`where`, e.g. "a timer") and
		// disables the owning instance, cancelling everything it owns, like an error in any other callback. For a
		// shared owner the error is only reported (the caller ends the failing timer or handler itself).
		virtual void FailOwner(UUID owner, const std::string& message, const std::string& where) = 0;

		virtual ScriptTasks& GetTasks() = 0;
		virtual ScriptEvents& GetEvents() = 0;
		// The session's save data (Lua Storage), loaded on first use: a problem with that load is reported as a
		// script error. Throws std::runtime_error when it cannot be located (no active project, no user data
		// directory).
		virtual SaveData& GetStorage() = 0;
		// Storage.Load: (re)reads the save data, returning the problem (rather than reporting it) even when this
		// is the first use. Throws like GetStorage when the save data cannot be located.
		virtual bool LoadStorage(std::string& outError) = 0;

	protected:
		~ScriptHost() = default;
	};

	// State shared between the bindings and the ScriptEngine.
	class ScriptContext
	{
	public:
		// Null when scripts are loaded outside a running scene (property discovery).
		Scene* SceneContext = nullptr;
		// The running session; null in property discovery (no scene) and once the engine starts shutting down,
		// so bindings called from __gc metamethods during lua_close never reach a dying engine.
		ScriptHost* Host = nullptr;
		float DeltaTime = 0.0f;
		// Backs Math.Random* and Lua's math.random: seeded and identical on every platform.
		Random Rng;
		// The session's starting seed (Scene::GetRandomSeed), which math.randomseed() returns to.
		uint32_t Seed = Random::DefaultSeed;

		// Starts the random sequence of a session: the seed and the generator change together.
		void ResetRandom(uint32_t seed)
		{
			Seed = seed;
			Rng.Seed(seed);
		}

		// The script instance (entity UUID) whose engine callback is running: an instance callback, or one of
		// its timers, coroutines or event handlers. Timers and event handlers created now belong to it (calling
		// another instance's method does not change it); coroutines belong to the instance they are started on.
		// 0 outside instance code (console, lua.exec, top-level script code).
		UUID GetCurrentOwner() const { return m_CurrentOwner; }

	private:
		// Changed only by ScriptOwnerScope.
		UUID m_CurrentOwner = 0;
		std::vector<UUID> m_ReportedClipLeaks;

		friend class ScriptOwnerScope;
	};

	// Makes `owner` the context's current owner for the lifetime of the scope (see ScriptContext::GetCurrentOwner).
	// Rule: only wrap protected calls (pcall, lua_resume). Lua is compiled as C, so an error raised through an
	// unprotected scope would longjmp past its destructor and leave the wrong owner current.
	class ScriptOwnerScope
	{
	public:
		ScriptOwnerScope(ScriptContext& context, UUID owner, const char* callback = "callback");
		~ScriptOwnerScope();

		ScriptOwnerScope(const ScriptOwnerScope&) = delete;
		ScriptOwnerScope& operator=(const ScriptOwnerScope&) = delete;

	private:
		ScriptContext& m_Context;
		UUID m_Previous;
		UUID m_Owner;
		const char* m_Callback;
		std::vector<UIBox> m_Clips;
	};

	// Registers the complete Basalt Lua API (see Docs/ScriptingAPI.md) into the state.
	void RegisterScriptBindings(sol::state& lua, ScriptContext& context);
	// The metatable shared by a script class's instances: keys resolve in the class (and its own metatable
	// chain), then in the methods every instance has (StartCoroutine, ...). Needs RegisterScriptBindings.
	sol::table MakeInstanceMetatable(const sol::state_view& lua, const sol::table& scriptClass);

}
