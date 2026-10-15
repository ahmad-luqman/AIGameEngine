#pragma once

#include <nlohmann/json.hpp>

#include <iosfwd>
#include <optional>
#include <string_view>

namespace Basalt {

	class AutomationSession;
	class CommandRegistry;

	// One MCP connection's protocol state. Hosts own framing and dispatch on their scene-owning thread.
	// Tool definitions and validation both come from CommandRegistry; notifications never run tools.
	class McpServer
	{
	public:
		static constexpr std::string_view ProtocolVersion = "2025-11-25";
		std::optional<nlohmann::json> HandleRequest(const CommandRegistry& registry, AutomationSession& session, const nlohmann::json& request);
		// Stops on EOF or an oversized frame. Protocol/tool errors are replies, not process failures.
		bool Serve(std::istream& input, std::ostream& output, const CommandRegistry& registry, AutomationSession& session);
		void Reset() { m_State = State::New; }
		static nlohmann::json Error(const nlohmann::json& id, int code, const std::string& message);

	private:
		enum class State
		{
			New,
			Initializing,
			Ready
		};
		State m_State = State::New;
	};

}
