#include "Breccia/Network/EnetTransport.h"

#include "Breccia/Core/FileSystem.h"
#include "Breccia/Core/Log.h"
#include "Breccia/Core/Platform.h"

#include <enet/enet.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <map>
#include <mutex>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace Breccia {

	namespace {

		// enet_initialize and enet_deinitialize are process-wide (WSAStartup on Windows); every transport holds a use.
		std::mutex s_LibraryMutex;
		uint32_t s_LibraryUsers = 0;

		// One use of the ENet library, released when destroyed (move-only).
		class LibraryUse
		{
		public:
			LibraryUse() = default;
			LibraryUse(LibraryUse&& other) noexcept
				: m_Held(std::exchange(other.m_Held, false))
			{
			}
			LibraryUse& operator=(LibraryUse&&) = delete;
			~LibraryUse()
			{
				if (!m_Held)
					return;
				std::scoped_lock lock(s_LibraryMutex);
				if (--s_LibraryUsers == 0)
					enet_deinitialize();
			}

			bool Acquire(std::string& outError)
			{
				if (m_Held)
					return true;
				std::scoped_lock lock(s_LibraryMutex);
				if (s_LibraryUsers == 0 && enet_initialize() != 0)
				{
					outError = "the network library could not start";
					return false;
				}
				s_LibraryUsers++;
				m_Held = true;
				return true;
			}

		private:
			bool m_Held = false;
		};

		std::string DescribeAddress(const ENetAddress& address)
		{
			char text[64] = {};
			if (enet_address_get_host_ip(&address, text, sizeof(text)) != 0)
				return "unknown address:" + std::to_string(address.port);
			return std::string(text) + ":" + std::to_string(address.port);
		}

		// The socket error ENet's last failed call left, for messages.
		std::string DescribeSocketError()
		{
#if defined(_WIN32)
			return Platform::GetErrorMessage(static_cast<uint32_t>(WSAGetLastError()));
#else
			return FileSystem::DescribeError(std::error_code(errno, std::generic_category()));
#endif
		}

		class EnetHostTransport final : public NetTransport
		{
		public:
			EnetHostTransport(LibraryUse library, ENetHost* host, const EnetTransportSettings& settings, uint16_t listenPort)
				: m_Library(std::move(library))
				, m_Host(host)
				, m_Settings(settings)
				, m_ListenPort(listenPort)
			{
				m_Host->maximumPacketSize = m_Settings.MaxMessageBytes;
				m_Host->maximumWaitingData = m_Settings.MaxMessageBytes * 2;
			}

			~EnetHostTransport() override
			{
				// A disconnect still waiting behind queued messages goes now, without them: the socket closes next, so this
				// is the only chance for the peer to learn the reason. Peers the session never disconnected time out.
				for (size_t i = 0; i < m_Host->peerCount; i++)
					if (ENetPeer& peer = m_Host->peers[i]; peer.state == ENET_PEER_STATE_DISCONNECT_LATER)
						enet_peer_disconnect(&peer, peer.eventData);
				enet_host_flush(m_Host);
				enet_host_destroy(m_Host);
			}

			// Registers a peer: an outgoing connection when it is made (before ENet reports it), an incoming one when ENet
			// reports it.
			NetPeerHandle Track(ENetPeer* peer)
			{
				const auto milliseconds = static_cast<enet_uint32>(std::clamp(m_Settings.TimeoutSeconds, 0.1f, 3600.0f) * 1000.0f);
				enet_peer_timeout(peer, 0, milliseconds, milliseconds);
				const NetPeerHandle handle = m_NextHandle++;
				peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));
				m_Peers[handle] = peer;
				return handle;
			}

			bool Send(NetPeerHandle handle, NetChannel channel, std::span<const uint8_t> data) override
			{
				ENetPeer* peer = Find(handle);
				if (!peer || peer->state != ENET_PEER_STATE_CONNECTED)
					return false;
				// Unreliable messages larger than a datagram travel as unreliable fragments: losing one loses the message,
				// which the next one repeats. Without the flag ENet would send them reliably.
				const enet_uint32 flags = channel == NetChannel::Reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
				ENetPacket* packet = enet_packet_create(data.data(), data.size(), flags);
				if (packet && enet_peer_send(peer, static_cast<enet_uint8>(channel), packet) == 0)
					return true;
				// A packet ENet did not queue is still ours.
				if (packet)
					enet_packet_destroy(packet);
				// Once per peer: a lasting cause would otherwise log at the tick rate.
				if (m_ReportedSendFailures.insert(handle).second)
				{
					const std::string reason = data.size() > m_Settings.MaxMessageBytes ? fmt::format("the limit is {} bytes", m_Settings.MaxMessageBytes) : "out of memory, or too many fragments queued";
					BR_CORE_ERROR("Network: could not send a {}-byte message to {} ({}); further failures to it are not logged", data.size(), DescribePeer(handle), reason);
				}
				return false;
			}

			void Disconnect(NetPeerHandle handle, uint32_t code) override
			{
				ENetPeer* peer = Find(handle);
				if (!peer)
					return;
				Unregister(handle, peer);
				// After queued messages; ENet drops the peer once acknowledged (or after its timeout).
				enet_peer_disconnect_later(peer, code);
			}

			void DisconnectAll(uint32_t code) override
			{
				// The map changes as peers go: take the handles first. A connection still being made (no reply yet) is
				// disconnected at once, which tells a host that already accepted it.
				while (!m_Peers.empty())
					Disconnect(m_Peers.begin()->first, code);
			}

			void Poll(std::vector<NetTransportEvent>& events) override
			{
				Service();
				events.insert(events.end(), std::make_move_iterator(m_Pending.begin()), std::make_move_iterator(m_Pending.end()));
				m_Pending.clear();
			}

			// enet_host_flush drops the result of the send it does, so a failing socket would go unreported: service the host
			// instead (which sends too), keeping what arrives for the next Poll.
			void Flush() override { Service(); }

			float GetRoundTripTime(NetPeerHandle handle) const override
			{
				const ENetPeer* peer = Find(handle);
				return peer ? static_cast<float>(peer->roundTripTime) / 1000.0f : 0.0f;
			}

			std::string DescribePeer(NetPeerHandle handle) const override
			{
				const ENetPeer* peer = Find(handle);
				return peer ? DescribeAddress(peer->address) : "a disconnected peer";
			}

			uint16_t GetListenPort() const override { return m_ListenPort; }

		private:
			static constexpr NetPeerHandle NoHandle = 0;

			void Unregister(NetPeerHandle handle, ENetPeer* peer)
			{
				peer->data = nullptr;
				m_Peers.erase(handle);
				m_ReportedSendFailures.erase(handle);
			}

			ENetPeer* Find(NetPeerHandle handle) const
			{
				auto it = m_Peers.find(handle);
				return it != m_Peers.end() ? it->second : nullptr;
			}

			static NetPeerHandle HandleOf(const ENetPeer* peer) { return static_cast<NetPeerHandle>(reinterpret_cast<uintptr_t>(peer->data)); }

			// Sends what is queued and receives into m_Pending. A send to one unreachable peer fails the whole call (ENet
			// stops there), and may do so every frame until that peer times out: the failures are logged at most every few
			// seconds, with their count.
			void Service()
			{
				ENetEvent event;
				for (;;)
				{
					const int result = enet_host_service(m_Host, &event, 0);
					if (result < 0)
					{
						m_ServiceErrors++;
						const auto now = std::chrono::steady_clock::now();
						if (m_ServiceErrors == 1 || now - m_LastServiceErrorLog >= ServiceErrorLogInterval)
						{
							BR_CORE_ERROR("Network: the UDP socket failed {} time(s) since the last report ({}); messages to or from some peers are lost until they recover or time out", m_ServiceErrors, DescribeSocketError());
							m_LastServiceErrorLog = now;
							m_ServiceErrors = 0;
						}
						break;
					}
					if (result == 0)
						break;
					Handle(event, m_Pending);
				}
			}

			void Handle(ENetEvent& event, std::vector<NetTransportEvent>& events)
			{
				switch (event.type)
				{
					case ENET_EVENT_TYPE_CONNECT:
					{
						NetPeerHandle handle = HandleOf(event.peer);
						// An incoming connection; an outgoing one was tracked when it was made.
						if (handle == NoHandle)
							handle = Track(event.peer);
						events.push_back({ NetTransportEvent::Kind::Connected, handle });
						break;
					}
					case ENET_EVENT_TYPE_DISCONNECT:
					{
						const NetPeerHandle handle = HandleOf(event.peer);
						// Disconnected here first: the session already forgot it.
						if (handle == NoHandle)
							break;
						Unregister(handle, event.peer);
						// Breccia peers always give a reason: 0 is ENet's own timeout (or an attempt nobody answered).
						events.push_back({ NetTransportEvent::Kind::Disconnected, handle, event.data, event.data == 0 });
						break;
					}
					case ENET_EVENT_TYPE_RECEIVE:
					{
						const NetPeerHandle handle = HandleOf(event.peer);
						if (handle != NoHandle && event.channelID < static_cast<enet_uint8>(NetChannel::Count))
						{
							NetTransportEvent received{ NetTransportEvent::Kind::Received, handle };
							received.Channel = static_cast<NetChannel>(event.channelID);
							received.Data.assign(event.packet->data, event.packet->data + event.packet->dataLength);
							events.push_back(std::move(received));
						}
						enet_packet_destroy(event.packet);
						break;
					}
					case ENET_EVENT_TYPE_NONE:
						break;
				}
			}

			// Declared first, destroyed last: the host goes before the library it needs.
			LibraryUse m_Library;
			ENetHost* m_Host = nullptr;
			EnetTransportSettings m_Settings;
			std::map<NetPeerHandle, ENetPeer*> m_Peers;
			std::unordered_set<NetPeerHandle> m_ReportedSendFailures;
			NetPeerHandle m_NextHandle = 1;
			uint16_t m_ListenPort = 0;
			static constexpr std::chrono::seconds ServiceErrorLogInterval{ 5 };
			uint32_t m_ServiceErrors = 0;
			std::chrono::steady_clock::time_point m_LastServiceErrorLog;
			// Events Flush received, which the next Poll returns first.
			std::vector<NetTransportEvent> m_Pending;
		};

	}

	namespace EnetTransport {

		Scope<NetTransport> Listen(uint16_t port, uint32_t maxPeers, const EnetTransportSettings& settings, std::string& outError)
		{
			if (maxPeers == 0 || maxPeers > ENET_PROTOCOL_MAXIMUM_PEER_ID)
			{
				outError = "a host accepts 1 to " + std::to_string(ENET_PROTOCOL_MAXIMUM_PEER_ID) + " connections";
				return nullptr;
			}
			LibraryUse library;
			if (!library.Acquire(outError))
				return nullptr;
			ENetAddress address{};
			address.host = ENET_HOST_ANY;
			address.port = port;
			ENetHost* host = enet_host_create(&address, maxPeers, static_cast<size_t>(NetChannel::Count), 0, 0);
			if (!host)
			{
				outError = "cannot listen on UDP port " + std::to_string(port) + " (" + DescribeSocketError() + "; is another program using it?)";
				return nullptr;
			}
			ENetAddress bound{};
			if (enet_socket_get_address(host->socket, &bound) != 0)
			{
				outError = "cannot read the port the UDP socket is bound to (" + DescribeSocketError() + ")";
				enet_host_destroy(host);
				return nullptr;
			}
			return CreateScope<EnetHostTransport>(std::move(library), host, settings, bound.port);
		}

		Scope<NetTransport> Connect(const std::string& hostName, uint16_t port, const EnetTransportSettings& settings, std::string& outError)
		{
			if (port == 0)
			{
				outError = "the host's port cannot be 0";
				return nullptr;
			}
			LibraryUse library;
			if (!library.Acquire(outError))
				return nullptr;
			ENetAddress address{};
			address.port = port;
			// A numeric address first, so the common case never waits on name resolution.
			if (enet_address_set_host_ip(&address, hostName.c_str()) != 0 && enet_address_set_host(&address, hostName.c_str()) != 0)
			{
				outError = "cannot resolve the host '" + hostName + "'";
				return nullptr;
			}
			ENetHost* host = enet_host_create(nullptr, 1, static_cast<size_t>(NetChannel::Count), 0, 0);
			if (!host)
			{
				outError = "cannot open a UDP socket (" + DescribeSocketError() + ")";
				return nullptr;
			}
			auto transport = CreateScope<EnetHostTransport>(std::move(library), host, settings, uint16_t{ 0 });
			ENetPeer* peer = enet_host_connect(host, &address, static_cast<size_t>(NetChannel::Count), 0);
			if (!peer)
			{
				outError = "cannot start a connection to " + DescribeAddress(address);
				return nullptr;
			}
			transport->Track(peer);
			return transport;
		}

	}

}
