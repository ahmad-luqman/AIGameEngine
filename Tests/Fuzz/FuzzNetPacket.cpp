// Network messages: every datagram a host or client receives comes from the network, so it is untrusted input. The first
// byte picks what the rest is: a message to decode (and re-encode, which must read back the same), then (bit 0) a snapshot
// a connected client applies to its scene, or (bit 1) a client update a host applies to its own. Whatever decodes must
// never end the session or crash the receiver.
#include "FuzzCommon.h"

#include <Breccia/Network/NetProtocol.h>
#include <Breccia/Network/NetworkSystem.h>
#include <Breccia/Network/ReplicationSchema.h>
#include <Breccia/Network/SimulatedNetwork.h>
#include <Breccia/Physics/PhysicsWorld.h>
#include <Breccia/Scene/Components.h>
#include <Breccia/Scene/Entity.h>
#include <Breccia/Scene/Scene.h>

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

	using namespace Breccia;

	constexpr float Step = 1.0f / 60.0f;

	std::vector<uint8_t> Reencode(const NetMessage& message)
	{
		switch (message.Type)
		{
			case NetMessageType::Hello:
				return NetProtocol::EncodeHello(std::get<NetHello>(message.Body));
			case NetMessageType::Welcome:
				return NetProtocol::EncodeWelcome(std::get<NetWelcome>(message.Body));
			case NetMessageType::Snapshot:
			case NetMessageType::ClientUpdate:
				return NetProtocol::EncodeState(message.Type, std::get<NetState>(message.Body));
		}
		__builtin_trap();
	}

	// A client scene connected to a host played by hand, kept between inputs (rebuilt now and then so entities spawned by
	// earlier inputs do not pile up).
	class Client
	{
	public:
		Client()
			: m_Network({}, 1)
		{
			std::string error;
			m_Host = m_Network.Listen(0, 1, error);
			m_Scene = std::make_shared<Scene>("FuzzClient");
			m_Scene->StartNetwork(CreateScope<NetworkSystem>(*m_Scene, NetworkRole::Client, m_Network.Connect(m_Host->GetListenPort()), NetworkSettings{}, NetworkSystem::Identity{}));
			m_Scene->OnRuntimeStart();
			std::vector<NetTransportEvent> events;
			for (int i = 0; i < 10 && events.empty(); i++)
			{
				Update();
				m_Host->Poll(events);
			}
			if (events.empty())
				__builtin_trap();
			m_Peer = events.front().Peer;
			m_Host->Send(m_Peer, NetChannel::Reliable, NetProtocol::EncodeWelcome({ 1, 30.0f }));
			Update();
			if (m_Scene->GetNetworkSystem()->GetState() != NetConnectionState::Connected)
				__builtin_trap();
		}

		~Client() { m_Scene->OnRuntimeStop(); }

		void Apply(NetState state)
		{
			// Sequence numbers only grow, or the client drops the snapshot as stale; an acknowledgement of an update the
			// client never sent is a reason to leave, which is not what this checks.
			state.Sequence = ++m_Sequence;
			state.Acknowledged = 0;
			const std::vector<uint8_t> bytes = NetProtocol::EncodeState(NetMessageType::Snapshot, state);
			if (bytes.empty())
				__builtin_trap();
			m_Host->Send(m_Peer, NetChannel::Unreliable, bytes);
			Update();
			// Whatever decodes is handled in place: the client never gives up on its host over it.
			if (m_Scene->GetNetworkSystem()->GetState() != NetConnectionState::Connected)
				__builtin_trap();
			m_Applied++;
		}

		size_t GetApplied() const { return m_Applied; }

	private:
		// Delivers what was sent, then lets the client receive it.
		void Update()
		{
			m_Network.Advance(Step);
			m_Scene->OnUpdate(Step);
		}

		SimulatedNetwork m_Network;
		Scope<NetTransport> m_Host;
		std::shared_ptr<Scene> m_Scene;
		NetPeerHandle m_Peer = 0;
		uint32_t m_Sequence = 0;
		size_t m_Applied = 0;
	};

	// A host scene with entities a client owns and others it does not, and a client played by hand that has joined as peer
	// 1 (kept between inputs, rebuilt now and then).
	class Host
	{
	public:
		Host()
			: m_Network({}, 1)
		{
			std::string error;
			auto listening = m_Network.Listen(0, 2, error);
			const uint16_t port = listening->GetListenPort();
			m_Scene = std::make_shared<Scene>("FuzzHost");
			// Entities 1-4: server-owned, owned by the client (with a light), owned by another client, a child of the first.
			for (uint64_t id = 1; id <= 4; id++)
				m_Scene->CreateEntityWithUUID(id, "Entity").AddComponent<NetworkedComponent>();
			m_Scene->GetEntityByUUID(2).GetComponent<NetworkedComponent>() = { 1, NetworkAuthority::Owner };
			m_Scene->GetEntityByUUID(2).AddComponent<PointLightComponent>();
			m_Scene->GetEntityByUUID(3).GetComponent<NetworkedComponent>() = { 2, NetworkAuthority::Owner };
			m_Scene->SetParent(m_Scene->GetEntityByUUID(4), m_Scene->GetEntityByUUID(1), false);
			m_Scene->StartNetwork(CreateScope<NetworkSystem>(*m_Scene, NetworkRole::Host, std::move(listening), NetworkSettings{}, NetworkSystem::Identity{}));
			m_Scene->OnRuntimeStart();
			m_Client = m_Network.Connect(port);
			std::vector<NetTransportEvent> events;
			for (int i = 0; i < 10 && m_Peer == 0; i++)
			{
				Update();
				m_Client->Poll(events);
				for (const NetTransportEvent& event : events)
					if (event.Type == NetTransportEvent::Kind::Connected)
						m_Peer = event.Peer;
			}
			if (m_Peer == 0)
				__builtin_trap();
			m_Client->Send(m_Peer, NetChannel::Reliable, NetProtocol::EncodeHello({ NetProtocol::Version, ReplicationSchema::Get().GetHash(), "", "" }));
			Update();
			if (m_Scene->GetNetworkSystem()->GetConnections().size() != 1)
				__builtin_trap();
		}

		~Host() { m_Scene->OnRuntimeStop(); }

		void Apply(NetState state)
		{
			// A message the host would drop as stale, or kick for (an acknowledgement of a snapshot never sent), is not
			// what this checks.
			state.Sequence = ++m_Sequence;
			state.Acknowledged = 0;
			m_Client->Send(m_Peer, NetChannel::Unreliable, NetProtocol::EncodeState(NetMessageType::ClientUpdate, state));
			Update();
			if (m_Scene->GetNetworkSystem()->GetConnections().size() != 1)
				__builtin_trap();
			m_Applied++;
		}

		size_t GetApplied() const { return m_Applied; }

	private:
		void Update()
		{
			m_Network.Advance(Step);
			m_Scene->OnUpdate(Step);
		}

		SimulatedNetwork m_Network;
		std::shared_ptr<Scene> m_Scene;
		Scope<NetTransport> m_Client;
		NetPeerHandle m_Peer = 0;
		uint32_t m_Sequence = 0;
		size_t m_Applied = 0;
	};

}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BrecciaFuzz::Init();
	static const bool configured = [] {
		// Each rebuilt client starts a physics world; a small one starts quickly.
		PhysicsCapacity capacity;
		capacity.MaxBodies = 256;
		capacity.MaxBodyPairs = 1024;
		capacity.MaxContactConstraints = 1024;
		PhysicsWorld::SetDefaultCapacity(capacity);
		return true;
	}();
	(void)configured;
	if (size == 0)
		return 0;
	const bool applyOnClient = (data[0] & 1) != 0;
	const bool applyOnHost = (data[0] & 2) != 0;
	const std::span<const uint8_t> bytes(data + 1, size - 1);

	std::string error;
	const std::optional<NetMessage> message = NetProtocol::Decode(bytes, ReplicationSchema::Get(), error);
	if (!message)
		return 0;
	// A Hello of another version decodes to its version alone, which encodes as a full Hello of that version.
	const bool partialHello = message->Type == NetMessageType::Hello && std::get<NetHello>(message->Body).ProtocolVersion != NetProtocol::Version;
	const std::vector<uint8_t> encoded = Reencode(*message);
	const std::optional<NetMessage> again = NetProtocol::Decode(encoded, ReplicationSchema::Get(), error);
	if (!again || again->Type != message->Type)
		__builtin_trap();
	if (!partialHello && Reencode(*again) != encoded)
		__builtin_trap();

	if (applyOnHost && message->Type == NetMessageType::ClientUpdate)
	{
		static Host* s_Host = nullptr;
		if (!s_Host || s_Host->GetApplied() >= 256)
		{
			delete s_Host;
			s_Host = new Host();
		}
		s_Host->Apply(std::get<NetState>(message->Body));
	}
	if (applyOnClient && message->Type == NetMessageType::Snapshot)
	{
		// Never destroyed at exit: sol2's static strings, which its Lua state's teardown reads, may already be gone.
		static Client* s_Client = nullptr;
		if (!s_Client || s_Client->GetApplied() >= 256)
		{
			delete s_Client;
			s_Client = new Client();
		}
		s_Client->Apply(std::get<NetState>(message->Body));
	}
	return 0;
}
