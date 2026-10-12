#include "Basalt/Scripting/ScriptTasks.h"

#include "Basalt/Scripting/ScriptGlue.h"

#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Basalt {

	namespace {

		// The error object on top of a failed coroutine's stack, as text (popped).
		std::string PopErrorMessage(lua_State* thread)
		{
			size_t length = 0;
			const char* text = lua_tolstring(thread, -1, &length);
			std::string message = text ? std::string(text, length) : std::string("(error object is not a string)");
			lua_pop(thread, 1);
			return message;
		}

	}

	ScriptTasks::ScriptTasks(const sol::state_view& lua, ScriptContext& context)
		: m_MainState(lua.lua_state())
		, m_Context(context)
	{
	}

	uint64_t ScriptTasks::AddTimer(UUID owner, sol::protected_function callback, double delay, double interval, uint32_t count)
	{
		const uint64_t id = m_NextID++;
		Task task;
		task.Type = Kind::Timer;
		task.Owner = owner;
		task.Callback = std::move(callback);
		task.ReadyFrame = m_Frame + 1;
		task.ReadyTime = m_Time + delay;
		task.Interval = interval;
		task.Remaining = count;
		m_Tasks.emplace(id, std::move(task));
		return id;
	}

	uint64_t ScriptTasks::StartCoroutine(lua_State* caller, UUID owner, const sol::protected_function& function, const sol::variadic_args& args)
	{
		sol::thread thread = sol::thread::create(m_MainState);
		lua_State* coroutine = thread.thread_state();
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
		Task task;
		task.Type = Kind::Coroutine;
		task.Owner = owner;
		task.Thread = thread;
		m_Tasks.emplace(id, std::move(task));

		std::string error;
		const ResumeStatus status = Resume(id, thread, owner, caller, argumentCount, error);
		if (status == ResumeStatus::Waiting)
			return id;
		m_Tasks.erase(id);
		if (status == ResumeStatus::Failed)
			throw std::runtime_error(error);
		return id;
	}

	bool ScriptTasks::Cancel(uint64_t id)
	{
		// A running coroutine keeps its thread alive through Resume's own reference until it yields.
		return m_Tasks.erase(id) > 0;
	}

	bool ScriptTasks::IsActive(uint64_t id) const
	{
		return m_Tasks.contains(id);
	}

	void ScriptTasks::CancelOwnedBy(UUID owner, bool coroutinesOnly)
	{
		std::erase_if(m_Tasks, [&](const auto& entry) { return entry.second.Owner == owner && (!coroutinesOnly || entry.second.Type == Kind::Coroutine); });
	}

	void ScriptTasks::Clear()
	{
		m_Tasks.clear();
	}

	void ScriptTasks::SetWaitSeconds(double seconds)
	{
		m_Wait = { .Set = true, .Frames = false, .FrameCount = 1, .Seconds = seconds };
	}

	void ScriptTasks::SetWaitFrames(uint64_t frames)
	{
		m_Wait = { .Set = true, .Frames = true, .FrameCount = frames, .Seconds = 0.0 };
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
			if (!m_Context.IsOwnerActive(it->second.Owner))
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
		const bool last = task.Interval <= 0.0 || task.Remaining == 1;
		if (last)
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
		m_Context.FailOwner(owner, std::string(error.what()) + " (in a timer)");
	}

	void ScriptTasks::RunCoroutine(uint64_t id)
	{
		// Copied: the coroutine may cancel its own task while it runs.
		const Task& task = m_Tasks.at(id);
		const sol::thread thread = task.Thread;
		const UUID owner = task.Owner;
		std::string error;
		const ResumeStatus status = Resume(id, thread, owner, m_MainState, 0, error);
		if (status == ResumeStatus::Waiting)
			return;
		m_Tasks.erase(id);
		if (status == ResumeStatus::Failed)
			m_Context.FailOwner(owner, error + " (in a coroutine)");
	}

	ScriptTasks::ResumeStatus ScriptTasks::Resume(uint64_t id, const sol::thread& thread, UUID owner, lua_State* from, int nargs, std::string& outError)
	{
		// The caller's `thread` reference keeps the coroutine alive even if it cancels its own task while running.
		lua_State* coroutine = thread.thread_state();
		lua_State* previousRunning = m_Running;
		const WaitRequest previousWait = m_Wait;
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
		m_Wait = previousWait;

		if (status == LUA_OK || status == LUA_YIELD)
		{
			// Values yielded or returned are not used.
			lua_pop(coroutine, resultCount);
			if (status == LUA_OK)
				return ResumeStatus::Finished;
			auto it = m_Tasks.find(id);
			if (it != m_Tasks.end())
			{
				Task& task = it->second;
				const bool frames = !wait.Set || wait.Frames;
				task.ReadyFrame = m_Frame + (frames ? wait.FrameCount : 1);
				task.ReadyTime = frames ? 0.0 : m_Time + wait.Seconds;
			}
			return ResumeStatus::Waiting;
		}

		outError = PopErrorMessage(coroutine);
		if (outError.empty())
			outError = "coroutine failed";
		return ResumeStatus::Failed;
	}

}
