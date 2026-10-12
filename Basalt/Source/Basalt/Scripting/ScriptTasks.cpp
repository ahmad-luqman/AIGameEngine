#include "Basalt/Scripting/ScriptTasks.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Scripting/ScriptGlue.h"

#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Basalt {

	namespace {

		// Protected helpers for a failed coroutine. Lua is compiled as C, so an error raised outside a protected
		// call would longjmp past the C++ frames above; these run in lua_pcall on the running thread.
		int ErrorObjectToString(lua_State* state)
		{
			// Honours __tostring (which may itself fail: the pcall catches that).
			luaL_tolstring(state, 1, nullptr);
			return 1;
		}

		int CoroutineTraceback(lua_State* state)
		{
			luaL_traceback(state, static_cast<lua_State*>(lua_touserdata(state, 1)), nullptr, 0);
			return 1;
		}

		// The value on top of `running` as text (popped), or a description of it when converting fails.
		std::string PopAsText(lua_State* running)
		{
			const std::string type = luaL_typename(running, -1);
			lua_pushcfunction(running, &ErrorObjectToString);
			lua_insert(running, -2);
			if (lua_pcall(running, 1, 1, 0) != LUA_OK)
			{
				lua_pop(running, 1);
				return "(error object is a " + type + " value whose __tostring failed)";
			}
			std::string text = lua_tostring(running, -1);
			lua_pop(running, 1);
			return text;
		}

		// The dead coroutine's error object (left on its stack) as text, without running anything on it.
		std::string ErrorText(lua_State* running, lua_State* thread)
		{
			lua_pushvalue(thread, -1);
			lua_xmove(thread, running, 1);
			return PopAsText(running);
		}

		// Where the dead coroutine failed (its stack is intact until lua_closethread), or empty.
		std::string Traceback(lua_State* running, lua_State* thread)
		{
			lua_pushcfunction(running, &CoroutineTraceback);
			lua_pushlightuserdata(running, thread);
			std::string traceback;
			if (lua_pcall(running, 1, 1, 0) == LUA_OK)
				traceback = lua_tostring(running, -1);
			lua_pop(running, 1);
			return traceback;
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

	ScriptTasks::StopResult ScriptTasks::StopCoroutine(uint64_t id, UUID owner)
	{
		const auto it = m_Tasks.find(id);
		if (it == m_Tasks.end())
			return StopResult::NotActive;
		if (it->second.Type != Kind::Coroutine || it->second.Owner != owner)
			return StopResult::NotOwned;
		m_Tasks.erase(it);
		return StopResult::Stopped;
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

	void ScriptTasks::RequestWaitSeconds(lua_State* thread, double seconds)
	{
		BS_CORE_ASSERT(IsRunningCoroutine(thread), "Wait checks that it runs in the scheduler's coroutine");
		m_Active.Wait = { .FrameCount = 1, .Seconds = seconds };
	}

	void ScriptTasks::RequestWaitFrames(lua_State* thread, uint64_t frames)
	{
		BS_CORE_ASSERT(IsRunningCoroutine(thread), "WaitFrames checks that it runs in the scheduler's coroutine");
		m_Active.Wait = { .FrameCount = frames, .Seconds = 0.0 };
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
		sol::protected_function callback;
		UUID owner = 0;
		{
			Task& task = m_Tasks.at(id);
			// Copied: the callback may cancel its own timer, which erases the task.
			callback = task.Callback;
			owner = task.Owner;
			// Rescheduled (or retired) before the call, so the callback can cancel itself either way.
			if (task.Remaining == 1)
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
		sol::thread thread;
		UUID owner = 0;
		{
			// Copied: this reference keeps the coroutine alive even if it cancels its own task while it runs.
			const Task& task = m_Tasks.at(id);
			thread = task.Thread;
			owner = task.Owner;
		}
		lua_State* coroutine = thread.thread_state();

		// Saved and restored as one: a coroutine may start (and so resume) another one, whose wait must not
		// become the starter's (a bare coroutine.yield() records no wait of its own).
		const ActiveResume previous = std::exchange(m_Active, ActiveResume{ .Thread = coroutine });
		int resultCount = 0;
		int status = LUA_OK;
		{
			ScriptOwnerScope scope(m_Context, owner);
			status = lua_resume(coroutine, from, nargs, &resultCount);
		}
		const WaitRequest wait = std::exchange(m_Active, previous).Wait;

		if (status == LUA_YIELD)
		{
			// Values yielded are not used.
			lua_pop(coroutine, resultCount);
			if (auto it = m_Tasks.find(id); it != m_Tasks.end())
			{
				it->second.ReadyFrame = m_Frame + wait.FrameCount;
				it->second.ReadyTime = m_Time + wait.Seconds;
			}
			return true;
		}

		m_Tasks.erase(id);
		if (status == LUA_OK)
		{
			lua_pop(coroutine, resultCount);
			return true;
		}

		// The message and where it was raised, taken before lua_closethread resets the stack. As the owner: a
		// __tostring may create timers or handlers, which must end with the failing instance.
		{
			ScriptOwnerScope scope(m_Context, owner);
			outError = ErrorText(from, coroutine);
		}
		const std::string traceback = Traceback(from, coroutine);
		if (!traceback.empty())
			outError += "\n" + traceback;
		// Closes the dead coroutine's pending to-be-closed variables (their __close gets the error), as its owner.
		lua_pushvalue(coroutine, -1);
		lua_xmove(coroutine, from, 1);
		int closeStatus = LUA_OK;
		{
			ScriptOwnerScope scope(m_Context, owner);
			closeStatus = lua_closethread(coroutine, from);
		}
		// The thread's error is left on its stack: the original one, or the error a __close raised instead.
		if (closeStatus != LUA_OK)
		{
			lua_xmove(coroutine, from, 1);
			if (!lua_rawequal(from, -1, -2))
			{
				ScriptOwnerScope scope(m_Context, owner);
				outError += "\n(closing its to-be-closed variables also failed: " + PopAsText(from) + ")";
			}
			else
			{
				lua_pop(from, 1);
			}
		}
		lua_pop(from, 1);
		return false;
	}

}
