#include "Breccia/Scripting/ScriptEvents.h"

#include "Breccia/Core/Assert.h"
#include "Breccia/Scripting/ScriptGlue.h"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <vector>

namespace Breccia {

	namespace {

		// Counts the nesting of Emit for the lifetime of one call.
		class DepthGuard
		{
		public:
			explicit DepthGuard(int& depth)
				: m_Depth(depth)
			{
				m_Depth++;
			}
			~DepthGuard() { m_Depth--; }

			DepthGuard(const DepthGuard&) = delete;
			DepthGuard& operator=(const DepthGuard&) = delete;

		private:
			int& m_Depth;
		};

	}

	ScriptEvents::ScriptEvents(const sol::state_view& lua, ScriptContext& context, ScriptHost& host)
		: m_MainState(lua.lua_state())
		, m_Context(context)
		, m_Host(host)
	{
	}

	uint64_t ScriptEvents::Subscribe(UUID owner, const std::string& name, const sol::protected_function& handler, bool once)
	{
		BR_CORE_ASSERT(!name.empty(), "event names are validated by the bindings");
		const uint64_t id = m_NextID++;
		// Rebound to the main thread: the handler may come from a coroutine, whose thread can be collected first.
		m_Subscriptions.emplace(id, Subscription{ .Owner = owner, .Name = name, .Handler = sol::protected_function(m_MainState, handler), .Once = once });
		return id;
	}

	bool ScriptEvents::Cancel(uint64_t id)
	{
		return m_Subscriptions.erase(id) > 0;
	}

	bool ScriptEvents::IsActive(uint64_t id) const
	{
		return m_Subscriptions.contains(id);
	}

	void ScriptEvents::RemoveOwnedBy(UUID owner)
	{
		std::erase_if(m_Subscriptions, [owner](const auto& entry) { return entry.second.Owner == owner; });
	}

	void ScriptEvents::Clear()
	{
		m_Subscriptions.clear();
	}

	int ScriptEvents::Emit(lua_State* caller, const std::string& name, const sol::variadic_args& args)
	{
		if (m_Depth >= MaxEmitDepth)
			throw std::runtime_error("Events.Emit('" + name + "'): events nested more than " + std::to_string(MaxEmitDepth) + " levels deep (does a handler emit the event it handles?)");
		bool emitterFailed = false;
		const int called = Dispatch(caller, name, [&args](const sol::protected_function& handler) { return handler(args); }, emitterFailed);
		if (emitterFailed)
			throw std::runtime_error("Events.Emit('" + name + "'): a handler of this instance failed (see the previous error), which disabled it");
		return called;
	}

	bool ScriptEvents::HasSubscribers(const std::string& name) const
	{
		return std::ranges::any_of(m_Subscriptions, [&name](const auto& entry) { return entry.second.Name == name; });
	}

	int ScriptEvents::EmitValues(const std::string& name, const std::vector<sol::object>& args)
	{
		bool emitterFailed = false;
		return Dispatch(m_MainState, name, [&args](const sol::protected_function& handler) { return handler(sol::as_args(args)); }, emitterFailed);
	}

	int ScriptEvents::Dispatch(lua_State* caller, const std::string& name, const std::function<sol::protected_function_result(const sol::protected_function&)>& call, bool& emitterFailed)
	{
		// Handlers subscribed by these handlers wait for the next emit.
		std::vector<uint64_t> ids;
		for (const auto& [id, subscription] : m_Subscriptions)
		{
			if (subscription.Name == name)
				ids.push_back(id);
		}
		if (ids.empty())
			return 0;

		const DepthGuard depth(m_Depth);
		const UUID emitter = m_Context.GetCurrentOwner();
		// Recorded rather than inferred: the emitter also stops being active when a handler merely destroys it.
		int called = 0;
		for (uint64_t id : ids)
		{
			auto it = m_Subscriptions.find(id);
			// Cancelled by an earlier handler.
			if (it == m_Subscriptions.end())
				continue;
			// The caller ran out: it stops at its next instruction, and the rest would only fail with it, through no fault of theirs.
			if (m_Context.Budget.IsSpent())
				break;
			const UUID owner = it->second.Owner;
			if (!m_Host.IsOwnerActive(owner))
				continue;
			// Bound to the caller's thread, where the arguments are: Emit may run inside a coroutine.
			const sol::protected_function handler(caller, it->second.Handler);
			if (it->second.Once)
				Cancel(id);

			called++;
			ScriptOwnerScope scope(m_Context, owner, "event handler");
			const sol::protected_function_result result = call(handler);
			if (std::string error; scope.Failed(result, error))
			{
				// A shared owner's handler (the session's, a script's or module's top level) would fail again on every
				// emit; an instance's ends with its instance.
				if (m_Host.IsSharedOwner(owner))
					Cancel(id);
				else if (owner == emitter)
					emitterFailed = true;
				m_Host.FailOwner(owner, error, "a handler of event '" + name + "'");
			}
		}
		return called;
	}

}
