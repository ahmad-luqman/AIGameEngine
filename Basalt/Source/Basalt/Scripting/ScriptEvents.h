#pragma once

#include "Basalt/Core/UUID.h"

#include <sol/sol.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace Basalt {

	struct ScriptContext;

	// Lua-side handle of an event subscription (EventHandle); holds only the id.
	struct ScriptEventHandle
	{
		uint64_t ID = 0;
	};

	// Named events between scripts of one play session (Lua Events.On/Once/Emit). Emit is synchronous: the
	// handlers subscribed when it starts run in subscription order before it returns. A handler belongs to
	// its owner (ScriptContext::CurrentOwner when subscribing): it runs with that owner current, is skipped
	// while the owner is not active, and an error in it is reported through ScriptContext::FailOwner.
	class ScriptEvents
	{
	public:
		// Handlers that emit events themselves nest; deeper than this is almost certainly an endless loop.
		static constexpr int MaxEmitDepth = 32;

		explicit ScriptEvents(ScriptContext& context);

		ScriptEvents(const ScriptEvents&) = delete;
		ScriptEvents& operator=(const ScriptEvents&) = delete;

		// With once, the subscription ends right before its handler first runs.
		uint64_t Subscribe(UUID owner, const std::string& name, sol::protected_function handler, bool once);
		// Returns whether the subscription was active.
		bool Cancel(uint64_t id);
		bool IsActive(uint64_t id) const;
		void RemoveOwnedBy(UUID owner);
		void Clear();

		// Calls handler(args...) for every subscription to name that exists when the call starts and is still
		// active when its turn comes, on the caller's thread. Returns how many handlers ran. Throws
		// std::runtime_error when nested deeper than MaxEmitDepth.
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
		ScriptContext& m_Context;
		std::map<uint64_t, Subscription> m_Subscriptions;
		// Subscription ids per event name, in subscription order.
		std::unordered_map<std::string, std::vector<uint64_t>> m_ByName;
		uint64_t m_NextID = 1;
		int m_Depth = 0;
	};

}
