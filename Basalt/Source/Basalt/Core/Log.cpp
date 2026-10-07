#include "Basalt/Core/Log.h"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace Basalt {

	namespace {

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

		// Forwards formatted messages into the LogHistory ring buffer.
		class HistorySink final : public spdlog::sinks::base_sink<std::mutex>
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
				m_History.Push(std::move(message));
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

	LogHistory::LogHistory(size_t capacity)
		: m_Capacity(capacity > 0 ? capacity : 1)
	{
		m_Messages.reserve(m_Capacity);
	}

	void LogHistory::Push(LogMessage message)
	{
		std::scoped_lock lock(m_Mutex);
		if (message.Level >= LogLevel::Error)
			m_ErrorCount++;

		if (m_Messages.size() < m_Capacity)
		{
			m_Messages.push_back(std::move(message));
		}
		else
		{
			m_Messages[m_Head] = std::move(message);
			m_Head = (m_Head + 1) % m_Capacity;
		}
		m_TotalCount++;
	}

	void LogHistory::Clear()
	{
		std::scoped_lock lock(m_Mutex);
		m_Messages.clear();
		m_Head = 0;
		m_ErrorCount = 0;
		// m_TotalCount keeps increasing so "since" cursors held by readers stay valid.
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
