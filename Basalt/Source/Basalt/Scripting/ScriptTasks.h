#pragma once

#include "Basalt/Core/UUID.h"

#include <sol/sol.hpp>

#include <cstdint>
#include <map>
#include <string>

namespace Basalt {

	struct ScriptContext;

	// Lua-side handle of a timer or coroutine (TaskHandle). Holds only the id, so it stays safe to keep after
	// the task ended or play stopped.
	struct ScriptTaskHandle
	{
		uint64_t ID = 0;
	};

	// Timers (Timer.After/Every) and coroutines (self:StartCoroutine) of one play session.
	//
	// Time is game time: BeginFrame advances it once per scene update (a paused scene runs nothing), and
	// RunDue runs the due tasks in creation order after OnUpdate. A task always waits at least until the next
	// frame. Every task has an owner (ScriptContext::CurrentOwner when created): it runs with that owner
	// current, is dropped once the owner is no longer active, and an error in it is reported through
	// ScriptContext::FailOwner (which disables the owning instance).
	class ScriptTasks
	{
	public:
		ScriptTasks(const sol::state_view& lua, ScriptContext& context);

		ScriptTasks(const ScriptTasks&) = delete;
		ScriptTasks& operator=(const ScriptTasks&) = delete;

		// Calls callback(handle) after `delay` seconds and then, when interval > 0, every `interval` seconds,
		// `count` times in all (0: until cancelled). A repeating timer fires at most once per frame; periods
		// missed during one long frame are skipped, keeping its phase.
		uint64_t AddTimer(UUID owner, sol::protected_function callback, double delay, double interval, uint32_t count);
		// Runs function(args...) as a coroutine right away, up to its first wait, from the `caller` thread.
		// Returns its id, which is already inactive if the coroutine finished without waiting. Throws
		// std::runtime_error with the Lua message if that first run raised an error.
		uint64_t StartCoroutine(lua_State* caller, UUID owner, const sol::protected_function& function, const sol::variadic_args& args);

		// Returns whether the task was active. A coroutine that cancels itself ends at its next wait.
		bool Cancel(uint64_t id);
		bool IsActive(uint64_t id) const;
		// Cancels the owner's tasks (only its coroutines with coroutinesOnly).
		void CancelOwnedBy(UUID owner, bool coroutinesOnly = false);
		void Clear();
		size_t GetCount() const { return m_Tasks.size(); }

		// Wait/WaitFrames may only suspend the coroutine this scheduler is running, from its own thread.
		bool IsRunningCoroutine(lua_State* thread) const { return thread != nullptr && thread == m_Running; }
		// Records how long the running coroutine waits; it yields right after. Without a call, a coroutine that
		// yields waits one frame.
		void SetWaitSeconds(double seconds);
		void SetWaitFrames(uint64_t frames);

		void BeginFrame(double deltaTime);
		void RunDue();

	private:
		enum class Kind
		{
			Timer,
			Coroutine
		};

		struct Task
		{
			Kind Type = Kind::Timer;
			UUID Owner = 0;
			sol::protected_function Callback;
			sol::thread Thread;
			// Due once the frame counter reaches ReadyFrame and the clock reaches ReadyTime.
			uint64_t ReadyFrame = 0;
			double ReadyTime = 0.0;
			double Interval = 0.0;
			// Firings left (timers); 0 means unlimited.
			uint32_t Remaining = 0;
		};

		struct WaitRequest
		{
			bool Set = false;
			bool Frames = true;
			uint64_t FrameCount = 1;
			double Seconds = 0.0;
		};

		enum class ResumeStatus
		{
			Waiting,
			Finished,
			Failed
		};

		bool IsDue(const Task& task) const { return m_Frame >= task.ReadyFrame && m_Time >= task.ReadyTime; }
		void RunTimer(uint64_t id);
		void RunCoroutine(uint64_t id);
		// Resumes the coroutine with nargs arguments on its stack. On Waiting, the task (if still present)
		// is rescheduled; on Failed, outError holds the message.
		ResumeStatus Resume(uint64_t id, const sol::thread& thread, UUID owner, lua_State* from, int nargs, std::string& outError);

	private:
		lua_State* m_MainState = nullptr;
		ScriptContext& m_Context;
		// Ordered by id, which is creation order: tasks run deterministically.
		std::map<uint64_t, Task> m_Tasks;
		uint64_t m_NextID = 1;
		uint64_t m_Frame = 0;
		double m_Time = 0.0;
		lua_State* m_Running = nullptr;
		WaitRequest m_Wait;
	};

}
