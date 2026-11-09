#include <doctest/doctest.h>

#include <Breccia/Automation/AutomationSession.h>
#include <Breccia/Automation/CommandRegistry.h>
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

#include "AutomationTestUtils.h"
#include "TestUtils.h"

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace Breccia;

namespace {

	constexpr float Step = BrecciaTest::Step;

	NetworkSystem::Identity TestIdentity()
	{
		return { "NetTest", "1.0.0" };
	}

	std::vector<uint8_t> Bytes(std::initializer_list<int> values)
	{
		std::vector<uint8_t> bytes;
		for (int value : values)
			bytes.push_back(static_cast<uint8_t>(value));
		return bytes;
	}

	std::vector<uint8_t> Encode(const nlohmann::json& value)
	{
		NetWriter writer;
		REQUIRE(NetValue::Encode(value, writer));
		return writer.TakeBytes();
	}

	std::optional<nlohmann::json> DecodeValue(const std::vector<uint8_t>& bytes)
	{
		NetReader reader(bytes);
		nlohmann::json value;
		if (!NetValue::Decode(reader, value) || !reader.AtEnd())
			return std::nullopt;
		return value;
	}

	uint32_t ComponentIndex(const char* name)
	{
		const std::optional<uint32_t> index = ReplicationSchema::Get().FindComponentIndex(name);
		REQUIRE(index);
		return *index;
	}

	std::optional<NetMessage> DecodeMessage(const std::vector<uint8_t>& bytes, std::string* error = nullptr)
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
			auto transport = m_Network.Listen(0, m_Settings.MaxClients + 4, error);
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
			m_Host = m_Link.Listen(0, 4, error);
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
		void Welcome() { Send(NetProtocol::EncodeWelcome({ 1, 30.0f })); }
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

	nlohmann::json Record(uint64_t id, nlohmann::json components, uint64_t parent = 0)
	{
		return { { "ID", id }, { "Name", "Spawned" }, { "Parent", parent }, { "Components", std::move(components) } };
	}

	NetEntitySpawn Spawn(uint64_t id, nlohmann::json components, uint64_t parent = 0)
	{
		return { id, parent, nlohmann::json::array({ Record(id, std::move(components)) }) };
	}

	NetState DecodeState(const std::vector<uint8_t>& bytes)
	{
		const auto message = DecodeMessage(bytes);
		REQUIRE(message);
		REQUIRE((message->Type == NetMessageType::Snapshot || message->Type == NetMessageType::ClientUpdate));
		return std::get<NetState>(message->Body);
	}

	// A captured entity with only a Transform, at x.
	ReplicatedEntity TransformEntity(UUID id, float x, float priority = 1.0f)
	{
		const auto field = [](const nlohmann::json& value) { return ReplicatedFieldValue{ Encode(value), value }; };
		ReplicatedEntity entity;
		entity.Entity = id;
		entity.Priority = priority;
		entity.Components.push_back({ ComponentIndex("Transform"), { field({ x, 0.0, 0.0 }), field({ 0.0, 0.0, 0.0 }), field({ 1.0, 1.0, 1.0 }) } });
		return entity;
	}

	// A world whose spawns are one record each, padded by `padding` bytes; counts the spawns built.
	ReplicationWorld SpawnWorld(std::vector<ReplicatedEntity> entities, size_t padding = 0, int* built = nullptr)
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

	void AddGround(Scene& scene)
	{
		Entity ground = scene.CreateEntity("Ground");
		ground.AddComponent<RigidBodyComponent>();
		ground.AddComponent<BoxColliderComponent>().HalfExtents = { 10.0f, 0.5f, 10.0f };
	}

	Entity CreateNetworked(Scene& scene, const std::string& name, UUID uuid = 0, glm::vec3 translation = glm::vec3(0.0f))
	{
		Entity entity = uuid.IsValid() ? scene.CreateEntityWithUUID(uuid, name) : scene.CreateEntity(name);
		entity.GetComponent<TransformComponent>().Translation = translation;
		entity.AddComponent<NetworkedComponent>();
		return entity;
	}

	void Patch(Entity entity, const char* component, const nlohmann::json& data)
	{
		std::string error;
		REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(entity, component, data, error), error);
	}

}

TEST_SUITE("Network")
{
	TEST_CASE("NetWriter and NetReader round-trip every value and fail on truncation")
	{
		NetWriter writer;
		writer.WriteU8(0xAB);
		writer.WriteU16(0xBEEF);
		writer.WriteU32(0xDEADBEEF);
		writer.WriteU64(0x0123456789ABCDEFull);
		writer.WriteVarUInt(0);
		writer.WriteVarUInt(127);
		writer.WriteVarUInt(128);
		writer.WriteVarUInt(std::numeric_limits<uint64_t>::max());
		writer.WriteVarInt(-1);
		writer.WriteVarInt(std::numeric_limits<int64_t>::min());
		writer.WriteVarInt(std::numeric_limits<int64_t>::max());
		writer.WriteFloat(-2.5f);
		writer.WriteDouble(1e300);
		writer.WriteString("héllo");
		const std::vector<uint8_t> bytes = writer.TakeBytes();

		NetReader reader(bytes);
		uint8_t u8 = 0;
		uint16_t u16 = 0;
		uint32_t u32 = 0;
		uint64_t u64 = 0;
		int64_t i64 = 0;
		float f = 0;
		double d = 0;
		std::string text;
		CHECK((reader.ReadU8(u8) && u8 == 0xAB));
		CHECK((reader.ReadU16(u16) && u16 == 0xBEEF));
		CHECK((reader.ReadU32(u32) && u32 == 0xDEADBEEF));
		CHECK((reader.ReadU64(u64) && u64 == 0x0123456789ABCDEFull));
		CHECK((reader.ReadVarUInt(u64) && u64 == 0));
		CHECK((reader.ReadVarUInt(u64) && u64 == 127));
		CHECK((reader.ReadVarUInt(u64) && u64 == 128));
		CHECK((reader.ReadVarUInt(u64) && u64 == std::numeric_limits<uint64_t>::max()));
		CHECK((reader.ReadVarInt(i64) && i64 == -1));
		CHECK((reader.ReadVarInt(i64) && i64 == std::numeric_limits<int64_t>::min()));
		CHECK((reader.ReadVarInt(i64) && i64 == std::numeric_limits<int64_t>::max()));
		CHECK((reader.ReadFloat(f) && f == -2.5f));
		CHECK((reader.ReadDouble(d) && d == 1e300));
		CHECK((reader.ReadString(text, 64) && text == "héllo"));
		CHECK(reader.AtEnd());
		// Past the end: this read and every later one fail.
		CHECK_FALSE(reader.ReadU8(u8));
		CHECK_FALSE(reader.Ok());

		// The maximum takes 10 bytes; a value needing an 11th, or a 10th byte above 1, overflows.
		const std::vector<uint8_t> overlongBytes = Bytes({ 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02 });
		NetReader overlong(overlongBytes);
		CHECK_FALSE(overlong.ReadVarUInt(u64));
		const std::vector<uint8_t> endlessBytes = Bytes({ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x81, 0x00 });
		NetReader endless(endlessBytes);
		CHECK_FALSE(endless.ReadVarUInt(u64));
		// The largest value with a 10th byte of 1 is fine.
		const std::vector<uint8_t> maximumBytes = Bytes({ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 });
		NetReader maximum(maximumBytes);
		CHECK((maximum.ReadVarUInt(u64) && u64 == std::numeric_limits<uint64_t>::max()));
		// Lengths and counts beyond their limits fail before anything is read.
		NetWriter longText;
		longText.WriteString("abcdef");
		NetReader limited(longText.GetBytes());
		CHECK_FALSE(limited.ReadString(text, 5));
		const std::vector<uint8_t> truncatedBytes = Bytes({ 10, 'a', 'b' });
		NetReader truncated(truncatedBytes);
		CHECK_FALSE(truncated.ReadString(text, 64));
		uint32_t count = 0;
		const std::vector<uint8_t> nine = Bytes({ 9 });
		NetReader counted(nine);
		CHECK_FALSE(counted.ReadCount(count, 8));
		const std::vector<uint8_t> eight = Bytes({ 8 });
		NetReader counted2(eight);
		CHECK((counted2.ReadCount(count, 8) && count == 8));
	}

	TEST_CASE("NetValue round-trips component JSON and refuses hostile encodings")
	{
		const nlohmann::json value = {
			{ "Null", nullptr },
			{ "Flags", { true, false } },
			{ "Unsigned", std::numeric_limits<uint64_t>::max() },
			{ "Negative", std::numeric_limits<int64_t>::min() },
			{ "Exact", 0.5 },
			{ "Precise", 0.1 },
			{ "Vector", { 1.0, -2.0, 0.25 } },
			{ "Mixed", { 1.0, 0.1 } },
			{ "Empty", nlohmann::json::array() },
			{ "Text", "ünïcode" },
			{ "Nested", { { "Inner", { { "Deep", 3 } } } } },
		};
		const std::vector<uint8_t> bytes = Encode(value);
		const auto decoded = DecodeValue(bytes);
		REQUIRE(decoded);
		CHECK(*decoded == value);
		// float64 only where float32 would lose bits.
		CHECK((*decoded)["Precise"].get<double>() == 0.1);
		// A float32 vector is one tag, a count and the raw floats.
		CHECK(Encode({ 1.0, 2.0, 3.0 }).size() == 2 + 3 * 4);
		// Every encoding decodes to a value that encodes the same way again.
		CHECK(Encode(*decoded) == bytes);

		NetWriter writer;
		CHECK_FALSE(NetValue::Encode(std::string("\xFF\xFE"), writer));
		CHECK_FALSE(NetValue::Encode(nlohmann::json::binary({ 1, 2 }), writer));

		// Unknown tag, truncated payloads, non-UTF-8 text, a duplicate key, counts beyond the bytes, an empty float array.
		CHECK_FALSE(DecodeValue(Bytes({ 200 })));
		CHECK_FALSE(DecodeValue(Bytes({ 5, 0, 0 })));
		CHECK_FALSE(DecodeValue(Bytes({ 7, 2, 0xC3, 0x28 })));
		CHECK_FALSE(DecodeValue(Bytes({ 9, 2, 1, 'a', 0, 1, 'a', 0 })));
		CHECK_FALSE(DecodeValue(Bytes({ 8, 100, 0 })));
		CHECK_FALSE(DecodeValue(Bytes({ 10, 0 })));
		CHECK_FALSE(DecodeValue(Bytes({ 4, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x80, 0x01 })));
		// Nesting: MaxDepth levels decode, one more does not (a hostile packet cannot overflow the stack).
		std::vector<uint8_t> nested;
		for (int i = 0; i < NetValue::MaxDepth; i++)
			nested.insert(nested.end(), { 8, 1 });
		nested.push_back(0);
		CHECK(DecodeValue(nested));
		nested.insert(nested.begin(), { 8, 1 });
		CHECK_FALSE(DecodeValue(nested));
		nlohmann::json deep = 1;
		for (int i = 0; i <= NetValue::MaxDepth; i++)
			deep = nlohmann::json::array({ deep });
		CHECK_FALSE(NetValue::Encode(deep, writer));
	}

	TEST_CASE("The replication schema follows the component registry")
	{
		const ReplicationSchema& schema = ReplicationSchema::Get();
		CHECK(schema.GetComponents().size() == ComponentRegistry::GetAll().size());
		const ReplicationSchema::Component* transform = schema.FindComponent(ComponentIndex("Transform"));
		const ReplicationSchema::Component* networked = schema.FindComponent(ComponentIndex("Networked"));
		REQUIRE(transform);
		REQUIRE(networked);
		CHECK(transform->Info->ReplicatedFields == std::vector<std::string>{ "Translation", "Rotation", "Scale" });
		CHECK(transform->IsCore);
		CHECK_FALSE(networked->IsCore);
		CHECK(networked->Info->ReplicatedFields == std::vector<std::string>{ "Owner", "Authority" });
		CHECK_FALSE(schema.FindComponent(static_cast<uint32_t>(schema.GetComponents().size())));
		CHECK(schema.GetHash() != 0);
		// The hash moves with anything spawns or updates depend on: a field (replicated or not), a type, an enum option, a
		// list element's field, the replicated list.
		std::vector<ComponentInfo> components = ComponentRegistry::GetAll();
		CHECK(ReplicationSchema::ComputeHash(components) == schema.GetHash());
		const auto changed = [&](const std::function<void(std::vector<ComponentInfo>&)>& change) {
			std::vector<ComponentInfo> copy = components;
			change(copy);
			return ReplicationSchema::ComputeHash(copy) != schema.GetHash();
		};
		const auto find = [](std::vector<ComponentInfo>& infos, const char* name) -> ComponentInfo& { return *std::ranges::find(infos, std::string(name), &ComponentInfo::Name); };
		CHECK(changed([&](auto& infos) { find(infos, "PointLight").Fields.push_back("Extra"); }));
		CHECK(changed([&](auto& infos) { find(infos, "PointLight").FieldTypes["Radius"].Kind = ComponentFieldKind::Integer; }));
		CHECK(changed([&](auto& infos) { find(infos, "RigidBody").EnumOptions["Type"].push_back("Ghost"); }));
		CHECK(changed([&](auto& infos) { find(infos, "PointLight").ReplicatedFields.pop_back(); }));
		CHECK(changed([&](auto& infos) {
			ComponentInfo& animator = find(infos, "Animator");
			auto element = std::make_shared<ComponentObjectSchema>(*animator.FieldTypes["Events"].Element);
			element->Fields.emplace_back("Extra");
			animator.FieldTypes["Events"].Element = element;
		}));
		// Only top-level fields, never asset fields (a client would load whatever path a host named).
		for (const ComponentInfo& info : ComponentRegistry::GetAll())
			for (const std::string& field : info.ReplicatedFields)
			{
				CHECK(std::ranges::find(info.Fields, field) != info.Fields.end());
				CHECK(std::ranges::find(info.AssetFields, field) == info.AssetFields.end());
			}
	}

	TEST_CASE("Networked component: defaults, round trip and validation")
	{
		Scene scene;
		Entity entity = scene.CreateEntity("Avatar");
		Patch(entity, "Networked", { { "Owner", 3 }, { "Authority", "Owner" }, { "Priority", 2.5 } });
		const NetworkedComponent& networked = entity.GetComponent<NetworkedComponent>();
		CHECK(networked.Owner == 3);
		CHECK(networked.Authority == NetworkAuthority::Owner);
		CHECK(networked.Priority == 2.5f);
		CHECK(ComponentRegistry::Find("Networked")->Serialize(entity) == nlohmann::json{ { "Owner", 3 }, { "Authority", "Owner" }, { "Priority", 2.5 } });
		std::string error;
		for (const nlohmann::json& bad : { nlohmann::json{ { "Priority", 0 } }, nlohmann::json{ { "Priority", 101 } }, nlohmann::json{ { "Priority", -1 } }, nlohmann::json{ { "Authority", "Client" } }, nlohmann::json{ { "Owner", -1 } } })
		{
			CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Networked", bad, error));
			CHECK(entity.GetComponent<NetworkedComponent>() == networked);
		}
	}

	TEST_CASE("NetProtocol encodes and decodes every message")
	{
		const auto hello = DecodeMessage(NetProtocol::EncodeHello({ NetProtocol::Version, 42, "Game", "1.2.3" }));
		REQUIRE(hello);
		CHECK(hello->Type == NetMessageType::Hello);
		CHECK(std::get<NetHello>(hello->Body).SchemaHash == 42);
		CHECK(std::get<NetHello>(hello->Body).Game == "Game");
		CHECK(std::get<NetHello>(hello->Body).GameVersion == "1.2.3");
		// Another version's Hello reports its version alone, whatever follows.
		std::vector<uint8_t> future = NetProtocol::EncodeHello({ NetProtocol::Version + 1, 0, "", "" });
		future.push_back(0xFF);
		const auto newer = DecodeMessage(future);
		REQUIRE(newer);
		CHECK(std::get<NetHello>(newer->Body).ProtocolVersion == NetProtocol::Version + 1);
		std::vector<uint8_t> stranger = NetProtocol::EncodeHello({ NetProtocol::Version, 0, "", "" });
		stranger[1] ^= 0xFF;
		std::string error;
		CHECK_FALSE(DecodeMessage(stranger, &error));
		CHECK(error == "not a Breccia client");

		const auto welcome = DecodeMessage(NetProtocol::EncodeWelcome({ 7, 20.0f }));
		REQUIRE(welcome);
		CHECK(std::get<NetWelcome>(welcome->Body).PeerId == 7);
		CHECK_FALSE(DecodeMessage(NetProtocol::EncodeWelcome({ 0, 20.0f })));
		CHECK_FALSE(DecodeMessage(NetProtocol::EncodeWelcome({ 1, std::numeric_limits<float>::quiet_NaN() })));

		const uint32_t transform = ComponentIndex("Transform");
		const uint32_t light = ComponentIndex("PointLight");
		NetState state;
		state.Sequence = 5;
		state.Acknowledged = 3;
		state.Spawns.push_back({ 10, 0, nlohmann::json::array({ { { "ID", 10 }, { "Name", "A" }, { "Parent", 0 }, { "Components", nlohmann::json::object() } } }) });
		state.Updates.push_back({ 11, UUID(10), { { transform, NetComponentChange::Changed, { { 0, { 1.0, 2.0, 3.0 } } } }, { light, NetComponentChange::Removed, {} } } });
		state.Updates.push_back({ 12, std::nullopt, { { light, NetComponentChange::Added, { { 0, { 1.0, 1.0, 1.0 } }, { 1, 4.0 } } } } });
		state.Despawns.emplace_back(13);
		const std::vector<uint8_t> bytes = NetProtocol::EncodeState(NetMessageType::Snapshot, state);
		const auto decoded = DecodeMessage(bytes, &error);
		REQUIRE_MESSAGE(decoded, error);
		const NetState& back = std::get<NetState>(decoded->Body);
		CHECK(back.Sequence == 5);
		CHECK(back.Acknowledged == 3);
		REQUIRE(back.Spawns.size() == 1);
		CHECK(back.Spawns[0].Entities == state.Spawns[0].Entities);
		REQUIRE(back.Updates.size() == 2);
		CHECK(back.Updates[0].Parent == std::optional<UUID>(10));
		CHECK(back.Updates[0].Components[0].Fields[0].Value == nlohmann::json{ 1.0, 2.0, 3.0 });
		CHECK(back.Updates[0].Components[1].Change == NetComponentChange::Removed);
		CHECK(back.Updates[1].Components[0].Change == NetComponentChange::Added);
		CHECK(back.Despawns == std::vector<UUID>{ 13 });
		CHECK(NetProtocol::EncodeState(NetMessageType::Snapshot, back) == bytes);
		// Trailing bytes, and clients sending spawns or despawns (each alone), are invalid.
		std::vector<uint8_t> trailing = bytes;
		trailing.push_back(0);
		CHECK_FALSE(DecodeMessage(trailing));
		NetState clientSpawns;
		clientSpawns.Sequence = 1;
		clientSpawns.Spawns = state.Spawns;
		CHECK_FALSE(DecodeMessage(NetProtocol::EncodeState(NetMessageType::ClientUpdate, clientSpawns), &error));
		CHECK(error == "clients cannot spawn entities");
		NetState clientDespawns;
		clientDespawns.Sequence = 1;
		clientDespawns.Despawns = { 13 };
		CHECK_FALSE(DecodeMessage(NetProtocol::EncodeState(NetMessageType::ClientUpdate, clientDespawns), &error));
		CHECK(error == "clients cannot despawn entities");
	}

	TEST_CASE("NetProtocol rejects records the schema does not allow")
	{
		const uint32_t transform = ComponentIndex("Transform");
		const uint32_t light = ComponentIndex("PointLight");
		const uint32_t listener = ComponentIndex("AudioListener");
		const auto withUpdate = [](NetEntityUpdate update) {
			NetState state;
			state.Sequence = 1;
			state.Updates.push_back(std::move(update));
			return state;
		};
		// The decoder refuses what CheckUpdate refuses, for the reason it gives.
		const auto refused = [&](NetEntityUpdate update, const std::string& reason) {
			CHECK(NetProtocol::CheckUpdate(update, ReplicationSchema::Get()) == reason);
			std::string error;
			CHECK_FALSE(DecodeMessage(NetProtocol::EncodeState(NetMessageType::Snapshot, withUpdate(std::move(update))), &error));
			CHECK(error == reason);
		};
		const NetEntityUpdate valid{ 1, std::nullopt, { { transform, NetComponentChange::Changed, { { 2, { 1.0, 1.0, 1.0 } } } } } };
		CHECK(NetProtocol::CheckUpdate(valid, ReplicationSchema::Get()).empty());
		CHECK(DecodeMessage(NetProtocol::EncodeState(NetMessageType::Snapshot, withUpdate(valid))));
		refused({ 1, std::nullopt, { { 100000, NetComponentChange::Changed, { { 0, 1 } } } } }, "unknown component index 100000");
		refused({ 1, std::nullopt, { { transform, NetComponentChange::Changed, { { 3, 1 } } } } }, "Transform has no replicated field 3");
		refused({ 1, std::nullopt, { { transform, NetComponentChange::Changed, { { 0, 1 }, { 0, 1 } } } } }, "Transform field 0 appears twice");
		refused({ 1, std::nullopt, { { transform, NetComponentChange::Removed, {} } } }, "Transform cannot be added or removed");
		refused({ 1, std::nullopt, { { light, NetComponentChange::Added, { { 0, 1 } } } } }, "an added PointLight must carry every replicated field");
		refused({ 1, std::nullopt, { { light, NetComponentChange::Removed, { { 0, 1 } } } } }, "a removed PointLight carries fields");
		refused({ 1, std::nullopt, { { light, NetComponentChange::Changed, {} } } }, "a changed PointLight carries no field");
		refused({ 1, std::nullopt, { { light, NetComponentChange::Changed, { { 1, 2.0 } } }, { light, NetComponentChange::Changed, { { 1, 2.0 } } } } }, "PointLight appears twice in one entity update");
		// Nothing is tracked for a component without replicated fields, so nothing may add or remove it after a spawn.
		refused({ 1, std::nullopt, { { listener, NetComponentChange::Added, {} } } }, "AudioListener has no replicated fields");
		refused({ 1, std::nullopt, { { listener, NetComponentChange::Removed, {} } } }, "AudioListener has no replicated fields");
		refused({ 1, std::nullopt, {} }, "an entity update carries nothing");
		refused({ 1, UUID(1), {} }, "an entity cannot be its own parent");
		refused({ 0, UUID(5), {} }, "an entity ID is 0");
		// An invalid change byte.
		// Type, sequence, acknowledgement, spawn and update counts (11 bytes); the entity, flags, component count and index
		// (11 more): then the change.
		std::vector<uint8_t> badChange = NetProtocol::EncodeState(NetMessageType::Snapshot, withUpdate(valid));
		REQUIRE(badChange.size() > 22);
		REQUIRE(badChange[21] == transform);
		REQUIRE(badChange[22] == static_cast<uint8_t>(NetComponentChange::Changed));
		badChange[22] = 3;
		CHECK_FALSE(DecodeMessage(badChange));
		// One entity twice in a message, sequence 0, and spawns with no records or too many.
		std::string error;
		NetState twice = withUpdate({ 1, UUID(5), {} });
		twice.Despawns.emplace_back(1);
		CHECK_FALSE(DecodeMessage(NetProtocol::EncodeState(NetMessageType::Snapshot, twice), &error));
		CHECK(error == "an entity appears twice in one message");
		NetState zero = withUpdate({ 1, UUID(5), {} });
		zero.Sequence = 0;
		CHECK_FALSE(DecodeMessage(NetProtocol::EncodeState(NetMessageType::Snapshot, zero)));
		NetState emptySpawn;
		emptySpawn.Sequence = 1;
		emptySpawn.Spawns.push_back({ 2, 0, nlohmann::json::array() });
		CHECK_FALSE(DecodeMessage(NetProtocol::EncodeState(NetMessageType::Snapshot, emptySpawn)));
		const NetEntitySpawn huge{ 2, 0, nlohmann::json(std::vector<nlohmann::json>(NetProtocol::MaxSpawnEntities + 1, nlohmann::json::object())) };
		CHECK_FALSE(NetProtocol::CheckSpawn(huge).empty());
		CHECK(NetProtocol::CheckSpawn({ 2, 0, nlohmann::json::array({ nlohmann::json::object() }) }).empty());
		CHECK_FALSE(NetProtocol::CheckSpawn({ 2, 2, nlohmann::json::array({ nlohmann::json::object() }) }).empty());

		CHECK(DisconnectReasonFromCode(0) == NetDisconnectReason::None);
		CHECK(DisconnectReasonFromCode(static_cast<uint32_t>(NetDisconnectReason::ServerFull)) == NetDisconnectReason::ServerFull);
		CHECK(DisconnectReasonFromCode(static_cast<uint32_t>(NetDisconnectReason::Unknown)) == NetDisconnectReason::Unknown);
		CHECK(DisconnectReasonFromCode(9999) == NetDisconnectReason::Unknown);
		CHECK(std::string(DescribeDisconnectReason(NetDisconnectReason::Unknown)) == "an unknown reason (a newer peer's)");
	}

	TEST_CASE("ReplicationSender repeats what is unacknowledged and sends only changes after")
	{
		int spawnsBuilt = 0;
		const auto world = [&](std::vector<ReplicatedEntity> entities) { return SpawnWorld(std::move(entities), 0, &spawnsBuilt); };

		ReplicationSender sender;
		// Unacknowledged spawns repeat (built once per world).
		NetState first = DecodeState(sender.Build(world({ TransformEntity(100, 1) }), 0, 100000));
		CHECK(first.Sequence == 1);
		CHECK(first.Spawns.size() == 1);
		CHECK(DecodeState(sender.Build(world({ TransformEntity(100, 1) }), 0, 100000)).Spawns.size() == 1);
		CHECK(spawnsBuilt == 2);
		// Acknowledging the second makes it known; nothing changed since, so nothing goes.
		CHECK(sender.Acknowledge(2));
		CHECK(sender.GetKnownEntityCount() == 1);
		NetState quiet = DecodeState(sender.Build(world({ TransformEntity(100, 1) }), 0, 100000));
		CHECK((quiet.Spawns.empty() && quiet.Updates.empty()));
		// A change goes as just that field, and repeats until acknowledged.
		NetState moved = DecodeState(sender.Build(world({ TransformEntity(100, 2) }), 0, 100000));
		REQUIRE(moved.Updates.size() == 1);
		REQUIRE(moved.Updates[0].Components.size() == 1);
		CHECK(moved.Updates[0].Components[0].Fields.size() == 1);
		CHECK(moved.Updates[0].Components[0].Fields[0].Field == 0);
		// Moving back to the acknowledged value still sends it: the receiver may have applied the unacknowledged one.
		NetState back = DecodeState(sender.Build(world({ TransformEntity(100, 1) }), 0, 100000));
		REQUIRE(back.Updates.size() == 1);
		CHECK(back.Updates[0].Components[0].Fields[0].Value == nlohmann::json{ 1.0, 0.0, 0.0 });
		// The receiver applied the newest (acknowledging it drops the older one, whatever it held).
		CHECK(sender.Acknowledge(back.Sequence));
		CHECK(DecodeState(sender.Build(world({ TransformEntity(100, 1) }), 0, 100000)).Updates.empty());
		// An acknowledgement of a message older than one already acknowledged changes nothing.
		CHECK(sender.Acknowledge(moved.Sequence));
		CHECK(DecodeState(sender.Build(world({ TransformEntity(100, 1) }), 0, 100000)).Updates.empty());
		// One this sender never built is refused.
		CHECK_FALSE(sender.Acknowledge(1000));

		// Gone: despawns repeat until acknowledged, even when the entity is back meanwhile, and its new spawn waits for that.
		const NetState gone = DecodeState(sender.Build(world({}), 0, 100000));
		CHECK(gone.Despawns == std::vector<UUID>{ 100 });
		NetState again = DecodeState(sender.Build(world({ TransformEntity(100, 5) }), 0, 100000));
		CHECK(again.Despawns == std::vector<UUID>{ 100 });
		CHECK(again.Spawns.empty());
		CHECK(again.Updates.empty());
		CHECK(sender.Acknowledge(again.Sequence));
		CHECK(sender.GetKnownEntityCount() == 0);
		CHECK(DecodeState(sender.Build(world({ TransformEntity(100, 5) }), 0, 100000)).Spawns.size() == 1);
	}

	TEST_CASE("ReplicationSender settles once a value is acknowledged, however long acknowledgements take")
	{
		ReplicationSender sender;
		sender.Build(SpawnWorld({ TransformEntity(100, 0) }), 0, 100000);
		REQUIRE(sender.Acknowledge(1));
		// A round trip longer than a tick: the change goes out three times before the first acknowledgement arrives.
		for (int tick = 0; tick < 3; tick++)
			CHECK(DecodeState(sender.Build(SpawnWorld({ TransformEntity(100, 7) }), 0, 100000)).Updates.size() == 1);
		REQUIRE(sender.Acknowledge(2));
		// Every unacknowledged message carried 7, which the receiver now holds: nothing more to send.
		CHECK(DecodeState(sender.Build(SpawnWorld({ TransformEntity(100, 7) }), 0, 100000)).Updates.empty());
		// A value that changed back while messages were in flight keeps going until a message carrying it is acknowledged.
		DecodeState(sender.Build(SpawnWorld({ TransformEntity(100, 8) }), 0, 100000));
		const NetState reverted = DecodeState(sender.Build(SpawnWorld({ TransformEntity(100, 7) }), 0, 100000));
		REQUIRE(reverted.Updates.size() == 1);
		REQUIRE(sender.Acknowledge(reverted.Sequence - 1));
		CHECK(DecodeState(sender.Build(SpawnWorld({ TransformEntity(100, 7) }), 0, 100000)).Updates.size() == 1);

		// The receiver may have applied a message carrying another value whose acknowledgement was lost, and lost the one
		// that put the value back: until a message carrying the value is acknowledged, it keeps going.
		ReplicationSender other;
		other.Build(SpawnWorld({ TransformEntity(100, 7) }), 0, 100000);
		REQUIRE(other.Acknowledge(1));
		DecodeState(other.Build(SpawnWorld({ TransformEntity(100, 8) }), 0, 100000));
		DecodeState(other.Build(SpawnWorld({ TransformEntity(100, 7) }), 0, 100000));
		CHECK(DecodeState(other.Build(SpawnWorld({ TransformEntity(100, 7) }), 0, 100000)).Updates.size() == 1);
		// A message carrying 7 is acknowledged: settled.
		REQUIRE(other.Acknowledge(4));
		CHECK(DecodeState(other.Build(SpawnWorld({ TransformEntity(100, 7) }), 0, 100000)).Updates.empty());
	}

	TEST_CASE("ReplicationSender limits: oversized records, a full history, unsendable components, scopes")
	{
		const uint32_t light = ComponentIndex("PointLight");
		// A record no message can carry is never sent, and smaller ones still are.
		ReplicationSender small(ReplicationSenderOptions{ .MaxMessageBytes = 200 });
		const NetState first = DecodeState(small.Build(SpawnWorld({ TransformEntity(1, 0), TransformEntity(2, 0) }, 300), 0, 100000));
		CHECK(first.Spawns.empty());
		CHECK(DecodeState(small.Build(SpawnWorld({ TransformEntity(3, 0) }), 0, 100000)).Spawns.size() == 1);

		// Forgotten messages: an acknowledgement of one changes nothing, and what it carried goes again.
		ReplicationSender forgetful(ReplicationSenderOptions{ .HistoryLength = 2 });
		for (int i = 0; i < 3; i++)
			forgetful.Build(SpawnWorld({ TransformEntity(1, 0) }), 0, 100000);
		CHECK(forgetful.Acknowledge(1));
		CHECK(forgetful.GetKnownEntityCount() == 0);
		CHECK(DecodeState(forgetful.Build(SpawnWorld({ TransformEntity(1, 0) }), 0, 100000)).Spawns.size() == 1);
		// An acknowledgement of a message that carried nothing (it is not in the history) makes nothing known.
		ReplicationSender empty;
		empty.Build(SpawnWorld({}), 0, 100000);
		empty.Build(SpawnWorld({ TransformEntity(1, 0) }), 0, 100000);
		CHECK(empty.Acknowledge(1));
		CHECK(empty.GetKnownEntityCount() == 0);

		// A component that cannot be encoded this tick is neither updated nor removed.
		ReplicationSender sender;
		ReplicatedEntity lit = TransformEntity(1, 0);
		lit.Components.push_back({ light, { { Encode({ 1.0, 1.0, 1.0 }), { 1.0, 1.0, 1.0 } }, { Encode(2.0), 2.0 } } });
		sender.Build(SpawnWorld({ lit }), 0, 100000);
		REQUIRE(sender.Acknowledge(1));
		ReplicatedEntity broken = lit;
		broken.Components[1] = { light, {}, true };
		CHECK(DecodeState(sender.Build(SpawnWorld({ broken }), 0, 100000)).Updates.empty());
		ReplicatedEntity removed = TransformEntity(1, 0);
		const NetState removal = DecodeState(sender.Build(SpawnWorld({ removed }), 0, 100000));
		REQUIRE(removal.Updates.size() == 1);
		CHECK(removal.Updates[0].Components[0].Change == NetComponentChange::Removed);

		// PresenceOnly: a receiver that decides an entity hears of a component the host adds, not of its field changes.
		const ReplicationFilter owned = [](const ReplicatedEntity&, uint32_t) { return ReplicationScope::PresenceOnly; };
		ReplicationSender owner;
		owner.Build(SpawnWorld({ TransformEntity(1, 0) }), 0, 100000, owned);
		REQUIRE(owner.Acknowledge(1));
		CHECK(DecodeState(owner.Build(SpawnWorld({ TransformEntity(1, 9) }), 0, 100000, owned)).Updates.empty());
		const NetState added = DecodeState(owner.Build(SpawnWorld({ lit }), 0, 100000, owned));
		REQUIRE(added.Updates.size() == 1);
		REQUIRE(added.Updates[0].Components.size() == 1);
		CHECK(added.Updates[0].Components[0].Component == light);
		CHECK(added.Updates[0].Components[0].Change == NetComponentChange::Added);

		// Client updates carry field changes only: no spawns, despawns, parents or additions.
		ReplicationSender client(ReplicationSenderOptions{ .Direction = ReplicationDirection::ClientToHost, .MaxMessageBytes = NetworkSystem::MaxClientMessageBytes });
		ReplicatedEntity child = lit;
		child.Parent = 50;
		const auto clientMessage = DecodeMessage(client.Build(SpawnWorld({ child }), 0, 100000));
		REQUIRE(clientMessage);
		CHECK(clientMessage->Type == NetMessageType::ClientUpdate);
		const NetState& clientState = std::get<NetState>(clientMessage->Body);
		REQUIRE(clientState.Updates.size() == 1);
		CHECK_FALSE(clientState.Updates[0].Parent);
		for (const NetComponentUpdate& change : clientState.Updates[0].Components)
			CHECK(change.Change == NetComponentChange::Changed);
		CHECK(DecodeState(client.Build(SpawnWorld({}), 0, 100000)).Despawns.empty());
	}

	TEST_CASE("ReplicationSender stays within its budget and lets waiting entities catch up")
	{
		const auto world = [](std::vector<ReplicatedEntity> entities) { return SpawnWorld(std::move(entities), 200); };
		ReplicationSender sender;
		// A budget for one spawn: the rest wait.
		std::vector<uint8_t> message = sender.Build(world({ TransformEntity(1, 0), TransformEntity(2, 0), TransformEntity(3, 0) }), 0, 300);
		CHECK(message.size() <= 300);
		CHECK(DecodeState(message).Spawns.size() == 1);
		CHECK(sender.GetDeferredRecordCount() == 2);
		// Below the size of any record, the first still goes when the budget is positive; at 0 nothing does.
		CHECK(DecodeState(sender.Build(world({ TransformEntity(1, 0), TransformEntity(2, 0), TransformEntity(3, 0) }), 0, 10)).Spawns.size() == 1);
		CHECK(DecodeState(sender.Build(world({ TransformEntity(1, 0), TransformEntity(2, 0), TransformEntity(3, 0) }), 0, 0)).Spawns.empty());

		// Priorities: with room for one update per tick, the higher priority goes more often, and the lower one is never
		// starved (its total catches up within a few ticks).
		ReplicationSender updates;
		updates.Build(world({ TransformEntity(1, 0, 1), TransformEntity(2, 0, 3) }), 0, 100000);
		REQUIRE(updates.Acknowledge(1));
		int sent[3] = {};
		int lastLow = 0;
		int longestWait = 0;
		for (int tick = 1; tick <= 40; tick++)
		{
			const auto x = static_cast<float>(tick);
			const NetState state = DecodeState(updates.Build(world({ TransformEntity(1, x, 1), TransformEntity(2, x, 3) }), 0, 40));
			REQUIRE(state.Updates.size() == 1);
			const auto entity = static_cast<size_t>(static_cast<uint64_t>(state.Updates[0].Entity));
			sent[entity]++;
			if (entity == 1)
			{
				longestWait = std::max(longestWait, tick - lastLow);
				lastLow = tick;
			}
			REQUIRE(updates.Acknowledge(state.Sequence));
		}
		CHECK(sent[2] > sent[1]);
		CHECK(sent[1] + sent[2] == 40);
		CHECK(longestWait <= 3);
	}

	TEST_CASE("SimulatedNetwork delivers with latency, loses unreliable messages and keeps reliable ones in order")
	{
		SimulatedNetwork network({ .Latency = 0.1f, .Loss = 0.5f, .TimeoutSeconds = 60.0f }, 7);
		std::string error;
		Scope<NetTransport> host = network.Listen(0, 4, error);
		REQUIRE(host);
		CHECK_FALSE(network.Listen(host->GetListenPort(), 4, error));
		Scope<NetTransport> client = network.Connect(host->GetListenPort());
		std::vector<NetTransportEvent> hostEvents;
		std::vector<NetTransportEvent> clientEvents;
		const auto advance = [&](float seconds) {
			for (int step = 0; step < static_cast<int>(std::lround(seconds / 0.01f)); step++)
			{
				network.Advance(0.01f);
				host->Poll(hostEvents);
				client->Poll(clientEvents);
			}
		};
		// Half of the handshake's messages are lost too, each costing a round trip.
		for (int i = 0; i < 100 && clientEvents.empty(); i++)
			advance(0.1f);
		REQUIRE_FALSE(hostEvents.empty());
		CHECK(hostEvents[0].Type == NetTransportEvent::Kind::Connected);
		REQUIRE_FALSE(clientEvents.empty());
		CHECK(clientEvents[0].Type == NetTransportEvent::Kind::Connected);
		const NetPeerHandle peer = hostEvents[0].Peer;
		hostEvents.clear();
		for (int i = 0; i < 100; i++)
		{
			client->Send(clientEvents[0].Peer, NetChannel::Reliable, Bytes({ i }));
			client->Send(clientEvents[0].Peer, NetChannel::Unreliable, Bytes({ i }));
			advance(0.02f);
		}
		advance(4.0f);
		std::vector<int> reliable;
		int unreliable = 0;
		for (const NetTransportEvent& event : hostEvents)
		{
			if (event.Type != NetTransportEvent::Kind::Received)
				continue;
			REQUIRE(event.Peer == peer);
			if (event.Channel == NetChannel::Reliable)
				reliable.push_back(event.Data[0]);
			else
				unreliable++;
		}
		REQUIRE(reliable.size() == 100);
		for (int i = 0; i < 100; i++)
			CHECK(reliable[i] == i);
		CHECK(unreliable > 20);
		CHECK(unreliable < 80);
		CHECK(network.GetLostCount() > 0);
		CHECK(host->GetRoundTripTime(peer) == doctest::Approx(0.2f));

		// A disconnect arrives with its code; the side that called it hears nothing.
		hostEvents.clear();
		clientEvents.clear();
		network.SetLinkSettings({ .Latency = 0.1f, .TimeoutSeconds = 60.0f });
		host->Disconnect(peer, 1234);
		advance(1.0f);
		CHECK(hostEvents.empty());
		REQUIRE(clientEvents.size() == 1);
		CHECK(clientEvents[0].Type == NetTransportEvent::Kind::Disconnected);
		CHECK(clientEvents[0].DisconnectCode == 1234);
		CHECK_FALSE(clientEvents[0].TimedOut);

		// Nobody listening, or a full host: the attempt times out.
		network.SetLinkSettings({ .TimeoutSeconds = 2.0f });
		Scope<NetTransport> lost = network.Connect(9999);
		Scope<NetTransport> full = network.Listen(0, 0, error);
		Scope<NetTransport> refused = network.Connect(full->GetListenPort());
		std::vector<NetTransportEvent> lostEvents;
		std::vector<NetTransportEvent> refusedEvents;
		for (int i = 0; i < 300; i++)
		{
			network.Advance(0.01f);
			lost->Poll(lostEvents);
			refused->Poll(refusedEvents);
		}
		REQUIRE(lostEvents.size() == 1);
		CHECK(lostEvents[0].TimedOut);
		REQUIRE(refusedEvents.size() == 1);
		CHECK(refusedEvents[0].TimedOut);
	}

	TEST_CASE("A host spawns, updates and despawns networked entities on its clients")
	{
		Session session;
		UUID crateId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			crateId = CreateNetworked(scene, "Crate", 0, { 1.0f, 2.0f, 3.0f }).GetUUID();
			scene.CreateEntity("Ground");
		});
		Scene& client = session.AddClient();
		REQUIRE(session.RunUntil([&] { return static_cast<bool>(client.GetEntityByUUID(crateId)); }, 60));
		CHECK(session.ClientNetwork(0).GetState() == NetConnectionState::Connected);
		CHECK(session.ClientNetwork(0).GetLocalPeerId() == 1);
		Entity copy = client.GetEntityByUUID(crateId);
		CHECK(copy.GetName() == "Crate");
		CHECK(copy.GetComponent<TransformComponent>().Translation == glm::vec3(1.0f, 2.0f, 3.0f));
		CHECK_FALSE(client.FindEntityByName("Ground"));
		CHECK(session.ClientNetwork(0).GetRemoteEntityCount() == 1);
		CHECK(session.HostNetwork().GetConnections().size() == 1);

		// Replicated fields follow.
		Entity crate = host.GetEntityByUUID(crateId);
		Patch(crate, "Transform", { { "Translation", { 5.0, 0.0, -1.0 } }, { "Rotation", { 0.0, 90.0, 0.0 } } });
		session.Run(10);
		CHECK(copy.GetComponent<TransformComponent>().Translation == glm::vec3(5.0f, 0.0f, -1.0f));
		// The same orientation (a quaternion and its negation are one rotation).
		CHECK(std::abs(glm::dot(copy.GetComponent<TransformComponent>().Rotation, crate.GetComponent<TransformComponent>().Rotation)) == doctest::Approx(1.0f));

		// A component with replicated fields appears, changes and goes; one without them never changes after the spawn.
		Patch(crate, "PointLight", { { "Intensity", 4.0 } });
		Patch(crate, "AudioListener", nlohmann::json::object());
		session.Run(10);
		REQUIRE(copy.HasComponent<PointLightComponent>());
		CHECK(copy.GetComponent<PointLightComponent>().Intensity == 4.0f);
		CHECK_FALSE(copy.HasComponent<AudioListenerComponent>());
		Patch(crate, "PointLight", { { "Intensity", 9.0 }, { "Radius", 3.0 } });
		session.Run(10);
		CHECK(copy.GetComponent<PointLightComponent>().Intensity == 9.0f);
		// Radius is not replicated: it stays as the component was added.
		CHECK(copy.GetComponent<PointLightComponent>().Radius == PointLightComponent{}.Radius);
		crate.RemoveComponent<PointLightComponent>();
		session.Run(10);
		CHECK_FALSE(copy.HasComponent<PointLightComponent>());

		// Destroyed on the host, gone on the client.
		host.DestroyEntity(crate);
		session.Run(10);
		CHECK_FALSE(client.GetEntityByUUID(crateId));
		CHECK(session.ClientNetwork(0).GetStatistics().SpawnsApplied == 1);
		CHECK(session.ClientNetwork(0).GetStatistics().DespawnsApplied == 1);
		CHECK(session.ClientNetwork(0).GetRemoteEntityCount() == 0);

		// Removing Networked on the host takes the entity off the clients (the host keeps it).
		Entity marker = CreateNetworked(host, "Marker");
		REQUIRE(session.RunUntil([&] { return static_cast<bool>(client.GetEntityByUUID(marker.GetUUID())); }, 30));
		marker.RemoveComponent<NetworkedComponent>();
		REQUIRE(session.RunUntil([&] { return !client.GetEntityByUUID(marker.GetUUID()); }, 30));
		CHECK(host.GetEntityByUUID(marker.GetUUID()));
	}

	TEST_CASE("A client replaces its own networked entities with the host's and keeps the rest")
	{
		const auto build = [](Scene& scene) {
			CreateNetworked(scene, "Door", 500, { 0.0f, 0.0f, 0.0f });
			scene.CreateEntityWithUUID(600, "Level");
		};
		Session session;
		Scene& host = session.StartHost(build);
		Patch(host.GetEntityByUUID(500), "Transform", { { "Translation", { 0.0, 3.0, 0.0 } } });
		Scene& client = session.AddClient(build);
		// Before the host accepts it, the client's copy is its own.
		CHECK(client.GetEntityByUUID(500).GetComponent<TransformComponent>().Translation.y == 0.0f);
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(0).GetRemoteEntityCount() == 1; }, 60));
		CHECK(client.GetEntityByUUID(500).GetComponent<TransformComponent>().Translation.y == 3.0f);
		CHECK(client.GetEntityByUUID(600));
		CHECK(client.FindEntitiesByName("Door").size() == 1);
	}

	TEST_CASE("Spawns carry an entity's tree, and networked descendants replicate on their own")
	{
		Session session;
		UUID vehicleId = 0;
		UUID wheelId = 0;
		UUID turretId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			Entity vehicle = CreateNetworked(scene, "Vehicle");
			Entity wheel = scene.CreateChildEntity(vehicle, "Wheel");
			Entity turret = scene.CreateChildEntity(vehicle, "Turret");
			turret.AddComponent<NetworkedComponent>().Priority = 0.5f;
			vehicleId = vehicle.GetUUID();
			wheelId = wheel.GetUUID();
			turretId = turret.GetUUID();
		});
		Scene& client = session.AddClient();
		REQUIRE(session.RunUntil([&] { return client.GetEntityByUUID(vehicleId) && client.GetEntityByUUID(turretId); }, 60));
		CHECK(client.GetEntityByUUID(wheelId).GetParent() == client.GetEntityByUUID(vehicleId));
		CHECK(client.GetEntityByUUID(turretId).GetParent() == client.GetEntityByUUID(vehicleId));
		// One copy of the turret: its own spawn, not inside the vehicle's.
		CHECK(client.FindEntitiesByName("Turret").size() == 1);
		CHECK(session.ClientNetwork(0).GetRemoteEntityCount() == 2);

		// Reparenting replicates.
		host.SetParent(host.GetEntityByUUID(turretId), Entity{}, false);
		session.Run(10);
		CHECK_FALSE(client.GetEntityByUUID(turretId).GetParent());
		host.SetParent(host.GetEntityByUUID(turretId), host.GetEntityByUUID(vehicleId), false);
		session.Run(10);
		CHECK(client.GetEntityByUUID(turretId).GetParent() == client.GetEntityByUUID(vehicleId));
	}

	TEST_CASE("Owner authority: a client drives the entity it owns, and the host passes it on")
	{
		Session session;
		UUID avatarId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			Entity avatar = CreateNetworked(scene, "Avatar");
			avatar.GetComponent<NetworkedComponent>() = { 1, NetworkAuthority::Owner };
			avatarId = avatar.GetUUID();
		});
		Scene& owner = session.AddClient();
		Scene& other = session.AddClient();
		REQUIRE(session.RunUntil([&] { return owner.GetEntityByUUID(avatarId) && other.GetEntityByUUID(avatarId); }, 60));
		CHECK(session.ClientNetwork(0).GetLocalPeerId() == 1);
		CHECK(session.ClientNetwork(1).GetLocalPeerId() == 2);
		CHECK(session.ClientNetwork(0).HasAuthority(owner.GetEntityByUUID(avatarId)));
		CHECK_FALSE(session.ClientNetwork(1).HasAuthority(other.GetEntityByUUID(avatarId)));
		CHECK_FALSE(session.HostNetwork().HasAuthority(host.GetEntityByUUID(avatarId)));
		// Entities without a Networked component are everyone's own.
		CHECK(session.HostNetwork().HasAuthority(host.CreateEntity("Local")));

		Patch(owner.GetEntityByUUID(avatarId), "Transform", { { "Translation", { 7.0, 0.0, 0.0 } } });
		session.Run(20);
		CHECK(host.GetEntityByUUID(avatarId).GetComponent<TransformComponent>().Translation.x == 7.0f);
		CHECK(other.GetEntityByUUID(avatarId).GetComponent<TransformComponent>().Translation.x == 7.0f);
		// The host's own changes do not reach the owner, which decides.
		Patch(host.GetEntityByUUID(avatarId), "Transform", { { "Translation", { -1.0, 0.0, 0.0 } } });
		session.Run(4);
		CHECK(owner.GetEntityByUUID(avatarId).GetComponent<TransformComponent>().Translation.x == 7.0f);
		// The other client's edits stay local: it has no say.
		Patch(other.GetEntityByUUID(avatarId), "Transform", { { "Translation", { 99.0, 0.0, 0.0 } } });
		session.Run(20);
		CHECK(host.GetEntityByUUID(avatarId).GetComponent<TransformComponent>().Translation.x != 99.0f);

		// Handing the entity to the other client moves the say with it.
		Patch(host.GetEntityByUUID(avatarId), "Networked", { { "Owner", 2 } });
		session.Run(20);
		CHECK_FALSE(session.ClientNetwork(0).HasAuthority(owner.GetEntityByUUID(avatarId)));
		CHECK(session.ClientNetwork(1).HasAuthority(other.GetEntityByUUID(avatarId)));
		Patch(other.GetEntityByUUID(avatarId), "Transform", { { "Translation", { 3.0, 0.0, 0.0 } } });
		session.Run(20);
		CHECK(host.GetEntityByUUID(avatarId).GetComponent<TransformComponent>().Translation.x == 3.0f);
		CHECK(owner.GetEntityByUUID(avatarId).GetComponent<TransformComponent>().Translation.x == 3.0f);
	}

	TEST_CASE("The host refuses changes to entities a client does not own, and structure changes from any client")
	{
		Session session;
		UUID crateId = 0;
		UUID ownedId = 0;
		UUID otherId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			crateId = CreateNetworked(scene, "Crate").GetUUID();
			Entity owned = CreateNetworked(scene, "Owned");
			owned.GetComponent<NetworkedComponent>() = { 1, NetworkAuthority::Owner };
			owned.AddComponent<PointLightComponent>();
			ownedId = owned.GetUUID();
			Entity other = CreateNetworked(scene, "OwnedByTwo");
			other.GetComponent<NetworkedComponent>() = { 2, NetworkAuthority::Owner };
			otherId = other.GetUUID();
		});
		RawPeer raw(session, session.Link().Connect(session.GetPort()));
		REQUIRE(raw.Join() == 1);
		const uint32_t transform = ComponentIndex("Transform");
		const uint32_t networked = ComponentIndex("Networked");
		const uint32_t light = ComponentIndex("PointLight");
		const uint32_t spot = ComponentIndex("SpotLight");
		uint32_t sequence = 0;
		const auto send = [&](NetEntityUpdate update) {
			NetState state;
			state.Sequence = ++sequence;
			state.Updates.push_back(std::move(update));
			raw.Send(NetProtocol::EncodeState(NetMessageType::ClientUpdate, state), NetChannel::Unreliable);
			session.Run(4);
			return session.HostNetwork().GetStatistics().RejectedRecords;
		};
		const auto moveTo = [&](UUID entity, double x) { return NetEntityUpdate{ entity, std::nullopt, { { transform, NetComponentChange::Changed, { { 0, { x, 0.0, 0.0 } } } } } }; };
		const Entity owned = host.GetEntityByUUID(ownedId);
		// One rule at a time: a server-authority entity, an entity another client owns.
		CHECK(send(moveTo(crateId, 50.0)) == 1);
		CHECK(host.GetEntityByUUID(crateId).GetComponent<TransformComponent>().Translation.x == 0.0f);
		CHECK(send(moveTo(otherId, 50.0)) == 2);
		CHECK(host.GetEntityByUUID(otherId).GetComponent<TransformComponent>().Translation.x == 0.0f);
		// On its own entity: ownership, a parent, removing or adding a component, and changing one the host's entity lacks.
		CHECK(send({ ownedId, std::nullopt, { { networked, NetComponentChange::Changed, { { 0, 2 } } } } }) == 3);
		CHECK(owned.GetComponent<NetworkedComponent>().Owner == 1);
		CHECK(send({ ownedId, crateId, {} }) == 4);
		CHECK_FALSE(owned.GetParent());
		CHECK(send({ ownedId, std::nullopt, { { light, NetComponentChange::Removed, {} } } }) == 5);
		CHECK(owned.HasComponent<PointLightComponent>());
		CHECK(send({ ownedId, std::nullopt, { { spot, NetComponentChange::Added, { { 0, { 1.0, 1.0, 1.0 } }, { 1, 5.0 } } } } }) == 6);
		CHECK_FALSE(owned.HasComponent<SpotLightComponent>());
		CHECK(send({ ownedId, std::nullopt, { { spot, NetComponentChange::Changed, { { 1, 5.0 } } } } }) == 7);
		CHECK_FALSE(owned.HasComponent<SpotLightComponent>());
		// What it may do: change its own entity's fields.
		CHECK(send(moveTo(ownedId, 4.0)) == 7);
		CHECK(owned.GetComponent<TransformComponent>().Translation.x == 4.0f);
		// A value the component refuses (a NaN) is refused here too, and the rest of that component's change with it.
		NetState invalid;
		invalid.Sequence = ++sequence;
		invalid.Updates.push_back({ ownedId, std::nullopt, { { transform, NetComponentChange::Changed, { { 0, { std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0 } }, { 2, { 2.0, 2.0, 2.0 } } } } } });
		raw.Send(NetProtocol::EncodeState(NetMessageType::ClientUpdate, invalid), NetChannel::Unreliable);
		session.Run(4);
		CHECK(session.HostNetwork().GetStatistics().RejectedValues == 1);
		CHECK(owned.GetComponent<TransformComponent>().Scale == glm::vec3(1.0f));
		// An older message is dropped.
		NetState stale;
		stale.Sequence = sequence - 1;
		stale.Updates.push_back(moveTo(ownedId, 8.0));
		raw.Send(NetProtocol::EncodeState(NetMessageType::ClientUpdate, stale), NetChannel::Unreliable);
		session.Run(4);
		CHECK(session.HostNetwork().GetStatistics().StaleMessages == 1);
		CHECK(owned.GetComponent<TransformComponent>().Translation.x == 4.0f);
		CHECK(session.HostNetwork().GetConnections().size() == 1);
	}

	TEST_CASE("The host ends a client's connection over messages a client must not send")
	{
		Session session;
		session.StartHost([](Scene& scene) { CreateNetworked(scene, "Crate"); });
		const auto kickedFor = [&](const std::vector<uint8_t>& message) {
			RawPeer raw(session, session.Link().Connect(session.GetPort()));
			raw.Join();
			raw.Send(message);
			const auto event = raw.WaitFor(NetTransportEvent::Kind::Disconnected);
			REQUIRE(event);
			CHECK(session.HostNetwork().GetConnections().empty());
			return DisconnectReasonFromCode(event->DisconnectCode);
		};
		// Anything that does not decode, a second Hello, a Snapshot, more than a client may send, and an acknowledgement of a
		// snapshot the host never sent.
		CHECK(kickedFor(Bytes({ 0xEE, 1, 2 })) == NetDisconnectReason::InvalidPacket);
		CHECK(kickedFor(NetProtocol::EncodeHello({ NetProtocol::Version, ReplicationSchema::Get().GetHash(), TestIdentity().Game, TestIdentity().GameVersion })) == NetDisconnectReason::InvalidPacket);
		NetState snapshot;
		snapshot.Sequence = 1;
		CHECK(kickedFor(NetProtocol::EncodeState(NetMessageType::Snapshot, snapshot)) == NetDisconnectReason::InvalidPacket);
		CHECK(kickedFor(std::vector<uint8_t>(NetworkSystem::MaxClientMessageBytes + 1, 0)) == NetDisconnectReason::InvalidPacket);
		NetState lying;
		lying.Sequence = 1;
		lying.Acknowledged = 100000;
		CHECK(kickedFor(NetProtocol::EncodeState(NetMessageType::ClientUpdate, lying)) == NetDisconnectReason::InvalidPacket);
	}

	TEST_CASE("The host refuses clients of another protocol, build or game, past its limit, or that never say hello")
	{
		NetworkSettings settings;
		settings.MaxClients = 1;
		settings.TimeoutSeconds = 2.0f;
		Session session({ .TimeoutSeconds = 5.0f }, settings);
		session.StartHost();
		const auto refused = [&](const std::vector<uint8_t>& hello) {
			RawPeer raw(session, session.Link().Connect(session.GetPort()));
			REQUIRE(raw.WaitFor(NetTransportEvent::Kind::Connected));
			raw.Send(hello);
			const auto event = raw.WaitFor(NetTransportEvent::Kind::Disconnected);
			REQUIRE(event);
			return DisconnectReasonFromCode(event->DisconnectCode);
		};
		const uint64_t hash = ReplicationSchema::Get().GetHash();
		CHECK(refused(NetProtocol::EncodeHello({ NetProtocol::Version + 1, hash, "NetTest", "1.0.0" })) == NetDisconnectReason::ProtocolMismatch);
		CHECK(refused(NetProtocol::EncodeHello({ NetProtocol::Version, hash + 1, "NetTest", "1.0.0" })) == NetDisconnectReason::SchemaMismatch);
		CHECK(refused(NetProtocol::EncodeHello({ NetProtocol::Version, hash, "OtherGame", "1.0.0" })) == NetDisconnectReason::GameMismatch);
		CHECK(refused(NetProtocol::EncodeHello({ NetProtocol::Version, hash, "NetTest", "1.0.1" })) == NetDisconnectReason::GameMismatch);
		CHECK(refused(NetProtocol::EncodeState(NetMessageType::ClientUpdate, { 1 })) == NetDisconnectReason::InvalidPacket);

		session.AddClient();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(0).GetState() == NetConnectionState::Connected; }, 60));
		session.AddClient();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(1).GetState() == NetConnectionState::Disconnected; }, 60));
		CHECK(session.ClientNetwork(1).GetDisconnectReason() == NetDisconnectReason::ServerFull);
		CHECK(session.ClientNetwork(0).GetState() == NetConnectionState::Connected);

		RawPeer silent(session, session.Link().Connect(session.GetPort()));
		REQUIRE(silent.WaitFor(NetTransportEvent::Kind::Connected));
		const auto event = silent.WaitFor(NetTransportEvent::Kind::Disconnected, 300);
		REQUIRE(event);
		CHECK(DisconnectReasonFromCode(event->DisconnectCode) == NetDisconnectReason::HandshakeTimeout);
	}

	TEST_CASE("Connections end with a reason: timeouts, failed attempts, quitting and the host stopping")
	{
		NetworkSettings settings;
		settings.TimeoutSeconds = 1.0f;
		Session session({ .TimeoutSeconds = 1.0f }, settings);
		session.StartHost([](Scene& scene) { CreateNetworked(scene, "Crate"); });
		session.AddClient();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(0).GetState() == NetConnectionState::Connected; }, 60));
		// The cable is cut: both sides time out.
		session.Link().SetLinkSettings({ .Loss = 1.0f, .TimeoutSeconds = 1.0f });
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(0).GetState() == NetConnectionState::Disconnected; }, 180));
		CHECK(session.ClientNetwork(0).GetDisconnectReason() == NetDisconnectReason::Timeout);
		CHECK(session.HostNetwork().GetConnections().empty());
		// A client whose host never answers did not connect at all.
		session.AddClient();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(1).GetState() == NetConnectionState::Disconnected; }, 180));
		CHECK(session.ClientNetwork(1).GetDisconnectReason() == NetDisconnectReason::ConnectionFailed);

		// Quitting and stopping are told to the other side.
		session.Link().SetLinkSettings({ .TimeoutSeconds = 1.0f });
		Scene& leaver = session.AddClient();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(2).GetState() == NetConnectionState::Connected; }, 60));
		leaver.StopNetwork();
		REQUIRE(session.RunUntil([&] { return session.HostNetwork().GetConnections().empty(); }, 60));
		session.AddClient();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(3).GetState() == NetConnectionState::Connected; }, 60));
		session.Host().StopNetwork();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(3).GetState() == NetConnectionState::Disconnected; }, 60));
		CHECK(session.ClientNetwork(3).GetDisconnectReason() == NetDisconnectReason::ServerShutdown);
	}

	TEST_CASE("Replication converges over a lossy, reordering link, and replays exactly")
	{
		const auto run = [](uint32_t seed) {
			Session session({ .Latency = 0.05f, .Jitter = 0.08f, .Loss = 0.25f, .TimeoutSeconds = 30.0f }, {}, seed);
			std::vector<UUID> ids;
			Scene& host = session.StartHost([&](Scene& scene) {
				for (int i = 0; i < 12; i++)
					ids.push_back(CreateNetworked(scene, "Box" + std::to_string(i), UUID(1000 + i), { static_cast<float>(i), 0.0f, 0.0f }).GetUUID());
			});
			Scene& client = session.AddClient();
			for (int frame = 0; frame < 120; frame++)
			{
				for (size_t i = 0; i < ids.size(); i++)
					host.GetEntityByUUID(ids[i]).GetComponent<TransformComponent>().Translation.y = static_cast<float>(frame) * 0.1f + static_cast<float>(i);
				session.Run(1);
			}
			// Once the host stops changing things, everything arrives.
			session.Run(240);
			CHECK(session.Link().GetLostCount() > 0);
			for (UUID id : ids)
			{
				Entity copy = client.GetEntityByUUID(id);
				REQUIRE(copy);
				CHECK(copy.GetComponent<TransformComponent>().Translation == host.GetEntityByUUID(id).GetComponent<TransformComponent>().Translation);
			}
			CHECK(session.ClientNetwork(0).GetStatistics().StaleMessages > 0);
			return SceneSerializer::ComputeStateHash(client);
		};
		CHECK(run(3) == run(3));
	}

	TEST_CASE("A small bandwidth budget spreads spawns over several ticks")
	{
		NetworkSettings settings;
		settings.MaxBytesPerSecond = NetworkSettings::MinBytesPerSecond;
		Session session({}, settings);
		session.StartHost([](Scene& scene) {
			for (int i = 0; i < 40; i++)
				CreateNetworked(scene, "Box" + std::to_string(i));
		});
		session.AddClient();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(0).GetRemoteEntityCount() > 0; }, 60));
		CHECK(session.ClientNetwork(0).GetRemoteEntityCount() < 40);
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(0).GetRemoteEntityCount() == 40; }, 60 * 60));
		// The host hears the last acknowledgement a tick later.
		REQUIRE(session.RunUntil([&] { return session.HostNetwork().GetConnections().front().KnownEntities == 40; }, 10));
	}

	TEST_CASE("Peers without authority drive dynamic bodies kinematically")
	{
		Session session;
		UUID crateId = 0;
		UUID avatarId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			AddGround(scene);
			Entity crate = CreateNetworked(scene, "Crate", 0, { 0.0f, 4.0f, 0.0f });
			crate.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
			crate.AddComponent<BoxColliderComponent>();
			crateId = crate.GetUUID();
			Entity avatar = CreateNetworked(scene, "Avatar", 0, { 3.0f, 4.0f, 0.0f });
			avatar.GetComponent<NetworkedComponent>() = { 1, NetworkAuthority::Owner };
			avatar.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
			avatar.AddComponent<SphereColliderComponent>();
			avatarId = avatar.GetUUID();
		});
		Scene& client = session.AddClient(AddGround);
		REQUIRE(session.RunUntil([&] { return client.GetEntityByUUID(crateId) && client.GetEntityByUUID(avatarId); }, 60));
		session.Run(2);
		Entity crate = client.GetEntityByUUID(crateId);
		Entity avatar = client.GetEntityByUUID(avatarId);
		// The component keeps its Type; physics treats it as the network says.
		CHECK(crate.GetComponent<RigidBodyComponent>().Type == RigidBodyType::Dynamic);
		CHECK(client.GetPhysicsWorld()->IsDrivenExternally(crate));
		CHECK_FALSE(client.GetPhysicsWorld()->IsDrivenExternally(avatar));
		CHECK_FALSE(host.GetPhysicsWorld()->IsDrivenExternally(host.GetEntityByUUID(crateId)));
		CHECK(host.GetPhysicsWorld()->IsDrivenExternally(host.GetEntityByUUID(avatarId)));
		// The crate falls on the host and the client shows where.
		session.Run(90);
		const float hostHeight = host.GetEntityByUUID(crateId).GetComponent<TransformComponent>().Translation.y;
		CHECK(hostHeight < 3.0f);
		CHECK(crate.GetComponent<TransformComponent>().Translation.y == doctest::Approx(hostHeight).epsilon(0.05));
		// The owner simulates its avatar, and the host follows.
		CHECK(host.GetEntityByUUID(avatarId).GetComponent<TransformComponent>().Translation.y == doctest::Approx(avatar.GetComponent<TransformComponent>().Translation.y).epsilon(0.05));
		CHECK(avatar.GetComponent<TransformComponent>().Translation.y < 3.0f);
		// With the owner gone, the host takes over.
		session.Client(0).StopNetwork();
		REQUIRE(session.RunUntil([&] { return session.HostNetwork().GetConnections().empty(); }, 60));
		session.Run(1);
		CHECK_FALSE(host.GetPhysicsWorld()->IsDrivenExternally(host.GetEntityByUUID(avatarId)));
		// Without a session, nothing is driven.
		CHECK_FALSE(client.GetPhysicsWorld()->IsDrivenExternally(crate));
	}

	TEST_CASE("Spawns from a host are checked like scene files, and asset paths stay inside the project")
	{
		FakeHost host([](Scene& scene) { scene.CreateEntityWithUUID(77, "Local"); });
		host.Welcome();
		REQUIRE(host.Network().GetState() == NetConnectionState::Connected);
		NetState state;
		// Outside the project: up, absolute, a sub-asset key hiding "..", a {UUID, Path} object, a backslash, a drive, and
		// a list element's asset field.
		state.Spawns.push_back(Spawn(10, { { "Mesh", { { "Mesh", "../../secret.gltf" } } } }));
		state.Spawns.push_back(Spawn(11, { { "Mesh", { { "Mesh", "/etc/passwd" } } } }));
		state.Spawns.push_back(Spawn(12, { { "Mesh", { { "Mesh", "Assets/x#/../../../secret.gltf" } } } }));
		state.Spawns.push_back(Spawn(13, { { "Mesh", { { "Mesh", { { "UUID", 5 }, { "Path", "../secret.gltf" } } } } } }));
		state.Spawns.push_back(Spawn(14, { { "Mesh", { { "Mesh", "..\\secret.gltf" } } } }));
		state.Spawns.push_back(Spawn(15, { { "Mesh", { { "Mesh", "C:/secret.gltf" } } } }));
		state.Spawns.push_back(Spawn(16, { { "Animator", { { "ClipLibraries", { { { "Mesh", "../secret.gltf" } } } } } } }));
		// Records a scene file would not pass, and a spawn whose first record is another entity.
		state.Spawns.push_back(Spawn(17, { { "Transform", { { "Scale", "big" } } } }));
		state.Spawns.push_back({ 18, 0, nlohmann::json::array({ Record(19, nlohmann::json::object()) }) });
		// Fine: a project file, and a sub-asset key.
		state.Spawns.push_back(Spawn(20, { { "Mesh", { { "Mesh", "Assets/Models/Ok.gltf" } } } }));
		state.Spawns.push_back(Spawn(21, { { "Material", { { "Material", "Assets/Models/Ok.gltf#material/0" } } } }));
		// An entity this client has already (an authored one the host made networked): it is the same entity.
		state.Spawns.push_back(Spawn(77, { { "Transform", { { "Translation", { 0.0, 7.0, 0.0 } } } } }));
		host.Snapshot(state);
		for (uint64_t id : { 10, 11, 12, 13, 14, 15, 16, 17, 18, 19 })
		{
			INFO("entity ", id);
			CHECK_FALSE(host.Client().GetEntityByUUID(id));
		}
		CHECK(host.Client().GetEntityByUUID(20));
		CHECK(host.Client().GetEntityByUUID(21));
		const Entity adopted = host.Client().GetEntityByUUID(77);
		CHECK(adopted.GetName() == "Local");
		CHECK(adopted.GetComponent<TransformComponent>().Translation.y == 7.0f);
		CHECK(host.Network().GetRemoteEntityCount() == 3);
		CHECK(host.Network().GetStatistics().RejectedRecords == 9);

		// Updates follow only for entities the host spawned (or the client took as such).
		NetState updates;
		updates.Updates.push_back({ 77, std::nullopt, { { ComponentIndex("Transform"), NetComponentChange::Changed, { { 0, { 0.0, 9.0, 0.0 } } } } } });
		updates.Updates.push_back({ 10, std::nullopt, { { ComponentIndex("Transform"), NetComponentChange::Changed, { { 0, { 0.0, 9.0, 0.0 } } } } } });
		host.Snapshot(updates);
		CHECK(adopted.GetComponent<TransformComponent>().Translation.y == 9.0f);
		CHECK(host.Network().GetStatistics().RejectedRecords == 10);

		// A message that does not decode ends the session from the client's side.
		host.Send(Bytes({ 3, 0 }));
		CHECK(host.Network().GetState() == NetConnectionState::Disconnected);
		CHECK(host.Network().GetDisconnectReason() == NetDisconnectReason::InvalidPacket);
	}

	TEST_CASE("A repeated spawn refreshes what the first created, and a child may arrive before its parent")
	{
		FakeHost host;
		host.Welcome();
		NetState first;
		first.Spawns.push_back(Spawn(30, { { "Transform", { { "Translation", { 1.0, 0.0, 0.0 } } } }, { "PointLight", nlohmann::json::object() } }));
		// The child names a parent that has not arrived.
		first.Spawns.push_back(Spawn(31, nlohmann::json::object(), 40));
		host.Snapshot(first);
		const Entity entity = host.Client().GetEntityByUUID(30);
		REQUIRE(entity);
		REQUIRE(host.Client().GetEntityByUUID(31));
		CHECK_FALSE(host.Client().GetEntityByUUID(31).GetParent());
		// The repeat (its first message unacknowledged) holds the host's newer state: applied, components included.
		NetState repeat;
		repeat.Spawns.push_back(Spawn(30, { { "Transform", { { "Translation", { 2.0, 0.0, 0.0 } } } }, { "SpotLight", nlohmann::json::object() } }));
		repeat.Spawns.push_back(Spawn(40, nlohmann::json::object()));
		host.Snapshot(repeat);
		CHECK(entity.GetComponent<TransformComponent>().Translation.x == 2.0f);
		CHECK_FALSE(entity.HasComponent<PointLightComponent>());
		CHECK(entity.HasComponent<SpotLightComponent>());
		CHECK(host.Network().GetStatistics().SpawnsApplied == 3);
		// The parent arrived: the child goes under it.
		CHECK(host.Client().GetEntityByUUID(31).GetParent() == host.Client().GetEntityByUUID(40));

		// An entity this client decides: a repeated spawn changes who owns it and which components it has, not its values.
		NetState owned;
		owned.Spawns.push_back(Spawn(50, { { "Transform", { { "Translation", { 1.0, 0.0, 0.0 } } } }, { "Networked", { { "Owner", 1 }, { "Authority", "Owner" } } } }));
		host.Snapshot(owned);
		const Entity mine = host.Client().GetEntityByUUID(50);
		REQUIRE(mine);
		CHECK(host.Network().HasAuthority(mine));
		NetState ownedAgain;
		ownedAgain.Spawns.push_back(Spawn(50, { { "Transform", { { "Translation", { 5.0, 0.0, 0.0 } } } }, { "PointLight", nlohmann::json::object() }, { "Networked", { { "Owner", 1 }, { "Authority", "Owner" } } } }));
		host.Snapshot(ownedAgain);
		CHECK(mine.GetComponent<TransformComponent>().Translation.x == 1.0f);
		CHECK(mine.HasComponent<PointLightComponent>());
		// Handed to another client in the same message: ownership applies first, then the host's values.
		NetState handedOver;
		handedOver.Spawns.push_back(Spawn(50, { { "Transform", { { "Translation", { 5.0, 0.0, 0.0 } } } }, { "PointLight", nlohmann::json::object() }, { "Networked", { { "Owner", 2 }, { "Authority", "Owner" } } } }));
		host.Snapshot(handedOver);
		CHECK(mine.GetComponent<NetworkedComponent>().Owner == 2);
		CHECK_FALSE(host.Network().HasAuthority(mine));
		CHECK(mine.GetComponent<TransformComponent>().Translation.x == 5.0f);
	}

	TEST_CASE("A client's handshake: a second Welcome, a snapshot before it, no Welcome at all, a lying acknowledgement")
	{
		{
			FakeHost host([](Scene& scene) {
				Entity mine = CreateNetworked(scene, "Mine");
				mine.GetComponent<NetworkedComponent>() = { 1, NetworkAuthority::Owner };
			});
			// Before the Welcome a client decides nothing networked, even what names it as owner.
			CHECK_FALSE(host.Network().HasAuthority(host.Client().FindEntityByName("Mine")));
			NetState early;
			early.Spawns.push_back(Spawn(60, nlohmann::json::object()));
			host.Snapshot(early);
			CHECK_FALSE(host.Client().GetEntityByUUID(60));
			host.Welcome();
			CHECK(host.Network().GetState() == NetConnectionState::Connected);
			host.Welcome();
			CHECK(host.Network().GetState() == NetConnectionState::Disconnected);
			CHECK(host.Network().GetDisconnectReason() == NetDisconnectReason::InvalidPacket);
		}
		{
			NetworkSettings settings;
			settings.TimeoutSeconds = 1.0f;
			FakeHost host({}, settings);
			host.Advance(90);
			CHECK(host.Network().GetState() == NetConnectionState::Disconnected);
			CHECK(host.Network().GetDisconnectReason() == NetDisconnectReason::HandshakeTimeout);
		}
		{
			FakeHost host;
			host.Welcome();
			NetState lying;
			lying.Acknowledged = 100000;
			host.Snapshot(lying);
			CHECK(host.Network().GetState() == NetConnectionState::Disconnected);
			CHECK(host.Network().GetDisconnectReason() == NetDisconnectReason::InvalidPacket);
		}
	}

	TEST_CASE("Networked descendants outlive a despawned parent, and tree members can become networked")
	{
		Session session;
		UUID vehicleId = 0;
		UUID turretId = 0;
		UUID wheelId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			Entity vehicle = CreateNetworked(scene, "Vehicle");
			Entity turret = scene.CreateChildEntity(vehicle, "Turret");
			turret.AddComponent<NetworkedComponent>();
			wheelId = scene.CreateChildEntity(vehicle, "Wheel").GetUUID();
			vehicleId = vehicle.GetUUID();
			turretId = turret.GetUUID();
		});
		Scene& client = session.AddClient();
		REQUIRE(session.RunUntil([&] { return client.GetEntityByUUID(vehicleId) && client.GetEntityByUUID(turretId); }, 60));
		REQUIRE(client.GetEntityByUUID(wheelId));
		// The wheel came inside the vehicle's spawn; made networked, it is the same entity and updates follow.
		host.GetEntityByUUID(wheelId).AddComponent<NetworkedComponent>();
		session.Run(10);
		Patch(host.GetEntityByUUID(wheelId), "Transform", { { "Translation", { 0.0, 0.0, 3.0 } } });
		session.Run(10);
		CHECK(client.FindEntitiesByName("Wheel").size() == 1);
		CHECK(client.GetEntityByUUID(wheelId).GetComponent<TransformComponent>().Translation.z == 3.0f);
		CHECK(session.ClientNetwork(0).GetStatistics().RejectedRecords == 0);
		// The vehicle stops being networked: it goes from the client, its networked children stay (at the root) and still
		// update.
		host.GetEntityByUUID(vehicleId).RemoveComponent<NetworkedComponent>();
		REQUIRE(session.RunUntil([&] { return !client.GetEntityByUUID(vehicleId); }, 30));
		REQUIRE(client.GetEntityByUUID(turretId));
		CHECK_FALSE(client.GetEntityByUUID(turretId).GetParent());
		Patch(host.GetEntityByUUID(turretId), "Transform", { { "Translation", { 4.0, 0.0, 0.0 } } });
		session.Run(10);
		CHECK(client.GetEntityByUUID(turretId).GetComponent<TransformComponent>().Translation.x == 4.0f);
		CHECK(session.ClientNetwork(0).GetRemoteEntityCount() == 2);
		// Destroyed on the host, everything goes.
		host.DestroyEntity(host.GetEntityByUUID(vehicleId));
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(0).GetRemoteEntityCount() == 0; }, 30));
		CHECK_FALSE(client.GetEntityByUUID(turretId));
		CHECK_FALSE(client.GetEntityByUUID(wheelId));
	}

	TEST_CASE("An authored entity the host makes networked during play is the same entity on clients")
	{
		const auto build = [](Scene& scene) { scene.CreateEntityWithUUID(700, "Door"); };
		Session session;
		Scene& host = session.StartHost(build);
		Scene& client = session.AddClient(build);
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(0).GetState() == NetConnectionState::Connected; }, 60));
		Entity door = host.GetEntityByUUID(700);
		door.AddComponent<NetworkedComponent>();
		Patch(door, "Transform", { { "Translation", { 0.0, 2.5, 0.0 } } });
		session.Run(10);
		CHECK(client.FindEntitiesByName("Door").size() == 1);
		CHECK(client.GetEntityByUUID(700).GetComponent<TransformComponent>().Translation.y == 2.5f);
		CHECK(client.GetEntityByUUID(700).HasComponent<NetworkedComponent>());
		Patch(door, "Transform", { { "Translation", { 0.0, 4.0, 0.0 } } });
		session.Run(10);
		CHECK(client.GetEntityByUUID(700).GetComponent<TransformComponent>().Translation.y == 4.0f);
	}

	TEST_CASE("An owner hears of components the host adds or removes on its entity, and keeps its own values")
	{
		Session session;
		UUID avatarId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			Entity avatar = CreateNetworked(scene, "Avatar");
			avatar.GetComponent<NetworkedComponent>() = { 1, NetworkAuthority::Owner };
			avatarId = avatar.GetUUID();
		});
		Scene& owner = session.AddClient();
		REQUIRE(session.RunUntil([&] { return static_cast<bool>(owner.GetEntityByUUID(avatarId)); }, 60));
		Patch(owner.GetEntityByUUID(avatarId), "Transform", { { "Translation", { 6.0, 0.0, 0.0 } } });
		session.Run(10);
		Patch(host.GetEntityByUUID(avatarId), "PointLight", { { "Intensity", 3.0 } });
		session.Run(10);
		Entity mine = owner.GetEntityByUUID(avatarId);
		REQUIRE(mine.HasComponent<PointLightComponent>());
		CHECK(mine.GetComponent<PointLightComponent>().Intensity == 3.0f);
		CHECK(mine.GetComponent<TransformComponent>().Translation.x == 6.0f);
		// Later field changes are the owner's: the host's do not reach it, the owner's reach the host.
		Patch(host.GetEntityByUUID(avatarId), "PointLight", { { "Intensity", 8.0 } });
		session.Run(10);
		CHECK(mine.GetComponent<PointLightComponent>().Intensity == 3.0f);
		Patch(mine, "PointLight", { { "Intensity", 5.0 } });
		session.Run(10);
		CHECK(host.GetEntityByUUID(avatarId).GetComponent<PointLightComponent>().Intensity == 5.0f);
		host.GetEntityByUUID(avatarId).RemoveComponent<PointLightComponent>();
		session.Run(10);
		CHECK_FALSE(mine.HasComponent<PointLightComponent>());
	}

	TEST_CASE("The host sends an owner who owns what, and which components it has, never the values it decides")
	{
		Session session;
		UUID ownedId = 0;
		UUID crateId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			Entity owned = CreateNetworked(scene, "Owned");
			owned.GetComponent<NetworkedComponent>() = { 1, NetworkAuthority::Owner };
			ownedId = owned.GetUUID();
			crateId = CreateNetworked(scene, "Crate").GetUUID();
		});
		RawPeer raw(session, session.Link().Connect(session.GetPort()));
		REQUIRE(raw.Join() == 1);
		// Acknowledges every snapshot (as a client would) and collects their updates.
		uint32_t sequence = 0;
		std::vector<NetEntityUpdate> updates;
		const auto listen = [&](int frames) {
			for (int i = 0; i < frames; i++)
			{
				std::vector<NetTransportEvent> events;
				raw.Transport().Poll(events);
				for (const NetTransportEvent& event : events)
				{
					const auto message = DecodeMessage(event.Data);
					if (!message || message->Type != NetMessageType::Snapshot)
						continue;
					const NetState& state = std::get<NetState>(message->Body);
					updates.insert(updates.end(), state.Updates.begin(), state.Updates.end());
					NetState ack;
					ack.Sequence = ++sequence;
					ack.Acknowledged = state.Sequence;
					raw.Send(NetProtocol::EncodeState(NetMessageType::ClientUpdate, ack), NetChannel::Unreliable);
				}
				session.Run(1);
			}
		};
		listen(20);
		updates.clear();
		Patch(host.GetEntityByUUID(ownedId), "Transform", { { "Translation", { 1.0, 0.0, 0.0 } } });
		Patch(host.GetEntityByUUID(crateId), "Transform", { { "Translation", { 2.0, 0.0, 0.0 } } });
		Patch(host.GetEntityByUUID(ownedId), "PointLight", { { "Intensity", 3.0 } });
		Patch(host.GetEntityByUUID(ownedId), "Networked", { { "Priority", 2.0 }, { "Authority", "Owner" } });
		listen(20);
		const uint32_t transform = ComponentIndex("Transform");
		bool crateMoved = false;
		bool lightAdded = false;
		for (const NetEntityUpdate& update : updates)
			for (const NetComponentUpdate& change : update.Components)
			{
				if (update.Entity == ownedId)
					CHECK_FALSE(change.Component == transform);
				if (update.Entity == ownedId && change.Component == ComponentIndex("PointLight"))
					lightAdded = change.Change == NetComponentChange::Added;
				if (update.Entity == crateId && change.Component == transform)
					crateMoved = true;
			}
		CHECK(crateMoved);
		CHECK(lightAdded);
	}

	TEST_CASE("An entity despawned and back before the client acknowledged ends up as one live copy, over a lossy link")
	{
		Session session({ .Latency = 0.03f, .Jitter = 0.03f, .Loss = 0.4f, .TimeoutSeconds = 30.0f }, {}, 5);
		UUID crateId = 0;
		Scene& host = session.StartHost([&](Scene& scene) { crateId = CreateNetworked(scene, "Crate", 900).GetUUID(); });
		Scene& client = session.AddClient();
		REQUIRE(session.RunUntil([&] { return static_cast<bool>(client.GetEntityByUUID(crateId)); }, 600));
		Entity crate = host.GetEntityByUUID(crateId);
		for (int i = 0; i < 5; i++)
		{
			crate.RemoveComponent<NetworkedComponent>();
			session.Run(2);
			crate.AddComponent<NetworkedComponent>();
			session.Run(1);
		}
		Patch(crate, "Transform", { { "Translation", { 0.0, 0.0, 9.0 } } });
		REQUIRE(session.RunUntil([&] { return client.GetEntityByUUID(crateId) && client.GetEntityByUUID(crateId).GetComponent<TransformComponent>().Translation.z == 9.0f; }, 600));
		CHECK(client.FindEntitiesByName("Crate").size() == 1);
		CHECK(session.Link().GetLostCount() > 0);
		Patch(crate, "Transform", { { "Translation", { 0.0, 0.0, 2.0 } } });
		REQUIRE(session.RunUntil([&] { return client.GetEntityByUUID(crateId).GetComponent<TransformComponent>().Translation.z == 2.0f; }, 600));
	}

	TEST_CASE("A value that cannot be sent leaves the component on clients, and a tree too large for a spawn is not sent")
	{
		Session session;
		UUID riggedId = 0;
		UUID hugeId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			Entity rigged = CreateNetworked(scene, "Rigged");
			rigged.AddComponent<AnimatorComponent>().Clip = "Walk";
			riggedId = rigged.GetUUID();
			Entity huge = CreateNetworked(scene, "Huge");
			for (uint32_t i = 0; i < NetProtocol::MaxSpawnEntities; i++)
				scene.CreateChildEntity(huge, "Part");
			hugeId = huge.GetUUID();
		});
		Scene& client = session.AddClient();
		REQUIRE(session.RunUntil([&] { return static_cast<bool>(client.GetEntityByUUID(riggedId)); }, 60));
		session.Run(4);
		CHECK_FALSE(client.GetEntityByUUID(hugeId));
		CHECK(session.ClientNetwork(0).GetState() == NetConnectionState::Connected);
		// C++ can store text that is not UTF-8 (Lua strings too): the component stays as it was on the client.
		host.GetEntityByUUID(riggedId).GetComponent<AnimatorComponent>().Clip = "\xFF\xFE";
		session.Run(10);
		REQUIRE(client.GetEntityByUUID(riggedId).HasComponent<AnimatorComponent>());
		CHECK(client.GetEntityByUUID(riggedId).GetComponent<AnimatorComponent>().Clip == "Walk");
		Patch(host.GetEntityByUUID(riggedId), "Animator", { { "Clip", "Run" } });
		session.Run(10);
		CHECK(client.GetEntityByUUID(riggedId).GetComponent<AnimatorComponent>().Clip == "Run");
		// One added while its value cannot be sent waits, then arrives whole.
		Entity late = CreateNetworked(host, "Late");
		REQUIRE(session.RunUntil([&] { return static_cast<bool>(client.GetEntityByUUID(late.GetUUID())); }, 30));
		late.AddComponent<AnimatorComponent>().Clip = "\xFF";
		session.Run(10);
		CHECK_FALSE(client.GetEntityByUUID(late.GetUUID()).HasComponent<AnimatorComponent>());
		Patch(late, "Animator", { { "Clip", "Idle" } });
		session.Run(10);
		REQUIRE(client.GetEntityByUUID(late.GetUUID()).HasComponent<AnimatorComponent>());
		CHECK(client.GetEntityByUUID(late.GetUUID()).GetComponent<AnimatorComponent>().Clip == "Idle");
	}

	TEST_CASE("A client without its host decides nothing, and a session attached during play")
	{
		Session session;
		UUID mineId = 0;
		session.StartHost([&](Scene& scene) {
			AddGround(scene);
			Entity mine = CreateNetworked(scene, "Mine", 0, { 0.0f, 3.0f, 0.0f });
			mine.GetComponent<NetworkedComponent>() = { 1, NetworkAuthority::Owner };
			mine.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
			mine.AddComponent<BoxColliderComponent>();
			mineId = mine.GetUUID();
		});
		Scene& client = session.AddClient(AddGround);
		REQUIRE(session.RunUntil([&] { return static_cast<bool>(client.GetEntityByUUID(mineId)); }, 60));
		session.Run(2);
		Entity mine = client.GetEntityByUUID(mineId);
		CHECK(session.ClientNetwork(0).HasAuthority(mine));
		CHECK_FALSE(client.GetPhysicsWorld()->IsDrivenExternally(mine));
		session.Host().StopNetwork();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(0).GetState() == NetConnectionState::Disconnected; }, 60));
		session.Run(1);
		CHECK_FALSE(session.ClientNetwork(0).HasAuthority(mine));
		CHECK(client.GetPhysicsWorld()->IsDrivenExternally(mine));

		// Attached mid-play: a host keeps the stream it has (replaying it could reissue the ID of an entity destroyed
		// meanwhile), a client moves to its own.
		SimulatedNetwork link;
		std::string error;
		Scene hostScene("LateHost");
		Scene offline("Offline");
		hostScene.OnRuntimeStart();
		offline.OnRuntimeStart();
		CHECK(hostScene.CreateEntity("A").GetUUID() == offline.CreateEntity("A").GetUUID());
		// Destroyed: a replayed stream would hand its ID out again.
		hostScene.DestroyEntity(hostScene.FindEntityByName("A"));
		offline.DestroyEntity(offline.FindEntityByName("A"));
		auto listening = link.Listen(0, 4, error);
		const uint16_t port = listening->GetListenPort();
		hostScene.StartNetwork(CreateScope<NetworkSystem>(hostScene, NetworkRole::Host, std::move(listening), NetworkSettings{}, TestIdentity()));
		CHECK(hostScene.CreateEntity("B").GetUUID() == offline.CreateEntity("B").GetUUID());
		Scene clientScene("LateClient");
		clientScene.OnRuntimeStart();
		clientScene.CreateEntity("A");
		clientScene.StartNetwork(CreateScope<NetworkSystem>(clientScene, NetworkRole::Client, link.Connect(port), NetworkSettings{}, TestIdentity()));
		CHECK(clientScene.CreateEntity("B").GetUUID() != offline.CreateEntity("C").GetUUID());
		clientScene.OnRuntimeStop();
		hostScene.OnRuntimeStop();
		offline.OnRuntimeStop();
	}

	TEST_CASE("Network sessions: client IDs come from their own stream, and scene loads are refused")
	{
		Session session;
		Scene& host = session.StartHost();
		Scene& client = session.AddClient();
		Scene offline("Offline");
		offline.OnRuntimeStart();
		// The same seed: an offline game and the host draw the same IDs; a client does not.
		const UUID hostId = host.CreateEntity("A").GetUUID();
		CHECK(offline.CreateEntity("A").GetUUID() == hostId);
		CHECK(client.CreateEntity("A").GetUUID() != hostId);
		offline.OnRuntimeStop();
		std::string error;
		CHECK_FALSE(host.RequestLoad("Assets/Scenes/Other.bscene", error));
		CHECK(error == "scenes cannot be loaded during a network session");
		// A network session runs only while playing; stopping ends it.
		host.OnRuntimeStop();
		CHECK_FALSE(host.GetNetworkSystem());
	}

	TEST_CASE("NetworkSettings: project file and project.set")
	{
		std::string error;
		const auto settings = NetworkSettings::FromJson({ { "Port", 9000 }, { "MaxClients", 2 }, { "TickRate", 60 }, { "MaxBytesPerSecond", 4096 }, { "TimeoutSeconds", 3 } }, error);
		REQUIRE(settings);
		CHECK(settings->Port == 9000);
		CHECK(settings->TickRate == 60.0f);
		CHECK(NetworkSettings::FromJson(settings->ToJson(), error) == settings);
		CHECK(NetworkSettings::FromJson(nlohmann::json::object(), error) == NetworkSettings{});
		for (const nlohmann::json& bad : { nlohmann::json{ { "Port", 0 } }, nlohmann::json{ { "Port", 65536 } }, nlohmann::json{ { "Port", 9000.5 } }, nlohmann::json{ { "MaxClients", -1 } }, nlohmann::json{ { "MaxClients", 0 } }, nlohmann::json{ { "MaxClients", 1025 } }, nlohmann::json{ { "TickRate", 0.5 } }, nlohmann::json{ { "TickRate", 240.5 } }, nlohmann::json{ { "TickRate", "fast" } }, nlohmann::json{ { "MaxBytesPerSecond", 1023 } }, nlohmann::json{ { "MaxBytesPerSecond", 104857601 } }, nlohmann::json{ { "TimeoutSeconds", 0.5 } }, nlohmann::json{ { "TimeoutSeconds", 600.5 } }, nlohmann::json{ { "Unknown", 1 } }, nlohmann::json(3) })
		{
			INFO(bad.dump());
			CHECK_FALSE(NetworkSettings::FromJson(bad, error));
			CHECK(error.find("Network") == 0);
		}
		// The edges themselves are fine.
		for (const nlohmann::json& edge : { nlohmann::json{ { "Port", 1 } }, nlohmann::json{ { "Port", 65535 } }, nlohmann::json{ { "MaxClients", 1 } }, nlohmann::json{ { "MaxClients", 1024 } }, nlohmann::json{ { "TickRate", 1 } }, nlohmann::json{ { "TickRate", 240 } }, nlohmann::json{ { "MaxBytesPerSecond", 1024 } }, nlohmann::json{ { "MaxBytesPerSecond", 104857600 } }, nlohmann::json{ { "TimeoutSeconds", 1 } }, nlohmann::json{ { "TimeoutSeconds", 600 } } })
		{
			INFO(edge.dump());
			CHECK(NetworkSettings::FromJson(edge, error));
		}
		NetworkSettings outOfRange;
		outOfRange.TickRate = std::numeric_limits<float>::quiet_NaN();
		CHECK_FALSE(outOfRange.Validate().empty());
		CHECK(NetworkSettings{}.Validate().empty());

		BrecciaTest::TempProject temp("NetworkSettingsProject");
		REQUIRE(temp.IsValid());
		CommandRegistry registry;
		AutomationSession session;
		CHECK(BrecciaTest::Run(registry, session, "project.info")["network"] == NetworkSettings{}.ToJson());
		BrecciaTest::Run(registry, session, "project.set", { { "network", { { "Port", 9100 }, { "TickRate", 20 } } } });
		CHECK(Project::GetActive()->GetConfig().Network.Port == 9100);
		CHECK(Project::GetActive()->GetConfig().Network.TickRate == 20.0f);
		CHECK(BrecciaTest::Contains(BrecciaTest::RunError(registry, session, "project.set", { { "network", { { "MaxClients", 0 } } } }), "Network.MaxClients"));
		CHECK(Project::GetActive()->GetConfig().Network.Port == 9100);
		// Saved and loaded with the project.
		const Ref<Project> loaded = Project::Load(temp.GetDirectory(), error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->GetConfig().Network.Port == 9100);
		FileSystem::WriteTextFile(temp.GetDirectory() / Project::FileName, R"({"Name":"Bad","Network":{"TickRate":1000}})");
		CHECK_FALSE(Project::Load(temp.GetDirectory(), error));
		CHECK(BrecciaTest::Contains(error, "Network.TickRate"));
	}

	TEST_CASE("PhysicsWorld drives a dynamic body kinematically on request and gives it back")
	{
		Scene scene;
		Entity crate = scene.CreateEntity("Crate");
		crate.GetComponent<TransformComponent>().Translation = { 0.0f, 10.0f, 0.0f };
		crate.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		crate.AddComponent<BoxColliderComponent>();
		scene.OnRuntimeStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		physics.SetDrivenExternally(crate, true);
		for (int i = 0; i < 30; i++)
			scene.OnUpdate(Step);
		// It stays where the transform says, and follows a new one.
		CHECK(crate.GetComponent<TransformComponent>().Translation.y == 10.0f);
		crate.GetComponent<TransformComponent>().Translation = { 2.0f, 5.0f, 0.0f };
		for (int i = 0; i < 2; i++)
			scene.OnUpdate(Step);
		CHECK(crate.GetComponent<TransformComponent>().Translation == glm::vec3(2.0f, 5.0f, 0.0f));
		physics.SetDrivenExternally(crate, false);
		for (int i = 0; i < 30; i++)
			scene.OnUpdate(Step);
		CHECK(crate.GetComponent<TransformComponent>().Translation.y < 5.0f);
		CHECK_FALSE(physics.IsDrivenExternally(crate));
		scene.OnRuntimeStop();
	}
}
