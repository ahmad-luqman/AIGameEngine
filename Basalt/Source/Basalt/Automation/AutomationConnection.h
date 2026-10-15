#pragma once

#include "Basalt/Automation/McpServer.h"

#include <atomic>
#include <string>

namespace Basalt {

	// Routes one TCP client's frames to legacy automation or MCP. Execute on the scene-owning thread;
	// only ShutdownResponse may run concurrently, and it reads an atomic protocol snapshot.
	class AutomationConnection
	{
	public:
		std::string HandleRequest(const CommandRegistry& registry, AutomationSession& session, const std::string& line);
		std::string ShutdownResponse(const std::string& line) const;

	private:
		enum class Protocol
		{
			Undetermined,
			Legacy,
			Mcp
		};
		Protocol SelectProtocol(const std::string& line, const nlohmann::json& parsed) const;
		std::atomic<Protocol> m_Protocol = Protocol::Undetermined;
		McpServer m_Mcp;
	};

}
