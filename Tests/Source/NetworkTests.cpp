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
		const ComponentInfo* transform = schema.FindComponent(ComponentIndex("Transform"))->Info;
		CHECK(transform->ReplicatedFields == std::vector<std::string>{ "Translation", "Rotation", "Scale" });
		CHECK(schema.FindComponent(ComponentIndex("Transform"))->IsCore);
		CHECK(schema.FindComponent(ComponentIndex("Networked"))->Info->ReplicatedFields == std::vector<std::string>{ "Owner", "Authority" });
		CHECK_FALSE(schema.FindComponent(static_cast<uint32_t>(schema.GetComponents().size())));
		CHECK(schema.GetHash() != 0);
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
		state.Updates.push_back({ 11, UUID(10), { { transform, false, false, { { 0, { 1.0, 2.0, 3.0 } } } }, { light, true, false, {} } } });
		state.Updates.push_back({ 12, std::nullopt, { { light, false, true, { { 0, { 1.0, 1.0, 1.0 } }, { 1, 4.0 } } } } });
		state.Despawns.push_back(13);
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
		CHECK(back.Updates[0].Components[1].Removed);
		CHECK(back.Updates[1].Components[0].Added);
		CHECK(back.Despawns == std::vector<UUID>{ 13 });
		CHECK(NetProtocol::EncodeState(NetMessageType::Snapshot, back) == bytes);
		// Trailing bytes, and clients sending spawns or despawns, are invalid.
		std::vector<uint8_t> trailing = bytes;
		trailing.push_back(0);
		CHECK_FALSE(DecodeMessage(trailing));
		CHECK_FALSE(DecodeMessage(NetProtocol::EncodeState(NetMessageType::ClientUpdate, state)));
	}

	TEST_CASE("NetProtocol rejects records the schema does not allow")
	{
		const uint32_t transform = ComponentIndex("Transform");
		const uint32_t light = ComponentIndex("PointLight");
		const auto encodes = [](const NetState& state) { return DecodeMessage(NetProtocol::EncodeState(NetMessageType::Snapshot, state)).has_value(); };
		const auto withUpdate = [](NetEntityUpdate update) {
			NetState state;
			state.Sequence = 1;
			state.Updates.push_back(std::move(update));
			return state;
		};
		CHECK(encodes(withUpdate({ 1, std::nullopt, { { transform, false, false, { { 2, { 1.0, 1.0, 1.0 } } } } } })));
		// An unknown component, a field past the replicated ones, a field twice, a core component added or removed, an
		// added component without every field, an empty entry, an entity that is its own parent, entity 0.
		CHECK_FALSE(encodes(withUpdate({ 1, std::nullopt, { { 100000, false, false, { { 0, 1 } } } } })));
		CHECK_FALSE(encodes(withUpdate({ 1, std::nullopt, { { transform, false, false, { { 3, 1 } } } } })));
		CHECK_FALSE(encodes(withUpdate({ 1, std::nullopt, { { transform, false, false, { { 0, 1 }, { 0, 1 } } } } })));
		CHECK_FALSE(encodes(withUpdate({ 1, std::nullopt, { { transform, true, false, {} } } })));
		CHECK_FALSE(encodes(withUpdate({ 1, std::nullopt, { { light, false, true, { { 0, 1 } } } } })));
		CHECK_FALSE(encodes(withUpdate({ 1, std::nullopt, { { light, false, false, {} } } })));
		CHECK_FALSE(encodes(withUpdate({ 1, std::nullopt, {} })));
		CHECK_FALSE(encodes(withUpdate({ 1, UUID(1), {} })));
		CHECK_FALSE(encodes(withUpdate({ 0, UUID(5), {} })));
		// One entity twice in a message.
		NetState twice = withUpdate({ 1, UUID(5), {} });
		twice.Despawns.push_back(1);
		CHECK_FALSE(encodes(twice));
		NetState zero = withUpdate({ 1, UUID(5), {} });
		zero.Sequence = 0;
		CHECK_FALSE(encodes(zero));
		NetState emptySpawn;
		emptySpawn.Sequence = 1;
		emptySpawn.Spawns.push_back({ 2, 0, nlohmann::json::array() });
		CHECK_FALSE(encodes(emptySpawn));
		CHECK(DisconnectReasonFromCode(static_cast<uint32_t>(NetDisconnectReason::ServerFull)) == NetDisconnectReason::ServerFull);
		CHECK(DisconnectReasonFromCode(9999) == NetDisconnectReason::None);
		CHECK(std::string(DescribeDisconnectReason(static_cast<NetDisconnectReason>(9999))) == "unknown reason");
	}

	TEST_CASE("ReplicationSender repeats what is unacknowledged and sends only changes after")
	{
		const uint32_t transform = ComponentIndex("Transform");
		const auto field = [](const nlohmann::json& value) { return ReplicatedFieldValue{ Encode(value), value }; };
		const auto entityAt = [&](float x) {
			ReplicatedEntity entity;
			entity.Entity = 100;
			entity.Components.push_back({ transform, { field({ x, 0.0, 0.0 }), field({ 0.0, 0.0, 0.0 }), field({ 1.0, 1.0, 1.0 }) } });
			return entity;
		};
		int spawnsBuilt = 0;
		const auto world = [&](std::vector<ReplicatedEntity> entities) {
			return ReplicationWorld(std::move(entities), [&](const ReplicatedEntity& entity) {
				spawnsBuilt++;
				NetWriter writer;
				NetProtocol::WriteSpawn(writer, { entity.Entity, 0, nlohmann::json::array({ { { "ID", static_cast<uint64_t>(entity.Entity) } } }) });
				return std::optional<std::vector<uint8_t>>(writer.TakeBytes());
			});
		};
		const auto decode = [](const std::vector<uint8_t>& bytes) {
			const auto message = DecodeMessage(bytes);
			REQUIRE(message);
			return std::get<NetState>(message->Body);
		};

		ReplicationSender sender;
		// Unacknowledged spawns repeat (built once per world).
		NetState first = decode(sender.Build(NetMessageType::Snapshot, world({ entityAt(1) }), 0, 100000));
		CHECK(first.Sequence == 1);
		CHECK(first.Spawns.size() == 1);
		CHECK(decode(sender.Build(NetMessageType::Snapshot, world({ entityAt(1) }), 0, 100000)).Spawns.size() == 1);
		CHECK(spawnsBuilt == 2);
		// Acknowledging the second makes it known; nothing changed since, so nothing goes.
		sender.Acknowledge(2);
		CHECK(sender.GetKnownEntityCount() == 1);
		NetState quiet = decode(sender.Build(NetMessageType::Snapshot, world({ entityAt(1) }), 0, 100000));
		CHECK((quiet.Spawns.empty() && quiet.Updates.empty()));
		// A change goes as just that field, and repeats until acknowledged.
		NetState moved = decode(sender.Build(NetMessageType::Snapshot, world({ entityAt(2) }), 0, 100000));
		REQUIRE(moved.Updates.size() == 1);
		REQUIRE(moved.Updates[0].Components.size() == 1);
		CHECK(moved.Updates[0].Components[0].Fields.size() == 1);
		CHECK(moved.Updates[0].Components[0].Fields[0].Field == 0);
		// Moving back to the acknowledged value still sends it: the receiver may have applied the unacknowledged one.
		NetState back = decode(sender.Build(NetMessageType::Snapshot, world({ entityAt(1) }), 0, 100000));
		REQUIRE(back.Updates.size() == 1);
		CHECK(back.Updates[0].Components[0].Fields[0].Value == nlohmann::json{ 1.0, 0.0, 0.0 });
		// The receiver applied the newest (acknowledging it drops the older one, whatever it held).
		sender.Acknowledge(back.Sequence);
		CHECK(decode(sender.Build(NetMessageType::Snapshot, world({ entityAt(1) }), 0, 100000)).Updates.empty());
		// An acknowledgement of a message older than one already acknowledged changes nothing.
		sender.Acknowledge(moved.Sequence);
		CHECK(decode(sender.Build(NetMessageType::Snapshot, world({ entityAt(1) }), 0, 100000)).Updates.empty());

		// Gone: despawns repeat until acknowledged, and a new entity with the ID waits for that.
		CHECK(decode(sender.Build(NetMessageType::Snapshot, world({}), 0, 100000)).Despawns == std::vector<UUID>{ 100 });
		NetState again = decode(sender.Build(NetMessageType::Snapshot, world({ entityAt(5) }), 0, 100000));
		CHECK(again.Despawns.empty());
		CHECK(again.Spawns.empty());
		NetState despawned = decode(sender.Build(NetMessageType::Snapshot, world({}), 0, 100000));
		CHECK(despawned.Despawns == std::vector<UUID>{ 100 });
		sender.Acknowledge(despawned.Sequence);
		CHECK(sender.GetKnownEntityCount() == 0);
		CHECK(decode(sender.Build(NetMessageType::Snapshot, world({ entityAt(5) }), 0, 100000)).Spawns.size() == 1);
	}

	TEST_CASE("ReplicationSender stays within its budget and lets waiting entities catch up")
	{
		const uint32_t transform = ComponentIndex("Transform");
		const auto entity = [&](UUID id, float priority, float x) {
			ReplicatedEntity replicated;
			replicated.Entity = id;
			replicated.Priority = priority;
			const nlohmann::json value = { x, 0.0, 0.0 };
			replicated.Components.push_back({ transform, { { Encode(value), value }, { Encode({ 0.0, 0.0, 0.0 }), { 0.0, 0.0, 0.0 } }, { Encode({ 1.0, 1.0, 1.0 }), { 1.0, 1.0, 1.0 } } } });
			return replicated;
		};
		const auto world = [](std::vector<ReplicatedEntity> entities) {
			return ReplicationWorld(std::move(entities), [](const ReplicatedEntity& replicated) {
				NetWriter writer;
				NetProtocol::WriteSpawn(writer, { replicated.Entity, 0, nlohmann::json::array({ { { "ID", static_cast<uint64_t>(replicated.Entity) }, { "Pad", std::string(200, 'x') } } }) });
				return std::optional<std::vector<uint8_t>>(writer.TakeBytes());
			});
		};
		ReplicationSender sender;
		// A budget for one spawn: the rest wait.
		std::vector<uint8_t> message = sender.Build(NetMessageType::Snapshot, world({ entity(1, 1, 0), entity(2, 1, 0), entity(3, 1, 0) }), 0, 300);
		CHECK(message.size() <= 300);
		CHECK(std::get<NetState>(DecodeMessage(message)->Body).Spawns.size() == 1);
		CHECK(sender.GetDeferredRecordCount() == 2);
		// Below the size of any record, the first still goes when the budget is positive; at 0 nothing does.
		CHECK(std::get<NetState>(DecodeMessage(sender.Build(NetMessageType::Snapshot, world({ entity(1, 1, 0), entity(2, 1, 0), entity(3, 1, 0) }), 0, 10))->Body).Spawns.size() == 1);
		CHECK(std::get<NetState>(DecodeMessage(sender.Build(NetMessageType::Snapshot, world({ entity(1, 1, 0), entity(2, 1, 0), entity(3, 1, 0) }), 0, 0))->Body).Spawns.empty());

		// Priorities: with room for one update per tick, the priority 3 entity goes twice for each time the priority 1 one
		// does (it waits until its total catches up), and the priority 1 one is never starved.
		ReplicationSender updates;
		updates.Build(NetMessageType::Snapshot, world({ entity(1, 1, 0), entity(2, 3, 0) }), 0, 100000);
		updates.Acknowledge(1);
		int sent[3] = {};
		for (int tick = 1; tick <= 40; tick++)
		{
			const auto state = std::get<NetState>(DecodeMessage(updates.Build(NetMessageType::Snapshot, world({ entity(1, 1, static_cast<float>(tick)), entity(2, 3, static_cast<float>(tick)) }), 0, 40))->Body);
			REQUIRE(state.Updates.size() == 1);
			sent[state.Updates[0].Entity]++;
			updates.Acknowledge(state.Sequence);
		}
		CHECK(sent[1] == 13);
		CHECK(sent[2] == 27);
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
			for (float t = 0; t < seconds; t += 0.01f)
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

		// A component removed on the host takes the entity off the clients.
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
		Scene& host = session.StartHost([&](Scene& scene) {
			crateId = CreateNetworked(scene, "Crate").GetUUID();
			Entity owned = CreateNetworked(scene, "Owned");
			owned.GetComponent<NetworkedComponent>() = { 1, NetworkAuthority::Owner };
			ownedId = owned.GetUUID();
		});
		RawPeer raw(session, session.Link().Connect(session.GetPort()));
		REQUIRE(raw.Join() == 1);
		const uint32_t transform = ComponentIndex("Transform");
		const uint32_t networked = ComponentIndex("Networked");
		NetState state;
		state.Sequence = 1;
		state.Updates.push_back({ crateId, std::nullopt, { { transform, false, false, { { 0, { 50.0, 0.0, 0.0 } } } } } });
		state.Updates.push_back({ ownedId, std::nullopt, { { networked, false, false, { { 0, 2 } } } } });
		raw.Send(NetProtocol::EncodeState(NetMessageType::ClientUpdate, state), NetChannel::Unreliable);
		session.Run(10);
		CHECK(host.GetEntityByUUID(crateId).GetComponent<TransformComponent>().Translation.x == 0.0f);
		CHECK(host.GetEntityByUUID(ownedId).GetComponent<NetworkedComponent>().Owner == 1);
		CHECK(session.HostNetwork().GetStatistics().RejectedRecords == 2);
		// A value the component refuses (a scale with a NaN) is refused here too; an older message is dropped.
		NetState invalid;
		invalid.Sequence = 3;
		invalid.Updates.push_back({ ownedId, std::nullopt, { { transform, false, false, { { 0, { std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0 } }, { 2, { 2.0, 2.0, 2.0 } } } } } });
		raw.Send(NetProtocol::EncodeState(NetMessageType::ClientUpdate, invalid), NetChannel::Unreliable);
		session.Run(10);
		CHECK(session.HostNetwork().GetStatistics().RejectedValues == 1);
		CHECK(host.GetEntityByUUID(ownedId).GetComponent<TransformComponent>().Scale == glm::vec3(1.0f));
		NetState stale;
		stale.Sequence = 2;
		stale.Updates.push_back({ ownedId, std::nullopt, { { transform, false, false, { { 0, { 8.0, 0.0, 0.0 } } } } } });
		raw.Send(NetProtocol::EncodeState(NetMessageType::ClientUpdate, stale), NetChannel::Unreliable);
		session.Run(10);
		CHECK(session.HostNetwork().GetStatistics().StaleMessages == 1);
		CHECK(host.GetEntityByUUID(ownedId).GetComponent<TransformComponent>().Translation.x == 0.0f);
		// Anything that does not decode ends the connection.
		raw.Send(Bytes({ 0xEE, 1, 2 }));
		const auto kicked = raw.WaitFor(NetTransportEvent::Kind::Disconnected);
		REQUIRE(kicked);
		CHECK(DisconnectReasonFromCode(kicked->DisconnectCode) == NetDisconnectReason::InvalidPacket);
		CHECK(session.HostNetwork().GetConnections().empty());
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

		Scene& first = session.AddClient();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(0).GetState() == NetConnectionState::Connected; }, 60));
		Scene& second = session.AddClient();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(1).GetState() == NetConnectionState::Disconnected; }, 60));
		CHECK(session.ClientNetwork(1).GetDisconnectReason() == NetDisconnectReason::ServerFull);
		CHECK(session.ClientNetwork(0).GetState() == NetConnectionState::Connected);
		(void)first;
		(void)second;

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
		Scene& orphan = session.AddClient();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(1).GetState() == NetConnectionState::Disconnected; }, 180));
		CHECK(session.ClientNetwork(1).GetDisconnectReason() == NetDisconnectReason::ConnectionFailed);
		(void)orphan;

		// Quitting and stopping are told to the other side.
		session.Link().SetLinkSettings({ .TimeoutSeconds = 1.0f });
		Scene& leaver = session.AddClient();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(2).GetState() == NetConnectionState::Connected; }, 60));
		leaver.StopNetwork();
		REQUIRE(session.RunUntil([&] { return session.HostNetwork().GetConnections().empty(); }, 60));
		Scene& stayer = session.AddClient();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(3).GetState() == NetConnectionState::Connected; }, 60));
		session.Host().StopNetwork();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(3).GetState() == NetConnectionState::Disconnected; }, 60));
		CHECK(session.ClientNetwork(3).GetDisconnectReason() == NetDisconnectReason::ServerShutdown);
		(void)stayer;
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
			Entity ground = scene.CreateEntity("Ground");
			ground.AddComponent<RigidBodyComponent>();
			ground.AddComponent<BoxColliderComponent>().HalfExtents = { 10.0f, 0.5f, 10.0f };
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
		Scene& client = session.AddClient([](Scene& scene) {
			Entity ground = scene.CreateEntity("Ground");
			ground.AddComponent<RigidBodyComponent>();
			ground.AddComponent<BoxColliderComponent>().HalfExtents = { 10.0f, 0.5f, 10.0f };
		});
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
		Session session;
		std::string error;
		Scope<NetTransport> listening = session.Link().Listen(0, 4, error);
		REQUIRE(listening);
		RawPeer fakeHost(session, std::move(listening));
		Ref<Scene> clientScene = CreateRef<Scene>("Client");
		clientScene->CreateEntityWithUUID(77, "Local");
		clientScene->StartNetwork(CreateScope<NetworkSystem>(*clientScene, NetworkRole::Client, session.Link().Connect(fakeHost.Transport().GetListenPort()), NetworkSettings{}, TestIdentity()));
		clientScene->OnRuntimeStart();
		const auto step = [&](int frames) {
			for (int i = 0; i < frames; i++)
			{
				clientScene->OnUpdate(Step);
				session.Link().Advance(Step);
			}
		};
		std::vector<NetTransportEvent> events;
		for (int i = 0; i < 30 && events.empty(); i++)
		{
			step(1);
			fakeHost.Transport().Poll(events);
		}
		REQUIRE_FALSE(events.empty());
		const NetPeerHandle client = events[0].Peer;
		fakeHost.Transport().Send(client, NetChannel::Reliable, NetProtocol::EncodeWelcome({ 1, 30.0f }));
		step(5);
		REQUIRE(clientScene->GetNetworkSystem()->GetState() == NetConnectionState::Connected);

		const auto record = [](uint64_t id, nlohmann::json components) { return nlohmann::json{ { "ID", id }, { "Name", "Spawned" }, { "Parent", 0 }, { "Components", std::move(components) } }; };
		NetState state;
		state.Sequence = 1;
		state.Spawns.push_back({ 10, 0, nlohmann::json::array({ record(10, { { "Mesh", { { "Mesh", "../../secret.gltf" } } } }) }) });
		state.Spawns.push_back({ 11, 0, nlohmann::json::array({ record(11, { { "Mesh", { { "Mesh", "/etc/passwd" } } } }) }) });
		state.Spawns.push_back({ 12, 0, nlohmann::json::array({ record(12, { { "Transform", { { "Scale", "big" } } } }) }) });
		state.Spawns.push_back({ 77, 0, nlohmann::json::array({ record(77, nlohmann::json::object()) }) });
		state.Spawns.push_back({ 13, 0, nlohmann::json::array({ record(14, nlohmann::json::object()) }) });
		state.Spawns.push_back({ 15, 0, nlohmann::json::array({ record(15, { { "Mesh", { { "Mesh", "Assets/Models/Ok.gltf" } } } }) }) });
		fakeHost.Transport().Send(client, NetChannel::Unreliable, NetProtocol::EncodeState(NetMessageType::Snapshot, state));
		step(5);
		for (uint64_t id : { 10, 11, 12, 13, 14 })
			CHECK_FALSE(clientScene->GetEntityByUUID(id));
		CHECK(clientScene->GetEntityByUUID(77).GetName() == "Local");
		CHECK(clientScene->GetEntityByUUID(15));
		CHECK(clientScene->GetNetworkSystem()->GetStatistics().RejectedRecords == 5);

		// A message that does not decode ends the session from the client's side.
		fakeHost.Transport().Send(client, NetChannel::Reliable, Bytes({ 3, 0 }));
		step(5);
		CHECK(clientScene->GetNetworkSystem()->GetState() == NetConnectionState::Disconnected);
		CHECK(clientScene->GetNetworkSystem()->GetDisconnectReason() == NetDisconnectReason::InvalidPacket);
		clientScene->OnRuntimeStop();
	}

	TEST_CASE("Network sessions: client IDs come from their own stream, and scene loads wait for a later session")
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
		for (const nlohmann::json& bad : { nlohmann::json{ { "Port", 0 } }, nlohmann::json{ { "Port", 70000 } }, nlohmann::json{ { "MaxClients", -1 } }, nlohmann::json{ { "TickRate", 0.5 } }, nlohmann::json{ { "TickRate", "fast" } }, nlohmann::json{ { "MaxBytesPerSecond", 10 } }, nlohmann::json{ { "TimeoutSeconds", 1e9 } }, nlohmann::json{ { "Unknown", 1 } }, nlohmann::json(3) })
		{
			CHECK_FALSE(NetworkSettings::FromJson(bad, error));
			CHECK_FALSE(error.empty());
		}

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
