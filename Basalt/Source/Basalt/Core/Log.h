#pragma once

#include "Basalt/Core/Base.h"

#include <spdlog/spdlog.h>

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Basalt {

	enum class LogLevel : uint8_t
	{
		Trace = 0,
		Info,
		Warn,
		Error,
		Critical
	};

	// Lowercase level names ("trace" ... "critical"), as log.get and automation events report them.
	const char* LogLevelName(LogLevel level);
	std::optional<LogLevel> LogLevelFromName(std::string_view name);

	struct LogMessage
	{
		LogLevel Level = LogLevel::Info;
		std::string LoggerName;
		std::string Text;
	};

	// Thread-safe ring buffer of recent log messages. Read by the editor console and by the
	// automation API so tools (and AI agents) can observe what the engine reported.
	class LogHistory
	{
	public:
		explicit LogHistory(size_t capacity = 2048);

		using Listener = std::function<void(const LogMessage& message)>;

		void Push(const LogMessage& message);
		void Clear();

		// Calls the listener with every message pushed from now on, after the history stores it, on the pushing
		// thread (any thread; one call at a time, under the listener lock). A listener must be quick and must not
		// add or remove listeners (that deadlocks); messages it logs itself reach the history but no listener.
		// Exceptions it throws are reported on stderr and dropped. Returns the ID for RemoveListener.
		uint64_t AddListener(Listener listener);
		// When this returns, no call of the listener is running or will start.
		void RemoveListener(uint64_t id);

		// Returns every message with an index >= sinceIndex, plus the index to pass next time.
		std::vector<LogMessage> GetMessagesSince(uint64_t sinceIndex, uint64_t& outNextIndex) const;
		std::vector<LogMessage> GetAll() const;
		uint64_t GetTotalCount() const;
		size_t GetErrorCount() const;

	private:
		mutable std::mutex m_Mutex;
		std::vector<LogMessage> m_Messages;
		size_t m_Capacity = 0;
		size_t m_Head = 0;
		uint64_t m_TotalCount = 0;
		size_t m_ErrorCount = 0;
		// Separate from m_Mutex so listeners can run without blocking readers of the history.
		std::mutex m_ListenerMutex;
		std::vector<std::pair<uint64_t, Listener>> m_Listeners;
		uint64_t m_NextListenerId = 1;
	};

	// Engine-wide logging (spdlog). Messages go to the console, an optional file, and LogHistory.
	class Log
	{
	public:
		// Safe to call more than once; later calls are no-ops.
		static void Init(const std::string& logFilePath = "");
		// Flushes all sinks. Loggers remain usable afterwards (they live until process exit).
		static void Shutdown();
		static bool IsInitialized();
		// Minimum level written to the console. File and history sinks always receive everything.
		static void SetConsoleLevel(LogLevel level);

		static spdlog::logger& GetCoreLogger();
		static spdlog::logger& GetClientLogger();
		static LogHistory& GetHistory();

	private:
		static void EnsureInitialized();
	};

}

#define BS_CORE_TRACE(...) ::Basalt::Log::GetCoreLogger().trace(__VA_ARGS__)
#define BS_CORE_INFO(...) ::Basalt::Log::GetCoreLogger().info(__VA_ARGS__)
#define BS_CORE_WARN(...) ::Basalt::Log::GetCoreLogger().warn(__VA_ARGS__)
#define BS_CORE_ERROR(...) ::Basalt::Log::GetCoreLogger().error(__VA_ARGS__)
#define BS_CORE_CRITICAL(...) ::Basalt::Log::GetCoreLogger().critical(__VA_ARGS__)

#define BS_TRACE(...) ::Basalt::Log::GetClientLogger().trace(__VA_ARGS__)
#define BS_INFO(...) ::Basalt::Log::GetClientLogger().info(__VA_ARGS__)
#define BS_WARN(...) ::Basalt::Log::GetClientLogger().warn(__VA_ARGS__)
#define BS_ERROR(...) ::Basalt::Log::GetClientLogger().error(__VA_ARGS__)
#define BS_CRITICAL(...) ::Basalt::Log::GetClientLogger().critical(__VA_ARGS__)
