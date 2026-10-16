#pragma once

#include "Basalt/Automation/McpServer.h"
#include "Basalt/Core/Base.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace Basalt {

	class AutomationEventHub;

	// Routes one TCP client's frames to legacy automation or MCP. Execute on the scene-owning thread;
	// only ShutdownResponse and TakePendingEvents may run concurrently (on the server thread): the first reads
	// an atomic protocol snapshot, the second the thread-safe event hub. Event subscriptions the client creates
	// end with the connection; a legacy client's stream to it, an MCP client's are polled.
	class AutomationConnection
	{
	public:
		AutomationConnection();
		~AutomationConnection();
		AutomationConnection(const AutomationConnection&) = delete;
		AutomationConnection& operator=(const AutomationConnection&) = delete;

		std::string HandleRequest(const CommandRegistry& registry, AutomationSession& session, const std::string& line);
		std::string ShutdownResponse(const std::string& line) const;
		// Event frames queued for this connection's subscriptions (newline-terminated), or "".
		std::string TakePendingEvents();

	private:
		enum class Protocol
		{
			Undetermined,
			Legacy,
			Mcp
		};
		Protocol SelectProtocol(const std::string& line, const nlohmann::json& parsed) const;
		void BindEvents(AutomationSession& session);
		std::atomic<Protocol> m_Protocol = Protocol::Undetermined;
		McpServer m_Mcp;
		// Owner of this connection's event subscriptions in the session's hub.
		const uint64_t m_ClientId;
		// The hub of the session that handled the last request; read on the server thread.
		std::mutex m_EventsMutex;
		Ref<AutomationEventHub> m_Events;
	};

}
