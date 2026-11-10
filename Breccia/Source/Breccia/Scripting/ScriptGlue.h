#pragma once

#include "Breccia/Core/Base.h"
#include "Breccia/Core/UUID.h"
#include "Breccia/Math/Random.h"
#include "Breccia/UI/UIGeometry.h"

#include <sol/sol.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>

namespace Breccia {

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
		// Network.On handlers: connection events and script messages, apart from Events so Events.Emit cannot fake them.
		virtual ScriptEvents& GetNetworkEvents() = 0;
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

	// What one call into Lua has spent: the outermost ScriptOwnerScope (a callback, a timer, one resume of a coroutine), with
	// any it nests sharing its budget. Past the project's ScriptInstructionLimit, or once it has kept that many more bytes on
	// the Lua heap (64 MiB at least), a count hook raises an ordinary Lua error, so a script that never returns fails instead of
	// hanging its host, and one appending to a table forever fails before it takes the machine's memory. A call that ran out
	// has failed even if it caught that error: ScriptOwnerScope::Failed says so where the call was made.
	//
	// A count hook sends every instruction through Lua's trace function, whatever the count: tight Lua runs up to 2.7 times
	// slower, a script moving entities through bindings about 5%, hence ScriptEngine::DisableInstructionLimit for games. It counts
	// instructions rather than time so that it never depends on the machine, on a blocking binding or on a debugger, and it
	// reaches coroutines, which a watchdog thread could not see.
	//
	// Not stopped: C functions (a slow string.find), which run no instructions, and a single one can pass the heap limit by any
	// amount; __gc finalizers, where Lua turns hooks off; since a hook's error leaves them off until a pcall restores them, an
	// xpcall message handler the stop itself calls and the __close of a coroutine it ended. A coroutine ending within the
	// hook's period (256 instructions at most) charges nothing, as a new thread starts a new count, so loops of those run longer
	// than the limit says, though not without end, as the loop that starts them counts.
	class ScriptCallBudget
	{
	public:
		ScriptCallBudget() = default;
		ScriptCallBudget(const ScriptCallBudget&) = delete;
		ScriptCallBudget& operator=(const ScriptCallBudget&) = delete;

		// limit 0 sets no hook. Call once on a new state, before it has coroutines, which inherit the hook. The budget must
		// outlive the state.
		void Install(sol::state& lua, uint32_t limit);

		// True once the current call has run out: everything it nests fails, so a caller stops running more of it. Never true
		// outside a call.
		bool IsSpent() const { return m_Stop != nullptr; }
		// Why it ran out; null while it has not.
		const char* GetStop() const { return m_Stop; }

	private:
		BR_NOINLINE static void OnInstructions(lua_State* state, lua_Debug* debug);
		void Enter();
		void Leave();
		// Sets the stop, if the call has gone past a limit, given what the last firing charged.
		void Charge(lua_State* state, int instructions);

		// Lua's state, for its heap size; null with no limit.
		lua_State* m_State = nullptr;
		uint64_t m_InstructionLimit = 0;
		int m_HeapLimitKilobytes = 0;
		int m_HookPeriod = 0;
		uint32_t m_Depth = 0;
		uint64_t m_Instructions = 0;
		int m_HeapAtEntryKilobytes = 0;
		// Heap size at which to look closer: garbage counts until a collection says otherwise.
		int m_HeapCheckKilobytes = 0;
		// One of the two below, or null.
		const char* m_Stop = nullptr;
		std::string m_InstructionStop;
		std::string m_HeapStop;

		friend class ScriptOwnerScope;
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
		// See ScriptCallBudget.
		ScriptCallBudget Budget;

		static constexpr size_t MaxClipLeakWarnings = 1024;
		// A reload makes repaired callbacks eligible for a fresh diagnostic.
		void ResetClipLeakWarnings()
		{
			m_ReportedClipLeaks.clear();
			m_ReportedClipLeakLimit = false;
		}

	private:
		// Changed only by ScriptOwnerScope.
		UUID m_CurrentOwner = 0;
		std::unordered_set<std::string> m_ReportedClipLeaks;
		bool m_ReportedClipLeakLimit = false;

		friend class ScriptOwnerScope;
	};

	// Makes `owner` current and snapshots the caller's UI clip stack; destruction restores both, including
	// after a protected call fails. Unbalanced clips warn once per owner/callback until scripts reload.
	// After 1024 distinct pairs per ScriptContext, one limit warning suppresses further leak diagnostics
	// (see ScriptContext::GetCurrentOwner).
	// Rule: only wrap protected calls (pcall, lua_resume), and be how C++ reaches a script's code: the budget's hook raises inside
	// a scope and never outside one. Lua is compiled as C, so an error raised through an unprotected scope would longjmp past
	// its destructor and leave the wrong owner current, or end the process.
	class ScriptOwnerScope
	{
	public:
		ScriptOwnerScope(ScriptContext& context, UUID owner, const char* callback);
		~ScriptOwnerScope();

		ScriptOwnerScope(const ScriptOwnerScope&) = delete;
		ScriptOwnerScope& operator=(const ScriptOwnerScope&) = delete;

		// Whether a protected call made in this scope failed, or ran out and caught that, which fails it all the same (it would
		// pay the budget again on every call); outError says why. Call it before anything else runs in the scope.
		bool Failed(const sol::protected_function_result& result, std::string& outError) const;
		// table[key] with its metamethods, in a protected call, for C++ reading a table a script controls: an __index that
		// raises would otherwise end the process, and one that never returns would hang it. nullopt, with the error, when the
		// lookup failed.
		std::optional<sol::object> Index(const sol::table& table, std::string_view key, std::string& outError);

	private:
		ScriptContext& m_Context;
		UUID m_Previous;
		UUID m_Owner;
		const char* m_Callback;
		std::vector<UIBox> m_Clips;
	};

	// Lua's tostring, kept where a script's _G cannot reach it: reading a global from C++ runs _G's metamethods.
	constexpr const char* ToStringRegistryKey = "Breccia.ToString";

	// Registers the complete Breccia Lua API (see Docs/ScriptingAPI.md) into the state.
	void RegisterScriptBindings(sol::state& lua, ScriptContext& context);
	// The metatable shared by a script class's instances: keys resolve in the class (and its own metatable
	// chain), then in the methods every instance has (StartCoroutine, ...). Needs RegisterScriptBindings. nullopt, with the
	// error, only when the call it runs inside has run out.
	std::optional<sol::table> MakeInstanceMetatable(const sol::state_view& lua, const sol::table& scriptClass, std::string& outError);

}
