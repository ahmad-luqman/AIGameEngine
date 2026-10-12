#include "Basalt/Scripting/ScriptTasks.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Scripting/ScriptGlue.h"

#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Basalt {

	namespace {

		// The error object on top of a failed coroutine's stack, as text with the coroutine's traceback (lua_resume
		// runs no message handler, but the dead coroutine's stack still describes where it failed). Pops it. The
		// traceback is built on `running`, the thread executing this code.
		std::string PopErrorMessage(lua_State* running, lua_State* thread)
		{
			// luaL_tolstring honours __tostring; it pushes the text onto the coroutine.
			const char* text = luaL_tolstring(thread, -1, nullptr);
			luaL_traceback(running, thread, text, 0);
			std::string message = lua_tostring(running, -1);
			lua_pop(running, 1);
			lua_pop(thread, 2);
			return message;
		}

	}

	ScriptTasks::ScriptTasks(const sol::state_view& lua, ScriptContext& context, ScriptHost& host)
		: m_MainState(lua.lua_state())
		, m_Context(context)
		, m_Host(host)
	{
	}

	uint64_t ScriptTasks::AddOneShot(UUID owner, const sol::protected_function& callback, double delay)
	{
		BS_CORE_ASSERT(std::isfinite(delay) && delay >= 0.0, "timer delays are validated by the bindings");
		return AddTimer(owner, callback, delay, 0.0, 1);
	}

	uint64_t ScriptTasks::AddRepeating(UUID owner, const sol::protected_function& callback, double interval, uint32_t count)
	{
		BS_CORE_ASSERT(std::isfinite(interval) && interval > 0.0, "timer intervals are validated by the bindings");
		return AddTimer(owner, callback, interval, interval, count);
	}

	uint64_t ScriptTasks::AddTimer(UUID owner, const sol::protected_function& callback, double delay, double interval, uint32_t count)
	{
		const uint64_t id = m_NextID++;
		// Rebound to the main thread: the callback may come from a coroutine, whose thread can be collected first.
		m_Tasks.emplace(id, Task{ .Type = Kind::Timer, .Owner = owner, .Callback = sol::protected_function(m_MainState, callback), .Interval = interval, .Remaining = count, .ReadyFrame = m_Frame + 1, .ReadyTime = m_Time + delay });
		return id;
	}

	uint64_t ScriptTasks::StartCoroutine(lua_State* caller, UUID owner, const sol::protected_function& function, const sol::variadic_args& args)
	{
		sol::thread thread = sol::thread::create(m_MainState);
		lua_State* coroutine = thread.thread_state();
		// A new thread guarantees only LUA_MINSTACK free slots; lua_xmove does not grow the stack.
		if (!lua_checkstack(coroutine, static_cast<int>(args.size()) + 1))
			throw std::runtime_error("StartCoroutine: too many arguments");
		function.push(coroutine);
		int argumentCount = 0;
		for (const sol::stack_proxy argument : args)
		{
			lua_pushvalue(caller, argument.stack_index());
			lua_xmove(caller, coroutine, 1);
			argumentCount++;
		}

		// Registered before it runs, so the body can stop itself (StopAllCoroutines) like any later run.
		const uint64_t id = m_NextID++;
		m_Tasks.emplace(id, Task{ .Type = Kind::Coroutine, .Owner = owner, .Thread = std::move(thread) });

		std::string error;
		if (!Resume(id, caller, argumentCount, error))
			throw std::runtime_error(error + " (in a coroutine)");
		return id;
	}

	bool ScriptTasks::Cancel(uint64_t id)
	{
		// A running coroutine stays alive through the copy of its thread that the resuming code holds.
		return m_Tasks.erase(id) > 0;
	}

	bool ScriptTasks::IsActive(uint64_t id) const
	{
		return m_Tasks.contains(id);
	}

	bool ScriptTasks::IsCoroutineOf(uint64_t id, UUID owner) const
	{
		const auto it = m_Tasks.find(id);
		return it != m_Tasks.end() && it->second.Type == Kind::Coroutine && it->second.Owner == owner;
	}

	void ScriptTasks::CancelOwnedBy(UUID owner)
	{
		std::erase_if(m_Tasks, [owner](const auto& entry) { return entry.second.Owner == owner; });
	}

	void ScriptTasks::CancelCoroutinesOwnedBy(UUID owner)
	{
		std::erase_if(m_Tasks, [owner](const auto& entry) { return entry.second.Owner == owner && entry.second.Type == Kind::Coroutine; });
	}

	void ScriptTasks::Clear()
	{
		m_Tasks.clear();
	}

	bool ScriptTasks::RequestWaitSeconds(lua_State* thread, double seconds)
	{
		if (thread == nullptr || thread != m_Running)
			return false;
		m_Wait = { .Seconds = true, .FrameCount = 1, .Duration = seconds };
		return true;
	}

	bool ScriptTasks::RequestWaitFrames(lua_State* thread, uint64_t frames)
	{
		if (thread == nullptr || thread != m_Running)
			return false;
		m_Wait = { .Seconds = false, .FrameCount = frames, .Duration = 0.0 };
		return true;
	}

	void ScriptTasks::BeginFrame(double deltaTime)
	{
		m_Frame++;
		m_Time += deltaTime;
	}

	void ScriptTasks::RunDue()
	{
		// Tasks created while these run (or rescheduled by them) are due next frame at the earliest.
		std::vector<uint64_t> due;
		for (const auto& [id, task] : m_Tasks)
		{
			if (IsDue(task))
				due.push_back(id);
		}

		for (uint64_t id : due)
		{
			auto it = m_Tasks.find(id);
			// Cancelled by a task that ran before it.
			if (it == m_Tasks.end())
				continue;
			if (!m_Host.IsOwnerActive(it->second.Owner))
			{
				m_Tasks.erase(it);
				continue;
			}
			if (it->second.Type == Kind::Timer)
				RunTimer(id);
			else
				RunCoroutine(id);
		}
	}

	void ScriptTasks::RunTimer(uint64_t id)
	{
		Task& task = m_Tasks.at(id);
		// Copied: the callback may cancel its own timer, which erases the task.
		const sol::protected_function callback = task.Callback;
		const UUID owner = task.Owner;

		// Rescheduled (or retired) before the call, so the callback can cancel itself either way.
		if (task.Interval <= 0.0 || task.Remaining == 1)
		{
			m_Tasks.erase(id);
		}
		else
		{
			if (task.Remaining > 0)
				task.Remaining--;
			task.ReadyFrame = m_Frame + 1;
			task.ReadyTime += task.Interval;
			if (task.ReadyTime <= m_Time)
				task.ReadyTime += std::floor((m_Time - task.ReadyTime) / task.Interval + 1.0) * task.Interval;
		}

		ScriptOwnerScope scope(m_Context, owner);
		const sol::protected_function_result result = callback(ScriptTaskHandle{ id });
		if (result.valid())
			return;
		const sol::error error = result;
		m_Tasks.erase(id);
		m_Host.FailOwner(owner, error.what(), "a timer");
	}

	void ScriptTasks::RunCoroutine(uint64_t id)
	{
		const UUID owner = m_Tasks.at(id).Owner;
		std::string error;
		if (!Resume(id, m_MainState, 0, error))
			m_Host.FailOwner(owner, error, "a coroutine");
	}

	bool ScriptTasks::Resume(uint64_t id, lua_State* from, int nargs, std::string& outError)
	{
		// Copied: this reference keeps the coroutine alive even if it cancels its own task while it runs.
		const Task& task = m_Tasks.at(id);
		const sol::thread thread = task.Thread;
		const UUID owner = task.Owner;
		lua_State* coroutine = thread.thread_state();

		// Saved and restored: a coroutine may start (and so resume) another one. m_Wait needs no saving: a
		// coroutine records its wait right before it yields, so no other resume can come in between.
		lua_State* previousRunning = m_Running;
		m_Running = coroutine;
		m_Wait = {};
		int resultCount = 0;
		int status = LUA_OK;
		{
			ScriptOwnerScope scope(m_Context, owner);
			status = lua_resume(coroutine, from, nargs, &resultCount);
		}
		const WaitRequest wait = m_Wait;
		m_Running = previousRunning;

		if (status == LUA_YIELD)
		{
			// Values yielded are not used.
			lua_pop(coroutine, resultCount);
			if (auto it = m_Tasks.find(id); it != m_Tasks.end())
			{
				it->second.ReadyFrame = m_Frame + wait.FrameCount;
				it->second.ReadyTime = wait.Seconds ? m_Time + wait.Duration : 0.0;
			}
			return true;
		}

		m_Tasks.erase(id);
		if (status == LUA_OK)
		{
			lua_pop(coroutine, resultCount);
			return true;
		}
		outError = PopErrorMessage(from, coroutine);
		// Closes the dead coroutine's pending to-be-closed variables (their own errors are already part of the
		// status, which is an error either way).
		lua_closethread(coroutine, from);
		return false;
	}

}
