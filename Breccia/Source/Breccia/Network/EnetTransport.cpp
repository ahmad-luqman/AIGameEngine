#include "Breccia/Network/EnetTransport.h"

#include "Breccia/Core/Log.h"

#include <enet/enet.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <unordered_set>

namespace Breccia {

	namespace {

		// enet_initialize and enet_deinitialize are process-wide (WSAStartup on Windows); every transport holds a use.
		std::mutex s_LibraryMutex;
		uint32_t s_LibraryUsers = 0;

		bool AcquireLibrary(std::string& outError)
		{
			std::scoped_lock lock(s_LibraryMutex);
			if (s_LibraryUsers == 0 && enet_initialize() != 0)
			{
				outError = "the network library could not start";
				return false;
			}
			s_LibraryUsers++;
			return true;
		}

		void ReleaseLibrary()
		{
			std::scoped_lock lock(s_LibraryMutex);
			if (--s_LibraryUsers == 0)
				enet_deinitialize();
		}

		std::string DescribeAddress(const ENetAddress& address)
		{
			char text[64] = {};
			if (enet_address_get_host_ip(&address, text, sizeof(text)) != 0)
				return "unknown address:" + std::to_string(address.port);
			return std::string(text) + ":" + std::to_string(address.port);
		}

		class EnetHostTransport final : public NetTransport
		{
		public:
			EnetHostTransport(ENetHost* host, const EnetTransportSettings& settings)
				: m_Host(host)
				, m_Settings(settings)
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
				ReleaseLibrary();
			}

			// The handle of a client transport's single connection, made before ENet reports it.
			NetPeerHandle AddOutgoing(ENetPeer* peer)
			{
				ApplyTimeout(peer);
				return Register(peer);
			}

			void Send(NetPeerHandle handle, NetChannel channel, std::span<const uint8_t> data) override
			{
				ENetPeer* peer = Find(handle);
				if (!peer || peer->state != ENET_PEER_STATE_CONNECTED)
					return;
				// Unreliable messages larger than a datagram travel as unreliable fragments: losing one loses the message,
				// which the next one repeats. Without the flag ENet would send them reliably.
				const enet_uint32 flags = channel == NetChannel::Reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
				ENetPacket* packet = enet_packet_create(data.data(), data.size(), flags);
				if (!packet)
				{
					BR_CORE_ERROR("Network: out of memory for a {}-byte message to {}", data.size(), DescribePeer(handle));
					return;
				}
				if (enet_peer_send(peer, static_cast<enet_uint8>(channel), packet) != 0)
				{
					// A packet ENet did not queue is still ours.
					enet_packet_destroy(packet);
					BR_CORE_ERROR("Network: could not send a {}-byte message to {} (the limit is {} bytes)", data.size(), DescribePeer(handle), m_Settings.MaxMessageBytes);
				}
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

			void Poll(std::vector<NetTransportEvent>& events) override
			{
				ENetEvent event;
				for (;;)
				{
					const int result = enet_host_service(m_Host, &event, 0);
					if (result < 0)
					{
						if (!m_ReportedServiceError)
							BR_CORE_ERROR("Network: the UDP socket reported an error; messages may be lost");
						m_ReportedServiceError = true;
						break;
					}
					if (result == 0)
						break;
					Handle(event, events);
				}
			}

			void Flush() override { enet_host_flush(m_Host); }

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
			void SetListenPort(uint16_t port) { m_ListenPort = port; }

		private:
			static constexpr uintptr_t NoHandle = 0;

			void ApplyTimeout(ENetPeer* peer) const
			{
				const auto milliseconds = static_cast<enet_uint32>(std::clamp(m_Settings.TimeoutSeconds, 0.1f, 3600.0f) * 1000.0f);
				enet_peer_timeout(peer, 0, milliseconds, milliseconds);
			}

			NetPeerHandle Register(ENetPeer* peer)
			{
				const NetPeerHandle handle = m_NextHandle++;
				peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));
				m_Peers[handle] = peer;
				return handle;
			}

			void Unregister(NetPeerHandle handle, ENetPeer* peer)
			{
				peer->data = reinterpret_cast<void*>(NoHandle);
				m_Peers.erase(handle);
			}

			ENetPeer* Find(NetPeerHandle handle) const
			{
				auto it = m_Peers.find(handle);
				return it != m_Peers.end() ? it->second : nullptr;
			}

			static NetPeerHandle HandleOf(const ENetPeer* peer) { return static_cast<NetPeerHandle>(reinterpret_cast<uintptr_t>(peer->data)); }

			void Handle(ENetEvent& event, std::vector<NetTransportEvent>& events)
			{
				switch (event.type)
				{
					case ENET_EVENT_TYPE_CONNECT:
					{
						NetPeerHandle handle = HandleOf(event.peer);
						// An incoming connection; an outgoing one was registered when it was made.
						if (handle == NoHandle)
						{
							ApplyTimeout(event.peer);
							handle = Register(event.peer);
						}
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

			ENetHost* m_Host = nullptr;
			EnetTransportSettings m_Settings;
			std::map<NetPeerHandle, ENetPeer*> m_Peers;
			NetPeerHandle m_NextHandle = 1;
			uint16_t m_ListenPort = 0;
			bool m_ReportedServiceError = false;
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
			if (!AcquireLibrary(outError))
				return nullptr;
			ENetAddress address{};
			address.host = ENET_HOST_ANY;
			address.port = port;
			ENetHost* host = enet_host_create(&address, maxPeers, static_cast<size_t>(NetChannel::Count), 0, 0);
			if (!host)
			{
				ReleaseLibrary();
				outError = "cannot listen on UDP port " + std::to_string(port) + " (is another program using it?)";
				return nullptr;
			}
			auto transport = CreateScope<EnetHostTransport>(host, settings);
			ENetAddress bound{};
			if (enet_socket_get_address(host->socket, &bound) != 0)
			{
				outError = "cannot read the port the UDP socket is bound to";
				return nullptr;
			}
			transport->SetListenPort(bound.port);
			return transport;
		}

		Scope<NetTransport> Connect(const std::string& hostName, uint16_t port, const EnetTransportSettings& settings, std::string& outError)
		{
			if (port == 0)
			{
				outError = "the host's port cannot be 0";
				return nullptr;
			}
			if (!AcquireLibrary(outError))
				return nullptr;
			ENetAddress address{};
			address.port = port;
			// A numeric address first, so the common case never waits on name resolution.
			if (enet_address_set_host_ip(&address, hostName.c_str()) != 0 && enet_address_set_host(&address, hostName.c_str()) != 0)
			{
				ReleaseLibrary();
				outError = "cannot resolve the host '" + hostName + "'";
				return nullptr;
			}
			ENetHost* host = enet_host_create(nullptr, 1, static_cast<size_t>(NetChannel::Count), 0, 0);
			if (!host)
			{
				ReleaseLibrary();
				outError = "cannot open a UDP socket";
				return nullptr;
			}
			auto transport = CreateScope<EnetHostTransport>(host, settings);
			ENetPeer* peer = enet_host_connect(host, &address, static_cast<size_t>(NetChannel::Count), 0);
			if (!peer)
			{
				outError = "cannot start a connection to " + DescribeAddress(address);
				return nullptr;
			}
			transport->AddOutgoing(peer);
			return transport;
		}

	}

}
