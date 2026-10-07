#include "Basalt/Automation/AutomationServer.h"

#include "Basalt/Core/Base.h"
#include "Basalt/Core/Log.h"

#if defined(BS_PLATFORM_WINDOWS)
	#include <winsock2.h>
	#include <ws2tcpip.h>
using SocketHandle = SOCKET;
static const SocketHandle s_InvalidSocket = INVALID_SOCKET;
#else
	#include <arpa/inet.h>
	#include <netinet/in.h>
	#include <sys/select.h>
	#include <sys/socket.h>
	#include <unistd.h>
using SocketHandle = int;
static const SocketHandle s_InvalidSocket = -1;
#endif

#include <cstring>

namespace Basalt {

	namespace {

		SocketHandle ToSocket(intptr_t value)
		{
			return static_cast<SocketHandle>(value);
		}

		void CloseSocket(SocketHandle socket)
		{
#if defined(BS_PLATFORM_WINDOWS)
			closesocket(socket);
#else
			close(socket);
#endif
		}

		// Waits until the socket is readable or the timeout passes. Returns >0 readable, 0 timeout, <0 error.
		int WaitReadable(SocketHandle socket, int timeoutMilliseconds)
		{
			fd_set set;
			FD_ZERO(&set);
			FD_SET(socket, &set);
			timeval timeout;
			timeout.tv_sec = timeoutMilliseconds / 1000;
			timeout.tv_usec = (timeoutMilliseconds % 1000) * 1000;
			return select(static_cast<int>(socket) + 1, &set, nullptr, nullptr, &timeout);
		}

		bool SendAll(SocketHandle socket, const std::string& data)
		{
			size_t sent = 0;
			while (sent < data.size())
			{
#if defined(BS_PLATFORM_WINDOWS)
				const int result = send(socket, data.data() + sent, static_cast<int>(data.size() - sent), 0);
#elif defined(BS_PLATFORM_LINUX)
				const ssize_t result = send(socket, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#else
				const ssize_t result = send(socket, data.data() + sent, data.size() - sent, 0);
#endif
				if (result <= 0)
					return false;
				sent += static_cast<size_t>(result);
			}
			return true;
		}

	}

	AutomationServer::~AutomationServer()
	{
		Stop();
	}

	bool AutomationServer::Start(uint16_t port, RequestHandler handler, std::string& outError)
	{
		if (m_Running)
		{
			outError = "server already running";
			return false;
		}

#if defined(BS_PLATFORM_WINDOWS)
		WSADATA wsaData;
		if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		{
			outError = "WSAStartup failed";
			return false;
		}
#endif

		SocketHandle listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listenSocket == s_InvalidSocket)
		{
			outError = "cannot create socket";
			return false;
		}

		int reuse = 1;
#if defined(BS_PLATFORM_WINDOWS)
		// On Windows SO_REUSEADDR would let another process bind (hijack) the same port; demand exclusivity.
		setsockopt(listenSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
		// Allows restarting the editor immediately while old connections linger in TIME_WAIT.
		setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#endif
#if defined(BS_PLATFORM_MACOS)
		setsockopt(listenSocket, SOL_SOCKET, SO_NOSIGPIPE, &reuse, sizeof(reuse));
#endif

		sockaddr_in address;
		std::memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_port = htons(port);
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // never reachable from other machines

		if (bind(listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listenSocket, 4) != 0)
		{
			CloseSocket(listenSocket);
			outError = "cannot listen on 127.0.0.1:" + std::to_string(port) + " (port in use?)";
			return false;
		}

		socklen_t length = sizeof(address);
		getsockname(listenSocket, reinterpret_cast<sockaddr*>(&address), &length);
		m_Port = ntohs(address.sin_port);
		m_ListenSocket = static_cast<intptr_t>(listenSocket);
		m_Handler = std::move(handler);
		m_Stopping = false;
		m_Running = true;
		m_Thread = std::thread([this]() { Run(); });
		BS_CORE_INFO("Automation server listening on 127.0.0.1:{}", m_Port);
		return true;
	}

	void AutomationServer::Stop()
	{
		if (!m_Running)
			return;
		m_Stopping = true;
		if (m_Thread.joinable())
			m_Thread.join();
		CloseSocket(ToSocket(m_ListenSocket));
		m_ListenSocket = -1;
		m_Running = false;
#if defined(BS_PLATFORM_WINDOWS)
		WSACleanup();
#endif
	}

	void AutomationServer::Run()
	{
		const SocketHandle listenSocket = ToSocket(m_ListenSocket);
		while (!m_Stopping)
		{
			if (WaitReadable(listenSocket, 100) <= 0)
				continue;
			SocketHandle client = accept(listenSocket, nullptr, nullptr);
			if (client == s_InvalidSocket)
				continue;
#if defined(BS_PLATFORM_MACOS)
			int noSigPipe = 1;
			setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
#endif
			ServeClient(static_cast<intptr_t>(client));
			CloseSocket(client);
		}
	}

	void AutomationServer::ServeClient(intptr_t clientHandle)
	{
		const SocketHandle client = ToSocket(clientHandle);
		constexpr size_t MaxLineBytes = 64 * 1024 * 1024;
		std::string buffer;
		char chunk[4096];

		while (!m_Stopping)
		{
			const int ready = WaitReadable(client, 100);
			if (ready < 0)
				return;
			if (ready == 0)
				continue;

			const auto received = recv(client, chunk, sizeof(chunk), 0);
			if (received <= 0)
				return; // closed or error
			buffer.append(chunk, static_cast<size_t>(received));
			if (buffer.size() > MaxLineBytes)
			{
				SendAll(client, "{\"ok\":false,\"error\":\"request too large\"}\n");
				return;
			}

			size_t newline;
			while ((newline = buffer.find('\n')) != std::string::npos)
			{
				std::string line = buffer.substr(0, newline);
				buffer.erase(0, newline + 1);
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (line.empty())
					continue;
				if (!SendAll(client, m_Handler(line) + "\n"))
					return;
			}
		}
	}

}
