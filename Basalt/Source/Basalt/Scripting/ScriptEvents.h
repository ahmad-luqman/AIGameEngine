#pragma once

#include "Basalt/Core/UUID.h"

#include <sol/sol.hpp>

#include <cstdint>
#include <map>
#include <string>

namespace Basalt {

	struct ScriptContext;
	class ScriptHost;

	// Lua-side handle of an event subscription (EventHandle); holds only the id.
	struct ScriptEventHandle
	{
		uint64_t ID = 0;
	};

	// Named events between scripts of one play session (Lua Events.On/Once/Emit). Emit is synchronous: the
	// handlers subscribed when it starts run in subscription order before it returns. A handler belongs to its
	// owner (ScriptContext::CurrentOwner when subscribing): it runs with that owner current, is skipped while
	// ScriptHost::IsOwnerActive says the owner is not active, and an error in it is reported through
	// ScriptHost::FailOwner. Handlers are kept bound to the main thread, like ScriptTasks' callbacks.
	class ScriptEvents
	{
	public:
		// Handlers that emit events themselves nest; deeper than this is almost certainly an endless loop.
		static constexpr int MaxEmitDepth = 32;

		ScriptEvents(const sol::state_view& lua, ScriptContext& context, ScriptHost& host);

		ScriptEvents(const ScriptEvents&) = delete;
		ScriptEvents& operator=(const ScriptEvents&) = delete;

		// With once, the subscription ends right before its handler first runs. name must not be empty.
		uint64_t Subscribe(UUID owner, const std::string& name, const sol::protected_function& handler, bool once);
		// Returns whether the subscription was active.
		bool Cancel(uint64_t id);
		bool IsActive(uint64_t id) const;
		void RemoveOwnedBy(UUID owner);
		void Clear();

		// Calls handler(args...) for every subscription to name that exists when the call starts and is still
		// active when its turn comes, on the caller's thread. Returns how many handlers were called (failed ones
		// included). A failing handler of the session (owner 0) is unsubscribed. Throws std::runtime_error when
		// nested deeper than MaxEmitDepth, or when a handler failed and so disabled the emitting instance itself
		// (its callback must not continue as if nothing happened).
		int Emit(lua_State* caller, const std::string& name, const sol::variadic_args& args);

	private:
		struct Subscription
		{
			UUID Owner = 0;
			std::string Name;
			sol::protected_function Handler;
			bool Once = false;
		};

	private:
		lua_State* m_MainState = nullptr;
		ScriptContext& m_Context;
		ScriptHost& m_Host;
		// Ordered by id, which is subscription order.
		std::map<uint64_t, Subscription> m_Subscriptions;
		uint64_t m_NextID = 1;
		int m_Depth = 0;
	};

}
