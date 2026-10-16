#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace Basalt {

	// Line-based TCP server for legacy automation and MCP, bound to 127.0.0.1 only. Each line received is a
	// JSON request; a non-empty handler result is sent back followed by a newline. A client may also be sent
	// frames it did not ask for (event subscriptions). Clients are served one at a time on a background
	// thread; the handler decides how to reach the main thread.
	//
	// Example (shell): echo '{"command":"scene.info"}' | nc 127.0.0.1 7420
	class AutomationServer
	{
	public:
		using RequestHandler = std::function<std::string(const std::string& request)>;

		// How long the server waits for a client between checks for stop and for unsolicited frames.
		static constexpr int IdlePollMilliseconds = 100;
		// A client that accepts no data (responses or event frames) for this long, e.g. one that stopped reading its
		// event stream, is disconnected, so it cannot block the server thread or Stop().
		static constexpr int DefaultSendTimeoutMilliseconds = 5000;

		// One client's callbacks, both run on the server thread.
		struct ClientHandler
		{
			// Required.
			RequestHandler OnRequest;
			// Optional: newline-terminated frames to send unprompted, or "". Called after every request line and
			// every IdlePollMilliseconds while the client is idle.
			std::function<std::string()> TakePending;
		};

		using HandlerFactory = std::function<ClientHandler()>;

		static constexpr uint16_t DefaultPort = 7420;

		AutomationServer() = default;
		~AutomationServer();

		AutomationServer(const AutomationServer&) = delete;
		AutomationServer& operator=(const AutomationServer&) = delete;

		// Port 0 picks a free port (see GetPort).
		bool Start(uint16_t port, RequestHandler handler, std::string& outError);
		// Called on the server thread once per accepted client; the returned handler owns connection state.
		bool StartWithFactory(uint16_t port, HandlerFactory factory, std::string& outError);
		void Stop();

		// For clients accepted from now on (tests shorten it).
		void SetSendTimeout(int milliseconds) { m_SendTimeoutMilliseconds = milliseconds; }

		bool IsRunning() const { return m_Running; }
		uint16_t GetPort() const { return m_Port; }
		// True while Stop() is in progress; long-running handlers should give up when this is set.
		bool IsStopping() const { return m_Stopping; }

	private:
		void Run();
		void ServeClient(intptr_t client, const ClientHandler& handler);

	private:
		HandlerFactory m_Factory;
		std::thread m_Thread;
		std::atomic<bool> m_Running = false;
		std::atomic<bool> m_Stopping = false;
		intptr_t m_ListenSocket = -1;
		uint16_t m_Port = 0;
		std::atomic<int> m_SendTimeoutMilliseconds = DefaultSendTimeoutMilliseconds;
	};

}
