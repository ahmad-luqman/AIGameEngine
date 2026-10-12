#pragma once

#include "Basalt/Core/UUID.h"

#include <sol/sol.hpp>

#include <cstdint>
#include <map>
#include <string>

namespace Basalt {

	struct ScriptContext;
	class ScriptHost;

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
	// frame. Every task has an owner (see ScriptContext::CurrentOwner): it runs with that owner current, is
	// dropped once ScriptHost::IsOwnerActive says the owner is gone, and an error in it is reported through
	// ScriptHost::FailOwner. Callers (the Lua bindings) validate arguments; the asserts here catch engine bugs.
	//
	// Lua references kept here are bound to the main thread: a reference bound to a coroutine's thread would
	// outlive that thread once the coroutine ends and is collected.
	class ScriptTasks
	{
	public:
		ScriptTasks(const sol::state_view& lua, ScriptContext& context, ScriptHost& host);

		ScriptTasks(const ScriptTasks&) = delete;
		ScriptTasks& operator=(const ScriptTasks&) = delete;

		// Calls callback(handle) after `delay` seconds (finite, >= 0).
		uint64_t AddOneShot(UUID owner, const sol::protected_function& callback, double delay);
		// Calls callback(handle) every `interval` seconds (finite, > 0), `count` times (0: until cancelled). It
		// fires at most once per frame; periods missed during one long frame are skipped, keeping its phase.
		uint64_t AddRepeating(UUID owner, const sol::protected_function& callback, double interval, uint32_t count);
		// Runs function(args...) as a coroutine right away, up to its first wait, from the `caller` thread.
		// Returns its id, which is already inactive if the coroutine finished without waiting. Throws
		// std::runtime_error with the Lua message if that first run raised an error (or the arguments do not
		// fit on a new Lua stack). Only for use by Lua bindings, which turn the exception into a Lua error.
		uint64_t StartCoroutine(lua_State* caller, UUID owner, const sol::protected_function& function, const sol::variadic_args& args);

		// Returns whether the task was active. A coroutine that cancels itself ends at its next wait. Pending
		// to-be-closed variables of a cancelled coroutine are not closed.
		bool Cancel(uint64_t id);
		bool IsActive(uint64_t id) const;
		// Whether id is an active coroutine started on `owner` (self:StopCoroutine only stops its own).
		bool IsCoroutineOf(uint64_t id, UUID owner) const;
		void CancelOwnedBy(UUID owner);
		void CancelCoroutinesOwnedBy(UUID owner);
		void Clear();

		// Whether `thread` is the coroutine this scheduler is running (only those may Wait).
		bool IsRunningCoroutine(lua_State* thread) const { return thread != nullptr && thread == m_Running; }
		// Wait/WaitFrames: records how long `thread` waits, which it does by yielding right after. False (nothing
		// recorded) unless `thread` is the coroutine this scheduler is running: only those may wait. Without a
		// call, a coroutine that yields waits one frame.
		bool RequestWaitSeconds(lua_State* thread, double seconds);
		bool RequestWaitFrames(lua_State* thread, uint64_t frames);

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
			// Timers only.
			sol::protected_function Callback;
			double Interval = 0.0;
			// Firings left; 0 means unlimited (repeating timers).
			uint32_t Remaining = 0;
			// Coroutines only.
			sol::thread Thread;
			// Due once the frame counter reaches ReadyFrame and the clock reaches ReadyTime.
			uint64_t ReadyFrame = 0;
			double ReadyTime = 0.0;
		};

		// How long the running coroutine waits: FrameCount frames, and with Seconds also until the clock
		// (Wait) — the default is a bare yield, one frame.
		struct WaitRequest
		{
			bool Seconds = false;
			uint64_t FrameCount = 1;
			double Duration = 0.0;
		};

		uint64_t AddTimer(UUID owner, const sol::protected_function& callback, double delay, double interval, uint32_t count);
		bool IsDue(const Task& task) const { return m_Frame >= task.ReadyFrame && m_Time >= task.ReadyTime; }
		void RunTimer(uint64_t id);
		void RunCoroutine(uint64_t id);
		// Resumes the task's coroutine with nargs arguments on its stack. A coroutine that waits is rescheduled
		// (unless it cancelled itself); one that finishes or fails is removed. False with outError on an error.
		bool Resume(uint64_t id, lua_State* from, int nargs, std::string& outError);

	private:
		lua_State* m_MainState = nullptr;
		ScriptContext& m_Context;
		ScriptHost& m_Host;
		// Ordered by id, which is creation order: tasks run deterministically.
		std::map<uint64_t, Task> m_Tasks;
		uint64_t m_NextID = 1;
		uint64_t m_Frame = 0;
		double m_Time = 0.0;
		lua_State* m_Running = nullptr;
		WaitRequest m_Wait;
	};

}
