#include "Basalt/Core/Log.h"

#include "Basalt/Core/EnumNames.h"

#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <cstdio>
#include <exception>
#include <iterator>

namespace Basalt {

	namespace {

		constexpr const char* const LevelNames[] = { "trace", "info", "warn", "error", "critical" };
		static_assert(std::size(LevelNames) == static_cast<size_t>(LogLevel::Critical) + 1, "one name per LogLevel");

		// Set while this thread runs listeners (of any history): what a listener logs is stored but not passed to
		// listeners again, which would re-enter them and their lock.
		thread_local bool s_InListener = false;

		class ListenerScope
		{
		public:
			ListenerScope() { s_InListener = true; }
			~ListenerScope() { s_InListener = false; }
			ListenerScope(const ListenerScope&) = delete;
			ListenerScope& operator=(const ListenerScope&) = delete;
		};

		LogLevel ToLogLevel(spdlog::level::level_enum level)
		{
			switch (level)
			{
				case spdlog::level::trace:
				case spdlog::level::debug:
					return LogLevel::Trace;
				case spdlog::level::info:
					return LogLevel::Info;
				case spdlog::level::warn:
					return LogLevel::Warn;
				case spdlog::level::err:
					return LogLevel::Error;
				case spdlog::level::critical:
					return LogLevel::Critical;
				default:
					return LogLevel::Info;
			}
		}

		// Forwards formatted messages into the LogHistory ring buffer. LogHistory locks for itself, so the sink takes no
		// lock of its own: a history listener may then log (re-entering this sink) without deadlocking.
		class HistorySink final : public spdlog::sinks::base_sink<spdlog::details::null_mutex>
		{
		public:
			explicit HistorySink(LogHistory& history)
				: m_History(history)
			{
			}

		protected:
			void sink_it_(const spdlog::details::log_msg& msg) override
			{
				LogMessage message;
				message.Level = ToLogLevel(msg.level);
				message.LoggerName = std::string(msg.logger_name.data(), msg.logger_name.size());
				message.Text = std::string(msg.payload.data(), msg.payload.size());
				m_History.Push(message);
			}

			void flush_() override {}

		private:
			LogHistory& m_History;
		};

		std::mutex s_InitMutex;
		std::shared_ptr<spdlog::logger> s_CoreLogger;
		std::shared_ptr<spdlog::logger> s_ClientLogger;
		spdlog::sink_ptr s_ConsoleSink;
		LogHistory s_History;

	}

	const char* LogLevelName(LogLevel level)
	{
		const char* name = EnumNameOrNull(LevelNames, level);
		return name ? name : "unknown";
	}

	std::optional<LogLevel> LogLevelFromName(std::string_view name)
	{
		return EnumFromName<LogLevel>(LevelNames, name);
	}

	LogHistory::LogHistory(size_t capacity)
		: m_Capacity(capacity > 0 ? capacity : 1)
	{
		m_Messages.reserve(m_Capacity);
	}

	void LogHistory::Push(const LogMessage& message)
	{
		{
			std::scoped_lock lock(m_Mutex);
			if (message.Level >= LogLevel::Error)
				m_ErrorCount++;

			if (m_Messages.size() < m_Capacity)
			{
				m_Messages.push_back(message);
			}
			else
			{
				m_Messages[m_Head] = message;
				m_Head = (m_Head + 1) % m_Capacity;
			}
			m_TotalCount++;
		}
		// After storing, so a failing listener cannot lose the message for log.get and error counts.
		if (s_InListener)
			return;
		const ListenerScope scope;
		std::scoped_lock listenerLock(m_ListenerMutex);
		for (const auto& [id, listener] : m_Listeners)
		{
			std::string failure;
			try
			{
				listener(message);
			}
			catch (const std::exception& e)
			{
				failure = e.what();
			}
			catch (...)
			{
				failure = "unknown exception";
			}
			if (failure.empty())
				continue;
			// Stored like any message (the scope keeps it from the listeners); stderr too, for hosts without a console.
			std::fprintf(stderr, "LogHistory: a log listener failed: %s\n", failure.c_str());
			Push({ LogLevel::Error, "BASALT", "LogHistory: a log listener failed: " + failure });
		}
	}

	void LogHistory::Clear()
	{
		std::scoped_lock lock(m_Mutex);
		m_Messages.clear();
		m_Head = 0;
		m_ErrorCount = 0;
		// m_TotalCount keeps increasing so "since" cursors held by readers stay valid.
	}

	uint64_t LogHistory::AddListener(Listener listener)
	{
		std::scoped_lock lock(m_ListenerMutex);
		const uint64_t id = m_NextListenerId++;
		m_Listeners.emplace_back(id, std::move(listener));
		return id;
	}

	void LogHistory::RemoveListener(uint64_t id)
	{
		std::scoped_lock lock(m_ListenerMutex);
		std::erase_if(m_Listeners, [id](const auto& entry) { return entry.first == id; });
	}

	std::vector<LogMessage> LogHistory::GetMessagesSince(uint64_t sinceIndex, uint64_t& outNextIndex) const
	{
		std::scoped_lock lock(m_Mutex);
		outNextIndex = m_TotalCount;

		const uint64_t stored = m_Messages.size();
		const uint64_t firstStoredIndex = m_TotalCount - stored;
		const uint64_t start = sinceIndex > firstStoredIndex ? sinceIndex : firstStoredIndex;

		std::vector<LogMessage> result;
		if (start >= m_TotalCount)
			return result;

		result.reserve(static_cast<size_t>(m_TotalCount - start));
		for (uint64_t i = start; i < m_TotalCount; i++)
		{
			// Oldest stored message lives at m_Head once the buffer has wrapped.
			const size_t offset = static_cast<size_t>(i - firstStoredIndex);
			const size_t slot = m_Messages.size() < m_Capacity ? offset : (m_Head + offset) % m_Capacity;
			result.push_back(m_Messages[slot]);
		}
		return result;
	}

	std::vector<LogMessage> LogHistory::GetAll() const
	{
		uint64_t next = 0;
		return GetMessagesSince(0, next);
	}

	uint64_t LogHistory::GetTotalCount() const
	{
		std::scoped_lock lock(m_Mutex);
		return m_TotalCount;
	}

	size_t LogHistory::GetErrorCount() const
	{
		std::scoped_lock lock(m_Mutex);
		return m_ErrorCount;
	}

	void Log::Init(const std::string& logFilePath)
	{
		std::scoped_lock lock(s_InitMutex);
		if (s_CoreLogger)
			return;

		std::vector<spdlog::sink_ptr> sinks;
		auto consoleSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
		consoleSink->set_pattern("%^[%T] %n: %v%$");
		s_ConsoleSink = consoleSink;
		sinks.push_back(consoleSink);

		bool fileSinkFailed = false;
		if (!logFilePath.empty())
		{
			try
			{
				auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath, true);
				fileSink->set_pattern("[%T] [%l] %n: %v");
				sinks.push_back(fileSink);
			}
			catch (const spdlog::spdlog_ex&)
			{
				fileSinkFailed = true;
			}
		}

		sinks.push_back(std::make_shared<HistorySink>(s_History));

		s_CoreLogger = std::make_shared<spdlog::logger>("BASALT", sinks.begin(), sinks.end());
		s_CoreLogger->set_level(spdlog::level::trace);
		s_CoreLogger->flush_on(spdlog::level::warn);

		s_ClientLogger = std::make_shared<spdlog::logger>("APP", sinks.begin(), sinks.end());
		s_ClientLogger->set_level(spdlog::level::trace);
		s_ClientLogger->flush_on(spdlog::level::warn);

		if (fileSinkFailed)
			s_CoreLogger->warn("Log: cannot open log file '{}'; logging to console only", logFilePath);
	}

	void Log::Shutdown()
	{
		// Loggers stay alive until process exit: other threads (drivers, job systems) may still log, and
		// destroying the loggers here would race with them. Shutdown only guarantees everything is flushed.
		std::scoped_lock lock(s_InitMutex);
		if (s_CoreLogger)
			s_CoreLogger->flush();
		if (s_ClientLogger)
			s_ClientLogger->flush();
	}

	void Log::SetConsoleLevel(LogLevel level)
	{
		EnsureInitialized();
		std::scoped_lock lock(s_InitMutex);
		switch (level)
		{
			case LogLevel::Trace:
				s_ConsoleSink->set_level(spdlog::level::trace);
				break;
			case LogLevel::Info:
				s_ConsoleSink->set_level(spdlog::level::info);
				break;
			case LogLevel::Warn:
				s_ConsoleSink->set_level(spdlog::level::warn);
				break;
			case LogLevel::Error:
				s_ConsoleSink->set_level(spdlog::level::err);
				break;
			case LogLevel::Critical:
				s_ConsoleSink->set_level(spdlog::level::critical);
				break;
		}
	}

	bool Log::IsInitialized()
	{
		std::scoped_lock lock(s_InitMutex);
		return s_CoreLogger != nullptr;
	}

	void Log::EnsureInitialized()
	{
		if (!IsInitialized())
			Init();
	}

	spdlog::logger& Log::GetCoreLogger()
	{
		EnsureInitialized();
		return *s_CoreLogger;
	}

	spdlog::logger& Log::GetClientLogger()
	{
		EnsureInitialized();
		return *s_ClientLogger;
	}

	LogHistory& Log::GetHistory()
	{
		return s_History;
	}

}
