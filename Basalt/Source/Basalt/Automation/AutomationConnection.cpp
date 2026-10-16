#include "Basalt/Automation/AutomationConnection.h"

#include "Basalt/Automation/AutomationEvents.h"
#include "Basalt/Automation/AutomationSession.h"
#include "Basalt/Automation/CommandRegistry.h"
#include "Basalt/Core/JsonUtils.h"

namespace Basalt {

	namespace {

		std::atomic<uint64_t> s_NextClientId = 1;

	}

	AutomationConnection::AutomationConnection()
		: m_ClientId(s_NextClientId.fetch_add(1))
	{
	}

	AutomationConnection::~AutomationConnection()
	{
		std::scoped_lock lock(m_EventsMutex);
		if (m_Events)
			m_Events->RemoveOwner(m_ClientId);
	}

	void AutomationConnection::BindEvents(AutomationSession& session)
	{
		std::scoped_lock lock(m_EventsMutex);
		if (m_Events == session.GetEvents())
			return;
		// Requests are handled with the session they arrive for; a connection moved to another session (never the
		// case in one editor) must not strand its subscriptions in the old hub.
		if (m_Events)
			m_Events->RemoveOwner(m_ClientId);
		m_Events = session.GetEvents();
	}

	std::string AutomationConnection::TakePendingEvents()
	{
		std::scoped_lock lock(m_EventsMutex);
		return m_Events ? m_Events->TakeStreamed(m_ClientId) : std::string();
	}

	AutomationConnection::Protocol AutomationConnection::SelectProtocol(const std::string& line, const nlohmann::json& parsed) const
	{
		const Protocol protocol = m_Protocol.load();
		if (protocol != Protocol::Undetermined)
			return protocol;
		if ((parsed.is_object() && (parsed.contains("jsonrpc") || parsed.contains("method"))) ||
			(parsed.is_discarded() && (line.find("\"jsonrpc\"") != std::string::npos || line.find("\"method\"") != std::string::npos)))
			return Protocol::Mcp;
		// Invalid JSON and blank frames cannot identify a legacy client. Leave detection open.
		return parsed.is_discarded() ? Protocol::Undetermined : Protocol::Legacy;
	}

	std::string AutomationConnection::HandleRequest(const CommandRegistry& registry, AutomationSession& session, const std::string& line)
	{
		const nlohmann::json parsed = ParseJson(line);
		const Protocol protocol = SelectProtocol(line, parsed);
		m_Protocol.store(protocol);
		BindEvents(session);
		if (protocol == Protocol::Mcp)
		{
			// MCP has no frames for unsolicited events: its subscriptions are polled, but still end with the connection.
			AutomationSession::EventClientScope client(session, { m_ClientId, AutomationEventDelivery::Polled });
			const auto response = m_Mcp.HandleRequest(registry, session, parsed);
			return response ? response->dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) : "";
		}
		if (line.empty())
			return "";
		if (parsed.is_discarded())
			return R"({"ok":false,"error":"invalid JSON"})";
		AutomationSession::EventClientScope client(session, { m_ClientId, AutomationEventDelivery::Streamed });
		return registry.HandleRequest(session, parsed).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
	}

	std::string AutomationConnection::ShutdownResponse(const std::string& line) const
	{
		const nlohmann::json parsed = ParseJson(line);
		if (SelectProtocol(line, parsed) == Protocol::Mcp)
		{
			// Shutdown cannot turn a notification or a client response into a server reply.
			if (parsed.is_object() && (!parsed.contains("id") || (!parsed.contains("method") && (parsed.contains("result") || parsed.contains("error")))))
				return "";
			const nlohmann::json id = parsed.is_object() ? parsed.value("id", nlohmann::json()) : nlohmann::json();
			return McpServer::Error(id.is_string() || id.is_number_integer() ? id : nlohmann::json(), -32603, "editor is shutting down").dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
		}
		return R"({"ok":false,"error":"editor is shutting down"})";
	}

}
