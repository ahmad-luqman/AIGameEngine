#pragma once

// Helpers shared by the network tests: several peers in one process over a SimulatedNetwork, hand-driven peers for
// messages a real one never sends, and builders of records and entities.

#include <doctest/doctest.h>

#include <Breccia/Network/NetBuffer.h>
#include <Breccia/Network/NetProtocol.h>
#include <Breccia/Network/NetValue.h>
#include <Breccia/Network/NetworkSettings.h>
#include <Breccia/Network/NetworkSystem.h>
#include <Breccia/Network/Replication.h>
#include <Breccia/Network/ReplicationSchema.h>
#include <Breccia/Network/SimulatedNetwork.h>
#include <Breccia/Physics/PhysicsWorld.h>
#include <Breccia/Scene/ComponentRegistry.h>
#include <Breccia/Scene/Components.h>
#include <Breccia/Scene/Entity.h>
#include <Breccia/Scene/Scene.h>
#include <Breccia/Scene/SceneSerializer.h>

#include "TestUtils.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace NetworkTestUtils {

	using namespace Breccia;

	constexpr float Step = BrecciaTest::Step;

	inline NetworkSystem::Identity TestIdentity()
	{
		return { "NetTest", "1.0.0" };
	}

	inline std::vector<uint8_t> Bytes(std::initializer_list<int> values)
	{
		std::vector<uint8_t> bytes;
		for (int value : values)
			bytes.push_back(static_cast<uint8_t>(value));
		return bytes;
	}

	inline std::vector<uint8_t> Encode(const nlohmann::json& value)
	{
		NetWriter writer;
		REQUIRE(NetValue::Encode(value, writer));
		return writer.TakeBytes();
	}

	inline std::optional<nlohmann::json> DecodeValue(const std::vector<uint8_t>& bytes)
	{
		NetReader reader(bytes);
		nlohmann::json value;
		if (!NetValue::Decode(reader, value) || !reader.AtEnd())
			return std::nullopt;
		return value;
	}

	inline uint32_t ComponentIndex(const char* name)
	{
		const std::optional<uint32_t> index = ReplicationSchema::Get().FindComponentIndex(name);
		REQUIRE(index);
		return *index;
	}

	inline std::optional<NetMessage> DecodeMessage(const std::vector<uint8_t>& bytes, std::string* error = nullptr)
	{
		std::string reason;
		auto message = NetProtocol::Decode(bytes, ReplicationSchema::Get(), reason);
		if (error)
			*error = reason;
		return message;
	}

	// A host and clients in one process, each a playing scene with its own network session, over a simulated link that
	// advances with the fixed frame step.
	class Session
	{
	public:
		explicit Session(SimulatedLinkSettings link = {}, NetworkSettings settings = {}, uint32_t seed = 1)
			: m_Network(link, seed)
			, m_Settings(settings)
		{
		}

		~Session()
		{
			for (const Ref<Scene>& client : m_Clients)
				client->OnRuntimeStop();
			if (m_Host)
				m_Host->OnRuntimeStop();
		}

		Scene& StartHost(const std::function<void(Scene&)>& build = {})
		{
			m_Host = CreateRef<Scene>("Host");
			if (build)
				build(*m_Host);
			std::string error;
			auto transport = m_Network.Listen(0, m_Settings.MaxClients + NetworkSystem::SpareTransportSlots, error);
			REQUIRE(transport);
			m_Port = transport->GetListenPort();
			m_Host->StartNetwork(CreateScope<NetworkSystem>(*m_Host, NetworkRole::Host, std::move(transport), m_Settings, m_Identity));
			m_Host->OnRuntimeStart();
			return *m_Host;
		}

		Scene& AddClient(const std::function<void(Scene&)>& build = {}, NetworkSystem::Identity identity = TestIdentity())
		{
			Ref<Scene> client = CreateRef<Scene>("Client");
			if (build)
				build(*client);
			client->StartNetwork(CreateScope<NetworkSystem>(*client, NetworkRole::Client, m_Network.Connect(m_Port), m_Settings, std::move(identity)));
			client->OnRuntimeStart();
			m_Clients.push_back(client);
			return *client;
		}

		void Run(int frames)
		{
			for (int i = 0; i < frames; i++)
			{
				if (m_Host)
					m_Host->OnUpdate(Step);
				for (const Ref<Scene>& client : m_Clients)
					client->OnUpdate(Step);
				m_Network.Advance(Step);
			}
		}

		// Runs until the condition holds (true) or `frames` pass (false).
		bool RunUntil(const std::function<bool()>& condition, int frames)
		{
			for (int i = 0; i < frames; i++)
			{
				if (condition())
					return true;
				Run(1);
			}
			return condition();
		}

		Scene& Host() { return *m_Host; }
		Scene& Client(size_t index) { return *m_Clients.at(index); }
		NetworkSystem& HostNetwork() { return *m_Host->GetNetworkSystem(); }
		NetworkSystem& ClientNetwork(size_t index) { return *m_Clients.at(index)->GetNetworkSystem(); }
		SimulatedNetwork& Link() { return m_Network; }
		uint16_t GetPort() const { return m_Port; }
		void SetIdentity(NetworkSystem::Identity identity) { m_Identity = std::move(identity); }

	private:
		SimulatedNetwork m_Network;
		NetworkSettings m_Settings;
		NetworkSystem::Identity m_Identity = TestIdentity();
		Ref<Scene> m_Host;
		std::vector<Ref<Scene>> m_Clients;
		uint16_t m_Port = 0;
	};

	// A transport driven by hand, for messages a real peer would never send.
	class RawPeer
	{
	public:
		RawPeer(Session& session, Scope<NetTransport> transport)
			: m_Session(session)
			, m_Transport(std::move(transport))
		{
		}

		// Runs the session until an event of this kind arrives here (returned), or `frames` pass.
		std::optional<NetTransportEvent> WaitFor(NetTransportEvent::Kind kind, int frames = 120)
		{
			for (int i = 0; i < frames; i++)
			{
				std::vector<NetTransportEvent> events;
				m_Transport->Poll(events);
				for (NetTransportEvent& event : events)
				{
					if (event.Type == NetTransportEvent::Kind::Connected)
						m_Peer = event.Peer;
					if (event.Type == kind)
						return std::move(event);
				}
				m_Session.Run(1);
			}
			return std::nullopt;
		}

		void Send(const std::vector<uint8_t>& data, NetChannel channel = NetChannel::Reliable) { m_Transport->Send(m_Peer, channel, data); }
		NetTransport& Transport() { return *m_Transport; }

		// Connects as a client of this build and returns the peer ID the host gave.
		uint32_t Join()
		{
			REQUIRE(WaitFor(NetTransportEvent::Kind::Connected));
			Send(NetProtocol::EncodeHello({ NetProtocol::Version, ReplicationSchema::Get().GetHash(), TestIdentity().Game, TestIdentity().GameVersion }));
			const auto welcome = WaitFor(NetTransportEvent::Kind::Received);
			REQUIRE(welcome);
			const auto message = DecodeMessage(welcome->Data);
			REQUIRE(message);
			REQUIRE(message->Type == NetMessageType::Welcome);
			return std::get<NetWelcome>(message->Body).PeerId;
		}

	private:
		Session& m_Session;
		Scope<NetTransport> m_Transport;
		NetPeerHandle m_Peer = 0;
	};

	// A client scene whose host is played by hand, for messages a real host would never send.
	class FakeHost
	{
	public:
		explicit FakeHost(const std::function<void(Scene&)>& build = {}, NetworkSettings settings = {})
		{
			std::string error;
			m_Host = m_Link.Listen(0, 1 + NetworkSystem::SpareTransportSlots, error);
			REQUIRE(m_Host);
			m_Client = CreateRef<Scene>("Client");
			if (build)
				build(*m_Client);
			m_Client->StartNetwork(CreateScope<NetworkSystem>(*m_Client, NetworkRole::Client, m_Link.Connect(m_Host->GetListenPort()), settings, TestIdentity()));
			m_Client->OnRuntimeStart();
			for (int i = 0; i < 30 && m_Peer == 0; i++)
			{
				Advance(1);
				std::vector<NetTransportEvent> events;
				m_Host->Poll(events);
				for (const NetTransportEvent& event : events)
					if (event.Type == NetTransportEvent::Kind::Connected)
						m_Peer = event.Peer;
			}
			REQUIRE(m_Peer != 0);
		}
		~FakeHost() { m_Client->OnRuntimeStop(); }

		FakeHost(const FakeHost&) = delete;
		FakeHost& operator=(const FakeHost&) = delete;

		// Delivers what was sent, then lets the client receive it.
		void Advance(int frames)
		{
			for (int i = 0; i < frames; i++)
			{
				m_Link.Advance(Step);
				m_Client->OnUpdate(Step);
			}
		}
		void Send(const std::vector<uint8_t>& message, NetChannel channel = NetChannel::Reliable)
		{
			m_Host->Send(m_Peer, channel, message);
			Advance(2);
		}
		void Welcome(float tickRate = 30.0f) { Send(NetProtocol::EncodeWelcome({ 1, tickRate })); }
		void Disconnect(uint32_t code)
		{
			m_Host->Disconnect(m_Peer, code);
			Advance(2);
		}
		void Snapshot(NetState state)
		{
			state.Sequence = ++m_Sequence;
			Send(NetProtocol::EncodeState(NetMessageType::Snapshot, state), NetChannel::Unreliable);
		}

		Scene& Client() { return *m_Client; }
		NetworkSystem& Network() { return *m_Client->GetNetworkSystem(); }

	private:
		SimulatedNetwork m_Link;
		Scope<NetTransport> m_Host;
		Ref<Scene> m_Client;
		NetPeerHandle m_Peer = 0;
		uint32_t m_Sequence = 0;
	};

	inline nlohmann::json Record(uint64_t id, nlohmann::json components, uint64_t parent = 0)
	{
		return { { "ID", id }, { "Name", "Spawned" }, { "Parent", parent }, { "Components", std::move(components) } };
	}

	inline NetEntitySpawn Spawn(uint64_t id, nlohmann::json components, uint64_t parent = 0)
	{
		return { id, parent, nlohmann::json::array({ Record(id, std::move(components)) }) };
	}

	inline NetState DecodeState(const std::vector<uint8_t>& bytes)
	{
		const auto message = DecodeMessage(bytes);
		REQUIRE(message);
		REQUIRE((message->Type == NetMessageType::Snapshot || message->Type == NetMessageType::ClientUpdate));
		return std::get<NetState>(message->Body);
	}

	// A captured entity with only a Transform, at x.
	inline ReplicatedEntity TransformEntity(UUID id, float x, float priority = 1.0f)
	{
		const auto field = [](const nlohmann::json& value) { return ReplicatedFieldValue{ Encode(value), value }; };
		ReplicatedEntity entity;
		entity.Entity = id;
		entity.Priority = priority;
		entity.Components.push_back({ ComponentIndex("Transform"), { field({ x, 0.0, 0.0 }), field({ 0.0, 0.0, 0.0 }), field({ 1.0, 1.0, 1.0 }) } });
		return entity;
	}

	// A world whose spawns are one record each, padded by `padding` bytes; counts the spawns built.
	inline ReplicationWorld SpawnWorld(std::vector<ReplicatedEntity> entities, size_t padding = 0, int* built = nullptr)
	{
		return ReplicationWorld(std::move(entities), [padding, built](const ReplicatedEntity& entity) {
			if (built)
				(*built)++;
			nlohmann::json record = { { "ID", static_cast<uint64_t>(entity.Entity) } };
			if (padding > 0)
				record["Pad"] = std::string(padding, 'x');
			NetWriter writer;
			NetProtocol::WriteSpawn(writer, { entity.Entity, 0, nlohmann::json::array({ record }) });
			return std::optional<std::vector<uint8_t>>(writer.TakeBytes());
		});
	}

	inline void AddGround(Scene& scene)
	{
		Entity ground = scene.CreateEntity("Ground");
		ground.AddComponent<RigidBodyComponent>();
		ground.AddComponent<BoxColliderComponent>().HalfExtents = { 10.0f, 0.5f, 10.0f };
	}

	inline Entity CreateNetworked(Scene& scene, const std::string& name, UUID uuid = 0, glm::vec3 translation = glm::vec3(0.0f))
	{
		Entity entity = uuid.IsValid() ? scene.CreateEntityWithUUID(uuid, name) : scene.CreateEntity(name);
		entity.GetComponent<TransformComponent>().Translation = translation;
		entity.AddComponent<NetworkedComponent>();
		return entity;
	}

	inline void Patch(Entity entity, const char* component, const nlohmann::json& data)
	{
		std::string error;
		REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(entity, component, data, error), error);
	}

}
