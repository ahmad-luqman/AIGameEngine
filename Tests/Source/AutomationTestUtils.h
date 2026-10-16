#pragma once

#include <Basalt/Automation/AutomationSession.h>
#include <Basalt/Automation/CommandRegistry.h>
#include <Basalt/Core/JsonUtils.h>

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#if defined(_WIN32)
	#include <winsock2.h>
	#include <ws2tcpip.h>
#else
	#include <arpa/inet.h>
	#include <netinet/in.h>
	#include <sys/socket.h>
	#include <sys/time.h>
	#include <unistd.h>
#endif

#include <cstdint>
#include <cstring>
#include <string>

namespace BasaltTest {

	// Runs one command and returns its result; fails the test with the error message otherwise.
	inline nlohmann::json Run(const Basalt::CommandRegistry& registry, Basalt::AutomationSession& session, const std::string& command, const nlohmann::json& params = nlohmann::json::object())
	{
		const nlohmann::json response = registry.HandleRequest(session, { { "id", 7 }, { "command", command }, { "params", params } });
		INFO(command << ": " << response.dump());
		REQUIRE(response["ok"] == true);
		CHECK(response["id"] == 7);
		return response["result"];
	}

	// Runs one command that must fail and returns its error.
	inline std::string RunError(const Basalt::CommandRegistry& registry, Basalt::AutomationSession& session, const std::string& command, const nlohmann::json& params = nlohmann::json::object())
	{
		const nlohmann::json response = registry.HandleRequest(session, { { "command", command }, { "params", params } });
		INFO(command << ": " << response.dump());
		REQUIRE(response["ok"] == false);
		return response["error"].get<std::string>();
	}

	inline bool Contains(const std::string& text, const std::string& part)
	{
		return text.find(part) != std::string::npos;
	}

	// A loopback TCP client of the automation server that keeps its connection open, so it also sees frames the server
	// sends unprompted (event streams). Reads time out after 5 seconds, so a missing frame fails instead of hanging.
	class LineClient
	{
	public:
		explicit LineClient(uint16_t port)
			: m_Socket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP))
		{
			sockaddr_in address;
			std::memset(&address, 0, sizeof(address));
			address.sin_family = AF_INET;
			address.sin_port = htons(port);
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			m_Connected = connect(m_Socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
#if defined(_WIN32)
			DWORD timeout = 5000;
			setsockopt(m_Socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
			timeval timeout{ 5, 0 };
			setsockopt(m_Socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
		}
		~LineClient() { Close(); }
		LineClient(const LineClient&) = delete;
		LineClient& operator=(const LineClient&) = delete;

		bool IsConnected() const { return m_Connected; }

		bool Send(const std::string& line)
		{
			const std::string payload = line + "\n";
			return m_Connected && send(m_Socket, payload.data(), static_cast<int>(payload.size()), 0) == static_cast<int>(payload.size());
		}

		// The next line, or "" when none arrives in time (or the connection closed).
		std::string ReadLine()
		{
			char buffer[4096];
			while (m_Buffer.find('\n') == std::string::npos)
			{
				const auto received = recv(m_Socket, buffer, sizeof(buffer), 0);
				if (received <= 0)
				{
					// A final line without a newline still counts.
					std::string rest = std::move(m_Buffer);
					m_Buffer.clear();
					return rest;
				}
				m_Buffer.append(buffer, static_cast<size_t>(received));
			}
			const size_t newline = m_Buffer.find('\n');
			std::string line = m_Buffer.substr(0, newline);
			m_Buffer.erase(0, newline + 1);
			return line;
		}

		// The next frame that satisfies the predicate (skipping others, e.g. log lines), or null after 50 frames or a
		// timeout.
		template<typename Predicate>
		nlohmann::json ReadFrame(Predicate&& predicate)
		{
			for (int i = 0; i < 50; i++)
			{
				const std::string line = ReadLine();
				if (line.empty())
					return nullptr;
				nlohmann::json frame = Basalt::ParseJson(line);
				if (predicate(frame))
					return frame;
			}
			return nullptr;
		}

		void Close()
		{
			if (m_Closed)
				return;
			m_Closed = true;
#if defined(_WIN32)
			closesocket(m_Socket);
#else
			close(m_Socket);
#endif
		}

	private:
#if defined(_WIN32)
		SOCKET m_Socket;
#else
		int m_Socket;
#endif
		std::string m_Buffer;
		bool m_Connected = false;
		bool m_Closed = false;
	};

	// Sends one line on a new connection and returns the first line back.
	inline std::string SendLine(uint16_t port, const std::string& line)
	{
		LineClient client(port);
		if (!client.Send(line))
			return "";
		return client.ReadLine();
	}

}
