#pragma once

#include "Breccia/Core/Base.h"
#include "Breccia/Network/NetTransport.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace Breccia {

	// Limits of a real UDP transport.
	struct EnetTransportSettings
	{
		// A peer that acknowledges nothing for this long is disconnected (TimedOut), and so is an unanswered attempt.
		float TimeoutSeconds = 10.0f;
		// Largest message accepted or sent. A peer may leave at most twice this waiting to be reassembled, so it cannot make
		// the transport buffer more.
		size_t MaxMessageBytes = size_t{ 1024 } * 1024;
	};

	// UDP through ENet (IPv4): reliable and unreliable channels, connection handshakes, round-trip times and timeouts.
	// The process-wide ENet state (WSAStartup on Windows) lives while any transport does.
	namespace EnetTransport {

		// A host transport on every local IPv4 address. port 0 lets the system pick (GetListenPort tells which).
		// maxPeers is the most simultaneous connections the transport itself accepts. Null, with the reason, when the
		// port cannot be bound.
		Scope<NetTransport> Listen(uint16_t port, uint32_t maxPeers, const EnetTransportSettings& settings, std::string& outError);
		// A client transport connecting to host:port (an IPv4 address or a name, resolved now, which may block). Null,
		// with the reason, when the address does not resolve; otherwise the attempt reports Connected or a timed-out
		// Disconnected through Poll.
		Scope<NetTransport> Connect(const std::string& host, uint16_t port, const EnetTransportSettings& settings, std::string& outError);

	}

}
