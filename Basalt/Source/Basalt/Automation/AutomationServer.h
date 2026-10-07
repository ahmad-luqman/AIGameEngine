#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace Basalt {

	// Line-based TCP server for the automation protocol, bound to 127.0.0.1 only. Each line received is a
	// JSON request; the handler's return value is sent back followed by a newline. Clients are served one
	// at a time on a background thread; the handler decides how to reach the main thread.
	//
	// Example (shell): echo '{"command":"scene.info"}' | nc 127.0.0.1 7420
	class AutomationServer
	{
	public:
		using RequestHandler = std::function<std::string(const std::string& request)>;

		static constexpr uint16_t DefaultPort = 7420;

		AutomationServer() = default;
		~AutomationServer();

		AutomationServer(const AutomationServer&) = delete;
		AutomationServer& operator=(const AutomationServer&) = delete;

		// Port 0 picks a free port (see GetPort).
		bool Start(uint16_t port, RequestHandler handler, std::string& outError);
		void Stop();

		bool IsRunning() const { return m_Running; }
		uint16_t GetPort() const { return m_Port; }
		// True while Stop() is in progress; long-running handlers should give up when this is set.
		bool IsStopping() const { return m_Stopping; }

	private:
		void Run();
		void ServeClient(intptr_t client);

	private:
		RequestHandler m_Handler;
		std::thread m_Thread;
		std::atomic<bool> m_Running = false;
		std::atomic<bool> m_Stopping = false;
		intptr_t m_ListenSocket = -1;
		uint16_t m_Port = 0;
	};

}
