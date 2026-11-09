#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Breccia {

	// A transport's handle for one connection; it never reuses one while that transport lives.
	using NetPeerHandle = uint32_t;

	enum class NetChannel : uint8_t
	{
		// Delivered once, in order (messages that must arrive: Hello, Welcome).
		Reliable = 0,
		// May be lost or arrive out of order (state the next message repeats: snapshots, updates).
		Unreliable,
		Count
	};

	struct NetTransportEvent
	{
		enum class Kind : uint8_t
		{
			Connected,
			// The connection ended from the other side, or failed. Never reported for a Disconnect called locally.
			Disconnected,
			Received
		};

		Kind Type = Kind::Received;
		NetPeerHandle Peer = 0;
		// Disconnected: the code the other side gave (NetDisconnectReason), 0 when it gave none.
		uint32_t DisconnectCode = 0;
		// Disconnected: nothing was heard from the peer for the timeout, or a connection attempt was never answered.
		bool TimedOut = false;
		NetChannel Channel = NetChannel::Reliable;
		std::vector<uint8_t> Data;
	};

	// Moves datagrams between connected peers for a NetworkSystem: either real UDP (EnetTransport) or an in-process link
	// with simulated latency and loss (SimulatedNetwork), so the session above is tested exactly as it runs. A host
	// transport accepts connections; a client transport makes one. Everything is non-blocking and runs on the thread
	// that owns the transport.
	class NetTransport
	{
	public:
		virtual ~NetTransport() = default;

		// Queues a message; dropped when the peer is unknown or gone. Reliable messages arrive in order; unreliable ones
		// may not arrive, or arrive late (the receiver orders them).
		virtual void Send(NetPeerHandle peer, NetChannel channel, std::span<const uint8_t> data) = 0;
		// Ends a connection: queued reliable messages are still delivered, then the peer gets a Disconnected event with
		// the code. The handle is dead at once on this side (no event follows here).
		virtual void Disconnect(NetPeerHandle peer, uint32_t code) = 0;
		// Sends what is queued, receives, and appends what happened since the last call to `events`.
		virtual void Poll(std::vector<NetTransportEvent>& events) = 0;
		// Sends what is queued now (before a shutdown, so final messages leave).
		virtual void Flush() = 0;
		// Smoothed round-trip time to the peer in seconds (0 when unknown).
		virtual float GetRoundTripTime(NetPeerHandle peer) const = 0;
		// The peer's address for messages ("127.0.0.1:7777").
		virtual std::string DescribePeer(NetPeerHandle peer) const = 0;
		// The port a host transport listens on (the one the system picked when asked for 0); 0 for a client.
		virtual uint16_t GetListenPort() const = 0;
	};

}
