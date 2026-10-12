#include "Basalt/Scripting/ScriptEvents.h"

#include "Basalt/Scripting/ScriptGlue.h"

#include <stdexcept>
#include <utility>

namespace Basalt {

	ScriptEvents::ScriptEvents(ScriptContext& context)
		: m_Context(context)
	{
	}

	uint64_t ScriptEvents::Subscribe(UUID owner, const std::string& name, sol::protected_function handler, bool once)
	{
		const uint64_t id = m_NextID++;
		m_Subscriptions.emplace(id, Subscription{ .Owner = owner, .Name = name, .Handler = std::move(handler), .Once = once });
		m_ByName[name].push_back(id);
		return id;
	}

	bool ScriptEvents::Cancel(uint64_t id)
	{
		auto it = m_Subscriptions.find(id);
		if (it == m_Subscriptions.end())
			return false;
		auto byName = m_ByName.find(it->second.Name);
		if (byName != m_ByName.end())
		{
			std::erase(byName->second, id);
			if (byName->second.empty())
				m_ByName.erase(byName);
		}
		m_Subscriptions.erase(it);
		return true;
	}

	bool ScriptEvents::IsActive(uint64_t id) const
	{
		return m_Subscriptions.contains(id);
	}

	void ScriptEvents::RemoveOwnedBy(UUID owner)
	{
		std::vector<uint64_t> owned;
		for (const auto& [id, subscription] : m_Subscriptions)
		{
			if (subscription.Owner == owner)
				owned.push_back(id);
		}
		for (uint64_t id : owned)
			Cancel(id);
	}

	void ScriptEvents::Clear()
	{
		m_Subscriptions.clear();
		m_ByName.clear();
	}

	int ScriptEvents::Emit(lua_State* caller, const std::string& name, const sol::variadic_args& args)
	{
		if (m_Depth >= MaxEmitDepth)
			throw std::runtime_error("Events.Emit('" + name + "'): events nested more than " + std::to_string(MaxEmitDepth) + " levels deep (does a handler emit the event it handles?)");

		auto byName = m_ByName.find(name);
		if (byName == m_ByName.end())
			return 0;
		// Handlers subscribed by these handlers wait for the next emit.
		const std::vector<uint64_t> ids = byName->second;

		struct DepthGuard
		{
			int& Depth;
			explicit DepthGuard(int& depth)
				: Depth(depth)
			{
				Depth++;
			}
			~DepthGuard() { Depth--; }
			DepthGuard(const DepthGuard&) = delete;
			DepthGuard& operator=(const DepthGuard&) = delete;
		} depthGuard(m_Depth);

		int called = 0;
		for (uint64_t id : ids)
		{
			auto it = m_Subscriptions.find(id);
			// Cancelled by an earlier handler.
			if (it == m_Subscriptions.end())
				continue;
			const UUID owner = it->second.Owner;
			if (!m_Context.IsOwnerActive(owner))
				continue;
			// Bound to the caller's thread, where the arguments are: Emit may run inside a coroutine.
			const sol::protected_function handler(caller, it->second.Handler);
			if (it->second.Once)
				Cancel(id);

			called++;
			ScriptOwnerScope scope(m_Context, owner);
			const sol::protected_function_result result = handler(args);
			if (!result.valid())
			{
				const sol::error error = result;
				m_Context.FailOwner(owner, std::string(error.what()) + " (in a handler of event '" + name + "')");
			}
		}
		return called;
	}

}
