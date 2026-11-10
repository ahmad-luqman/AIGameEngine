#include <doctest/doctest.h>

#include <Breccia/Automation/AutomationSession.h>
#include <Breccia/Automation/CommandRegistry.h>
#include <Breccia/Core/FileSystem.h>
#include <Breccia/Core/Log.h>
#include <Breccia/Math/Random.h>
#include <Breccia/Network/NetBuffer.h>
#include <Breccia/Network/NetProtocol.h>
#include <Breccia/Network/NetValue.h>
#include <Breccia/Network/NetworkSettings.h>
#include <Breccia/Network/NetworkSystem.h>
#include <Breccia/Network/ReplicationSchema.h>
#include <Breccia/Network/SimulatedNetwork.h>
#include <Breccia/Physics/PhysicsWorld.h>
#include <Breccia/Scene/Components.h>
#include <Breccia/Scene/Entity.h>
#include <Breccia/Scene/Scene.h>
#include <Breccia/Scene/SceneSerializer.h>
#include <Breccia/Scripting/ScriptEngine.h>

#include "AutomationTestUtils.h"
#include "NetworkTestUtils.h"
#include "TestUtils.h"

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace Breccia;
using namespace NetworkTestUtils;
using BrecciaTest::Contains;
using nlohmann::json;

namespace {

	NetworkSettings WithDelay(float delay, NetworkSettings settings = {})
	{
		settings.InterpolationDelay = delay;
		return settings;
	}

	// The events of one kind in a list.
	std::vector<NetworkEvent> OfKind(const std::vector<NetworkEvent>& events, NetworkEvent::Kind kind)
	{
		std::vector<NetworkEvent> result;
		for (const NetworkEvent& event : events)
			if (event.Type == kind)
				result.push_back(event);
		return result;
	}

	// A networked character controller on the ground, with Predicted authority for `owner`.
	Entity CreatePredictedCharacter(Scene& scene, uint32_t owner, const glm::vec3& position = { 0.0f, 1.0f, 0.0f })
	{
		Entity character = CreateNetworked(scene, "Player", 0, position);
		character.GetComponent<NetworkedComponent>() = { owner, NetworkAuthority::Predicted };
		character.AddComponent<CapsuleColliderComponent>();
		character.AddComponent<CharacterControllerComponent>();
		return character;
	}

	// Advances the link and receives on one network system by hand (no scene update takes its events), for up to `frames`
	// frames or until an event of `kind` arrived; returns the events of that kind.
	std::vector<NetworkEvent> PumpUntil(Session& session, NetworkSystem& network, NetworkEvent::Kind kind, int frames)
	{
		std::vector<NetworkEvent> found;
		for (int i = 0; i < frames && found.empty(); i++)
		{
			session.Link().Advance(Step);
			network.Receive(Step);
			found = OfKind(network.TakeEvents(), kind);
		}
		return found;
	}

	// Advances the link and receives on one network system by hand (no scene update takes its events) for `frames` frames;
	// returns every event it queued meanwhile.
	std::vector<NetworkEvent> PumpAll(Session& session, NetworkSystem& network, int frames)
	{
		std::vector<NetworkEvent> events;
		for (int i = 0; i < frames; i++)
		{
			session.Link().Advance(Step);
			network.Receive(Step);
			for (NetworkEvent& event : network.TakeEvents())
				events.push_back(std::move(event));
		}
		return events;
	}

	// A client update carrying `count` equal inputs for `entity`, numbered from `first`, with the update sequence number
	// `sequence` (0: `first`; a host ignores an update older than the newest it has).
	std::vector<uint8_t> InputUpdate(UUID entity, uint32_t first, uint32_t count, const glm::vec3& velocity = { 1.0f, 0.0f, 0.0f }, uint32_t sequence = 0)
	{
		NetState update;
		update.Sequence = sequence != 0 ? sequence : first;
		update.Extras.Inputs.push_back({ entity, first, std::vector<CharacterInput>(count, { velocity, glm::quat(1, 0, 0, 0) }) });
		return NetProtocol::EncodeState(NetMessageType::ClientUpdate, update);
	}

	std::vector<uint8_t> ScriptBytes(NetScriptKind kind, UUID entity, const std::string& name, nlohmann::json arguments = nlohmann::json::array())
	{
		return NetProtocol::EncodeScript({ kind, entity, name, std::move(arguments) });
	}
}

TEST_SUITE("Network")
{
	TEST_CASE("NetProtocol: script messages round-trip, and invalid ones are refused on both ends")
	{
		const NetScriptMessage message{ NetScriptKind::Rpc, UUID(42), "Fire", nlohmann::json::array({ 1, "two", nlohmann::json::array({ 3.5, true }), nullptr }) };
		const std::vector<uint8_t> bytes = NetProtocol::EncodeScript(message);
		REQUIRE_FALSE(bytes.empty());
		std::string error;
		const std::optional<NetMessage> decoded = DecodeMessage(bytes, &error);
		REQUIRE_MESSAGE(decoded, error);
		REQUIRE(decoded->Type == NetMessageType::Script);
		const NetScriptMessage& back = std::get<NetScriptMessage>(decoded->Body);
		CHECK(back.Kind == NetScriptKind::Rpc);
		CHECK(back.Entity == UUID(42));
		CHECK(back.Name == "Fire");
		CHECK(back.Arguments == message.Arguments);

		CHECK(NetProtocol::CheckScript({ NetScriptKind::Message, 0, "Chat", nlohmann::json::array() }).empty());
		CHECK_FALSE(NetProtocol::CheckScript({ NetScriptKind::Message, 0, "", nlohmann::json::array() }).empty());
		CHECK_FALSE(NetProtocol::CheckScript({ NetScriptKind::Message, 0, std::string(NetProtocol::MaxScriptNameLength + 1, 'a'), nlohmann::json::array() }).empty());
		CHECK_FALSE(NetProtocol::CheckScript({ NetScriptKind::Message, 0, "\xff", nlohmann::json::array() }).empty());
		CHECK_FALSE(NetProtocol::CheckScript({ NetScriptKind::Message, 0, "Chat", nlohmann::json::object() }).empty());
		CHECK_FALSE(NetProtocol::CheckScript({ NetScriptKind::Message, 0, "Chat", nlohmann::json(std::vector<int>(NetProtocol::MaxScriptArguments + 1, 0)) }).empty());
		CHECK_FALSE(NetProtocol::CheckScript({ NetScriptKind::Rpc, 0, "Fire", nlohmann::json::array() }).empty());
		CHECK_FALSE(NetProtocol::CheckScript({ NetScriptKind::Message, 7, "Chat", nlohmann::json::array() }).empty());

		// Too large to encode, and too large to decode.
		const std::string big(NetProtocol::MaxScriptMessageBytes, 'x');
		CHECK(NetProtocol::EncodeScript({ NetScriptKind::Message, 0, "Big", nlohmann::json::array({ big }) }).empty());
		std::vector<uint8_t> oversized = ScriptBytes(NetScriptKind::Message, 0, "Pad", nlohmann::json::array({ std::string(1000, 'y') }));
		oversized.resize(NetProtocol::MaxScriptMessageBytes + 1, 0);
		CHECK_FALSE(DecodeMessage(oversized));
		// A hand-made message the encoder would refuse: an RPC without an entity, a kind that does not exist.
		NetWriter writer;
		writer.WriteU8(static_cast<uint8_t>(NetMessageType::Script));
		writer.WriteU8(static_cast<uint8_t>(NetScriptKind::Rpc));
		writer.WriteU64(0);
		writer.WriteString("Fire");
		writer.WriteBytes(Encode(nlohmann::json::array()));
		CHECK_FALSE(DecodeMessage(writer.GetBytes()));
		std::vector<uint8_t> badKind = bytes;
		badKind[1] = 7;
		CHECK_FALSE(DecodeMessage(badKind));
	}

	TEST_CASE("FuzzNetPacket's seeds decode with this protocol version")
	{
		// The first byte of a seed picks what the fuzz target does with the rest, a message.
		const std::filesystem::path seeds = std::filesystem::path(BRECCIA_SOURCE_DIR) / "Tests/Fuzz/Corpus/FuzzNetPacket";
		size_t count = 0;
		for (const auto& entry : std::filesystem::directory_iterator(seeds))
		{
			const auto bytes = FileSystem::ReadBinaryFile(entry.path());
			REQUIRE(bytes);
			REQUIRE(bytes->size() > 1);
			std::string error;
			const std::optional<NetMessage> message = NetProtocol::Decode(std::span(*bytes).subspan(1), ReplicationSchema::Get(), error);
			// One seed is a client update the decoder must refuse (it changes a parent).
			const std::string name = entry.path().filename().string();
			CHECK_MESSAGE(message.has_value() == (name != "client-update-structure"), name, ": ", error);
			count++;
		}
		CHECK(count >= 13);
	}

	TEST_CASE("NetProtocol: snapshot time and predicted states, client inputs, and their bounds")
	{
		NetState snapshot;
		snapshot.Sequence = 3;
		snapshot.Extras.Time = 12.5;
		snapshot.Extras.Predictions.push_back({ UUID(9), 77, { { 1.0f, 2.0f, 3.0f }, { 0.0f, -4.0f, 0.5f }, 0.25f, true } });
		std::string error;
		std::optional<NetMessage> decoded = DecodeMessage(NetProtocol::EncodeState(NetMessageType::Snapshot, snapshot), &error);
		REQUIRE_MESSAGE(decoded, error);
		const NetState back = std::get<NetState>(decoded->Body);
		CHECK(back.Extras.Time == 12.5);
		REQUIRE(back.Extras.Predictions.size() == 1);
		CHECK(back.Extras.Predictions[0].Entity == UUID(9));
		CHECK(back.Extras.Predictions[0].Input == 77);
		CHECK(back.Extras.Predictions[0].State == snapshot.Extras.Predictions[0].State);

		NetState update;
		update.Sequence = 1;
		update.Extras.Inputs.push_back({ UUID(9), 5, { { { 1.0f, 0.0f, 0.0f }, glm::quat(1, 0, 0, 0) }, { { 0.0f, 3.0f, 0.0f }, glm::angleAxis(0.5f, glm::vec3(0, 1, 0)) } } });
		decoded = DecodeMessage(NetProtocol::EncodeState(NetMessageType::ClientUpdate, update), &error);
		REQUIRE_MESSAGE(decoded, error);
		const NetState& inputs = std::get<NetState>(decoded->Body);
		REQUIRE(inputs.Extras.Inputs.size() == 1);
		CHECK(inputs.Extras.Inputs[0].First == 5);
		CHECK(inputs.Extras.Inputs[0].Steps == update.Extras.Inputs[0].Steps);
		// Each direction carries only its own extras.
		CHECK(inputs.Extras.Predictions.empty());
		CHECK(back.Extras.Inputs.empty());

		const auto refused = [](NetMessageType type, const NetState& state, const char* expected) {
			std::string reason;
			CHECK_FALSE(DecodeMessage(NetProtocol::EncodeState(type, state), &reason));
			CHECK_MESSAGE(reason.find(expected) != std::string::npos, reason);
		};
		NetState bad = snapshot;
		bad.Extras.Time = -1.0;
		refused(NetMessageType::Snapshot, bad, "time");
		bad.Extras.Time = std::numeric_limits<double>::infinity();
		refused(NetMessageType::Snapshot, bad, "time");
		bad = snapshot;
		bad.Extras.Predictions[0].State.Position.y = NAN;
		refused(NetMessageType::Snapshot, bad, "not finite");
		bad = snapshot;
		bad.Extras.Predictions[0].State.SinceGrounded = CharacterState::MaxSinceGrounded + 1.0f;
		refused(NetMessageType::Snapshot, bad, "out of range");
		bad = std::move(snapshot);
		bad.Extras.Predictions.push_back(bad.Extras.Predictions[0]);
		refused(NetMessageType::Snapshot, bad, "twice");

		NetState badInputs = update;
		badInputs.Extras.Inputs[0].Steps[1].Velocity = { NetProtocol::MaxInputSpeed + 1.0f, 0.0f, 0.0f };
		refused(NetMessageType::ClientUpdate, badInputs, "faster");
		badInputs = update;
		badInputs.Extras.Inputs[0].Steps[0].Rotation = glm::quat(0, 0, 0, 0);
		refused(NetMessageType::ClientUpdate, badInputs, "unit quaternion");
		badInputs = update;
		badInputs.Extras.Inputs[0].Steps[0].Velocity.z = INFINITY;
		refused(NetMessageType::ClientUpdate, badInputs, "not finite");
		badInputs = update;
		badInputs.Extras.Inputs[0].First = 0;
		refused(NetMessageType::ClientUpdate, badInputs, "numbered");
		badInputs = update;
		badInputs.Extras.Inputs[0].First = std::numeric_limits<uint32_t>::max();
		refused(NetMessageType::ClientUpdate, badInputs, "last sequence");
		badInputs = update;
		badInputs.Extras.Inputs[0].Steps.clear();
		refused(NetMessageType::ClientUpdate, badInputs, "empty");
		badInputs = update;
		badInputs.Extras.Inputs[0].Steps.resize(NetProtocol::MaxInputsPerCharacter + 1);
		refused(NetMessageType::ClientUpdate, badInputs, "too many");
		badInputs = std::move(update);
		badInputs.Extras.Inputs.push_back(badInputs.Extras.Inputs[0]);
		refused(NetMessageType::ClientUpdate, badInputs, "twice");
	}

	TEST_CASE("Network settings: interpolation delay and message rate are validated and saved")
	{
		NetworkSettings settings;
		CHECK(settings.Validate().empty());
		std::string error;
		const auto read = NetworkSettings::FromJson({ { "InterpolationDelay", 0.25 }, { "MaxMessagesPerSecond", 30 } }, error);
		REQUIRE_MESSAGE(read, error);
		CHECK(read->InterpolationDelay == 0.25f);
		CHECK(read->MaxMessagesPerSecond == 30);
		CHECK(NetworkSettings::FromJson(read->ToJson(), error) == read);
		CHECK_FALSE(NetworkSettings::FromJson({ { "InterpolationDelay", -0.1 } }, error));
		CHECK(error.find("InterpolationDelay") != std::string::npos);
		CHECK_FALSE(NetworkSettings::FromJson({ { "InterpolationDelay", NetworkSettings::MaxInterpolationDelay + 0.5 } }, error));
		CHECK_FALSE(NetworkSettings::FromJson({ { "MaxMessagesPerSecond", 0 } }, error));
		CHECK(error.find("MaxMessagesPerSecond") != std::string::npos);
		CHECK_FALSE(NetworkSettings::FromJson({ { "MaxMessagesPerSecond", NetworkSettings::MaxMessagesPerSecondLimit + 1 } }, error));
		CHECK_FALSE(NetworkSettings::FromJson({ { "MaxMessagesPerSecond", "fast" } }, error));
	}

	TEST_CASE("Connection events reach the host and the client, with reasons")
	{
		Session session;
		session.StartHost();
		session.AddClient();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(0).GetState() == NetConnectionState::Connected; }, 60));
		// Scene::OnUpdate hands them to scripts every frame: run the systems by hand to see them.
		NetworkSystem& host = session.HostNetwork();
		NetworkSystem& network = session.ClientNetwork(0);
		CHECK(host.GetPeers() == std::vector<uint32_t>{ 1 });
		CHECK(network.GetPeers() == std::vector<uint32_t>{ 0 });
		// Both were taken by the scenes' updates already; a new client shows the host's side.
		Scene& second = session.AddClient();
		std::vector<NetworkEvent> hostEvents;
		std::vector<NetworkEvent> clientEvents;
		for (int i = 0; i < 60 && session.ClientNetwork(1).GetState() != NetConnectionState::Connected; i++)
		{
			session.Link().Advance(Step);
			host.Receive(Step);
			session.ClientNetwork(1).Receive(Step);
			for (NetworkEvent& event : host.TakeEvents())
				hostEvents.push_back(std::move(event));
			for (NetworkEvent& event : session.ClientNetwork(1).TakeEvents())
				clientEvents.push_back(std::move(event));
			host.Send(Step);
			session.ClientNetwork(1).Send(Step);
		}
		REQUIRE(OfKind(hostEvents, NetworkEvent::Kind::PeerConnected).size() == 1);
		CHECK(OfKind(hostEvents, NetworkEvent::Kind::PeerConnected)[0].Peer == 2);
		REQUIRE(OfKind(clientEvents, NetworkEvent::Kind::Connected).size() == 1);
		CHECK(OfKind(clientEvents, NetworkEvent::Kind::Connected)[0].Peer == 2);

		// The second client leaves: the host hears it with the reason.
		second.StopNetwork();
		hostEvents = PumpUntil(session, host, NetworkEvent::Kind::PeerDisconnected, 30);
		REQUIRE(hostEvents.size() == 1);
		CHECK(hostEvents[0].Peer == 2);
		CHECK(hostEvents[0].Reason == NetDisconnectReason::ClientQuit);

		// The host stops: the client hears ServerShutdown.
		session.Host().StopNetwork();
		clientEvents = PumpUntil(session, network, NetworkEvent::Kind::Disconnected, 30);
		REQUIRE(clientEvents.size() == 1);
		CHECK(clientEvents[0].Reason == NetDisconnectReason::ServerShutdown);
		CHECK(network.GetPeers().empty());
	}

	TEST_CASE("Script messages travel both ways, with the sender stamped by the host and limits on what goes")
	{
		NetworkSettings settings;
		settings.MaxMessagesPerSecond = 10;
		Session session({}, settings);
		session.StartHost();
		session.AddClient();
		session.AddClient();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(1).GetState() == NetConnectionState::Connected; }, 60));
		NetworkSystem& host = session.HostNetwork();
		NetworkSystem& client = session.ClientNetwork(1);
		host.TakeEvents();
		client.TakeEvents();

		std::string error;
		REQUIRE_MESSAGE(client.SendScript(0, { NetScriptKind::Message, 0, "Chat", { "hello", 3 } }, true, error), error);
		std::vector<NetworkEvent> received = PumpUntil(session, host, NetworkEvent::Kind::Message, 10);
		REQUIRE(received.size() == 1);
		CHECK(received[0].Peer == 2);
		CHECK(received[0].Name == "Chat");
		CHECK(received[0].Arguments == nlohmann::json::array({ "hello", 3 }));

		REQUIRE_MESSAGE(host.SendScript(2, { NetScriptKind::Message, 0, "Welcome", { true } }, false, error), error);
		received = PumpUntil(session, client, NetworkEvent::Kind::Message, 10);
		REQUIRE(received.size() == 1);
		CHECK(received[0].Peer == 0);
		CHECK(received[0].Name == "Welcome");
		CHECK(host.GetStatistics().ScriptMessagesSent == 1);
		CHECK(client.GetStatistics().ScriptMessagesReceived == 1);

		// Who may send to whom.
		CHECK_FALSE(client.SendScript(1, { NetScriptKind::Message, 0, "Chat", nlohmann::json::array() }, true, error));
		CHECK(error.find("only to the host") != std::string::npos);
		CHECK_FALSE(host.SendScript(0, { NetScriptKind::Message, 0, "Chat", nlohmann::json::array() }, true, error));
		CHECK_FALSE(host.SendScript(9, { NetScriptKind::Message, 0, "Chat", nlohmann::json::array() }, true, error));
		CHECK(error.find("no client 9") != std::string::npos);
		CHECK_FALSE(client.SendScript(0, { NetScriptKind::Message, 0, "", nlohmann::json::array() }, true, error));
		CHECK_FALSE(client.SendScript(0, { NetScriptKind::Message, 0, "Big", { std::string(NetProtocol::MaxScriptMessageBytes, 'x') } }, true, error));
		CHECK(error.find("larger") != std::string::npos);

		// MaxMessagesPerSecond: a sender refuses more, and gets them back as time passes.
		int sent = 0;
		while (client.SendScript(0, { NetScriptKind::Message, 0, "Spam", nlohmann::json::array() }, true, error))
			sent++;
		CHECK(sent == 9);
		CHECK(error.find("MaxMessagesPerSecond") != std::string::npos);
		session.Run(30);
		CHECK(client.SendScript(0, { NetScriptKind::Message, 0, "Spam", nlohmann::json::array() }, true, error));
	}

	TEST_CASE("A host hands a client's script messages to scripts at its rate, and disconnects a flood")
	{
		NetworkSettings settings;
		settings.MaxMessagesPerSecond = 60;
		Session session({}, settings);
		session.StartHost();
		RawPeer raw(session, session.Link().Connect(session.GetPort()));
		REQUIRE(raw.Join() == 1);
		NetworkSystem& host = session.HostNetwork();
		host.TakeEvents();
		// 120 at once: the first second's worth now, the rest over the next second (one per frame at 60 per second).
		for (int i = 0; i < 120; i++)
			raw.Send(ScriptBytes(NetScriptKind::Message, 0, "Burst", { i }));
		session.Link().Advance(Step);
		host.Receive(Step);
		std::vector<NetworkEvent> first = OfKind(host.TakeEvents(), NetworkEvent::Kind::Message);
		CHECK(first.size() == 60);
		REQUIRE_FALSE(first.empty());
		CHECK(first.front().Arguments[0] == 0);
		size_t total = first.size();
		for (int i = 0; i < 70; i++)
		{
			session.Link().Advance(Step);
			host.Receive(Step);
			total += OfKind(host.TakeEvents(), NetworkEvent::Kind::Message).size();
		}
		CHECK(total == 120);

		// More than MaxInboxMessages waiting: the connection ends.
		for (size_t i = 0; i < NetworkSystem::MaxInboxMessages + 70; i++)
			raw.Send(ScriptBytes(NetScriptKind::Message, 0, "Flood", nlohmann::json::array()));
		const auto ended = raw.WaitFor(NetTransportEvent::Kind::Disconnected, 30);
		REQUIRE(ended);
		CHECK(DisconnectReasonFromCode(ended->DisconnectCode) == NetDisconnectReason::InvalidPacket);
		CHECK(host.GetPeers().empty());

		// Far fewer, but large ones past MaxInboxBytes: that ends it too.
		RawPeer heavy(session, session.Link().Connect(session.GetPort()));
		REQUIRE(heavy.Join() == 2);
		const std::string payload(size_t{ 30 } * 1024, 'h');
		const size_t count = NetworkSystem::MaxInboxBytes / payload.size() + 70;
		REQUIRE(count < NetworkSystem::MaxInboxMessages / 2);
		for (size_t i = 0; i < count; i++)
			heavy.Send(ScriptBytes(NetScriptKind::Message, 0, "Heavy", { payload }));
		const auto heavyEnded = heavy.WaitFor(NetTransportEvent::Kind::Disconnected, 30);
		REQUIRE(heavyEnded);
		CHECK(DisconnectReasonFromCode(heavyEnded->DisconnectCode) == NetDisconnectReason::InvalidPacket);
	}

	TEST_CASE("An RPC that arrives before its entity waits for the spawn, and one that never arrives is dropped")
	{
		Session session({ .Latency = 0.02f });
		Scene& host = session.StartHost();
		session.AddClient();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(0).GetState() == NetConnectionState::Connected; }, 60));
		NetworkSystem& client = session.ClientNetwork(0);
		// The spawn goes with the next snapshot; the RPC, reliable, now. Lose the snapshots for a while.
		Entity crate = CreateNetworked(host, "Crate");
		std::string error;
		const SimulatedLinkSettings lossy{ .Latency = 0.02f, .Loss = 1.0f };
		REQUIRE(session.HostNetwork().SendScript(1, { NetScriptKind::Rpc, crate.GetUUID(), "Open", { 5 } }, true, error));
		session.Link().SetLinkSettings(lossy);
		session.Run(3);
		session.Link().SetLinkSettings({ .Latency = 0.02f });
		std::vector<NetworkEvent> rpcs;
		for (int i = 0; i < 120 && rpcs.empty(); i++)
		{
			session.Link().Advance(Step);
			session.HostNetwork().Send(Step);
			client.Receive(Step);
			rpcs = OfKind(client.TakeEvents(), NetworkEvent::Kind::Rpc);
			if (rpcs.empty())
				client.Send(Step);
		}
		REQUIRE(rpcs.size() == 1);
		CHECK(session.Client(0).GetEntityByUUID(crate.GetUUID()));
		CHECK(rpcs[0].Name == "Open");
		CHECK(rpcs[0].Entity == crate.GetUUID());

		// An RPC for an entity the host never spawns is dropped after PendingRpcSeconds.
		REQUIRE(session.HostNetwork().SendScript(1, { NetScriptKind::Rpc, UUID(123456789), "Ghost", nlohmann::json::array() }, true, error));
		const uint64_t dropped = client.GetStatistics().ScriptMessagesDropped;
		session.Run(static_cast<int>(NetworkSystem::PendingRpcSeconds / Step) + 30);
		CHECK(client.GetStatistics().ScriptMessagesDropped == dropped + 1);
	}

	TEST_CASE("A client shows remote entities between snapshots, behind the host by the interpolation delay")
	{
		NetworkSettings settings = WithDelay(0.1f);
		settings.TickRate = 20.0f;
		Session session({}, settings);
		UUID moverId = 0;
		Scene& host = session.StartHost([&](Scene& scene) { moverId = CreateNetworked(scene, "Mover").GetUUID(); });
		Scene& client = session.AddClient();
		REQUIRE(session.RunUntil([&] { return static_cast<bool>(client.GetEntityByUUID(moverId)); }, 60));
		session.Run(30);
		Entity hostMover = host.GetEntityByUUID(moverId);
		Entity clientMover = client.GetEntityByUUID(moverId);
		// At rest for a while, then moving at 3 m/s: the client starts from rest, not from the last change.
		std::vector<float> shown;
		for (int frame = 0; frame < 90; frame++)
		{
			hostMover.GetTransform().Translation.x += 3.0f * Step;
			session.Run(1);
			shown.push_back(clientMover.GetTransform().Translation.x);
		}
		// Smooth from its first frame (it starts from where it rested): every frame moves, never by much more than the host's
		// step (snapshots come every third frame).
		int changes = 0;
		for (size_t i = 1; i < shown.size(); i++)
		{
			const float delta = shown[i] - shown[i - 1];
			CHECK(delta >= -1e-5f);
			CHECK(delta <= 3.0f * Step * 1.6f);
			changes += delta > 1e-5f ? 1 : 0;
		}
		CHECK(changes >= 70);
		// Behind the host by about the delay (0.1 s at 3 m/s is 0.3 m), give or take a snapshot interval.
		const float lag = hostMover.GetTransform().Translation.x - clientMover.GetTransform().Translation.x;
		CHECK(lag > 0.2f);
		CHECK(lag < 0.6f);
		CHECK(session.ClientNetwork(0).GetViewTime() < session.ClientNetwork(0).GetTime());

		// Once the host stops, the client arrives exactly.
		session.Run(30);
		CHECK(clientMover.GetTransform().Translation.x == hostMover.GetTransform().Translation.x);

		// A local edit stays until the next update, which brings the host's values back.
		clientMover.GetTransform().Translation.y = 9.0f;
		for (int frame = 0; frame < 30; frame++)
		{
			session.Run(1);
			REQUIRE(clientMover.GetTransform().Translation.y == 9.0f);
		}
		hostMover.GetTransform().Translation.x += 1.0f;
		session.Run(31);
		CHECK(clientMover.GetTransform().Translation == hostMover.GetTransform().Translation);
		// Edited in the middle of a move, it keeps the edit until the host's next change (or a repeat of this one).
		hostMover.GetTransform().Translation.z = 5.0f;
		session.Run(4);
		clientMover.GetTransform().Translation.y = 7.0f;
		session.Run(30);
		hostMover.GetTransform().Translation.x += 1.0f;
		session.Run(30);
		CHECK(clientMover.GetTransform().Translation == hostMover.GetTransform().Translation);
	}

	TEST_CASE("Without an interpolation delay a client shows each snapshot as it arrives")
	{
		Session session({}, WithDelay(0.0f));
		UUID moverId = 0;
		Scene& host = session.StartHost([&](Scene& scene) { moverId = CreateNetworked(scene, "Mover").GetUUID(); });
		Scene& client = session.AddClient();
		REQUIRE(session.RunUntil([&] { return static_cast<bool>(client.GetEntityByUUID(moverId)); }, 60));
		host.GetEntityByUUID(moverId).GetTransform().Translation.x = 4.0f;
		REQUIRE(session.RunUntil([&] { return client.GetEntityByUUID(moverId).GetTransform().Translation.x != 0.0f; }, 10));
		CHECK(client.GetEntityByUUID(moverId).GetTransform().Translation.x == 4.0f);
	}

	TEST_CASE("A client predicts the character it owns and the host moves it by the client's inputs")
	{
		Session session({ .Latency = 0.05f }, WithDelay(0.1f));
		UUID playerId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			AddGround(scene);
			playerId = CreatePredictedCharacter(scene, 1).GetUUID();
		});
		Scene& client = session.AddClient([](Scene& scene) { AddGround(scene); });
		REQUIRE(session.RunUntil([&] { return client.GetEntityByUUID(playerId) && session.ClientNetwork(0).IsPredicted(client.GetEntityByUUID(playerId)); }, 120));
		session.Run(60);
		Entity hostPlayer = host.GetEntityByUUID(playerId);
		Entity clientPlayer = client.GetEntityByUUID(playerId);
		NetworkSystem& hostNetwork = session.HostNetwork();
		NetworkSystem& clientNetwork = session.ClientNetwork(0);
		CHECK(hostNetwork.HasAuthority(hostPlayer));
		CHECK_FALSE(hostNetwork.IsOwner(hostPlayer));
		CHECK_FALSE(clientNetwork.HasAuthority(clientPlayer));
		CHECK(clientNetwork.IsOwner(clientPlayer));
		CHECK(host.GetPhysicsWorld()->IsDrivenExternally(hostPlayer));
		CHECK_FALSE(client.GetPhysicsWorld()->IsDrivenExternally(clientPlayer));
		const float startX = clientPlayer.GetTransform().Translation.x;

		// The client walks at once, before any round trip; the host follows from the inputs.
		client.GetPhysicsWorld()->MoveCharacter(clientPlayer, { 2.0f, 0.0f, 0.0f });
		session.Run(1);
		CHECK(clientPlayer.GetTransform().Translation.x > startX + 0.02f);
		CHECK(hostPlayer.GetTransform().Translation.x == doctest::Approx(startX).epsilon(1e-4));
		// A host script's Move on the client's character changes nothing: only inputs move it.
		host.GetPhysicsWorld()->MoveCharacter(hostPlayer, { -5.0f, 0.0f, 0.0f });
		session.Run(60);
		CHECK(hostNetwork.GetStatistics().InputsApplied > 50);
		CHECK(hostPlayer.GetTransform().Translation.x > startX + 1.0f);
		// Ahead of the host by about the round trip.
		CHECK(clientPlayer.GetTransform().Translation.x > hostPlayer.GetTransform().Translation.x);
		client.GetPhysicsWorld()->MoveCharacter(clientPlayer, { 0.0f, 0.0f, 0.0f });
		session.Run(60);
		// At rest they agree, and a static world needed no visible correction.
		CHECK(clientPlayer.GetTransform().Translation.x == doctest::Approx(hostPlayer.GetTransform().Translation.x).epsilon(1e-3));
		CHECK(clientNetwork.GetStatistics().Corrections == 0);

		// The host moves the character itself (a respawn): the client is corrected to it.
		hostPlayer.GetTransform().Translation = { -6.0f, 1.0f, 3.0f };
		REQUIRE(host.GetPhysicsWorld()->SetCharacterState(hostPlayer, { { -6.0f, 1.0f, 3.0f }, glm::vec3(0.0f), 0.0f, false }));
		session.Run(30);
		CHECK(clientPlayer.GetTransform().Translation.x == doctest::Approx(-6.0f).epsilon(1e-2));
		CHECK(clientPlayer.GetTransform().Translation.z == doctest::Approx(3.0f).epsilon(1e-2));
		CHECK(clientNetwork.GetStatistics().Corrections >= 1);

		// Taken back by the host: the host simulates it again, and the client follows the host's transforms.
		Patch(hostPlayer, "Networked", { { "Authority", "Server" } });
		session.Run(30);
		CHECK_FALSE(host.GetPhysicsWorld()->IsDrivenExternally(hostPlayer));
		CHECK_FALSE(clientNetwork.IsPredicted(clientPlayer));
		CHECK(client.GetPhysicsWorld()->IsDrivenExternally(clientPlayer));
		host.GetPhysicsWorld()->MoveCharacter(hostPlayer, { 0.0f, 0.0f, 2.0f });
		session.Run(60);
		host.GetPhysicsWorld()->MoveCharacter(hostPlayer, { 0.0f, 0.0f, 0.0f });
		session.Run(30);
		CHECK(clientPlayer.GetTransform().Translation.z == doctest::Approx(hostPlayer.GetTransform().Translation.z).epsilon(1e-3));
	}

	TEST_CASE("A host applies a client's inputs no faster than its own steps, and only to characters it predicts")
	{
		Session session({}, WithDelay(0.0f));
		UUID playerId = 0;
		UUID otherId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			AddGround(scene);
			playerId = CreatePredictedCharacter(scene, 1).GetUUID();
			otherId = CreatePredictedCharacter(scene, 2, { 4.0f, 1.0f, 0.0f }).GetUUID();
		});
		RawPeer raw(session, session.Link().Connect(session.GetPort()));
		REQUIRE(raw.Join() == 1);
		session.Run(30);
		NetworkSystem& network = session.HostNetwork();
		const float startX = host.GetEntityByUUID(playerId).GetTransform().Translation.x;
		// A burst of inputs at once (a speed hack): at most half the input window of steps apply now.
		uint32_t next = 1;
		const auto sendInputs = [&](uint32_t count, UUID entity) {
			raw.Send(InputUpdate(entity, next, count, glm::vec3(10.0f, 0.0f, 0.0f)), NetChannel::Unreliable);
			next += count;
		};
		const uint64_t before = network.GetStatistics().InputsApplied;
		sendInputs(NetProtocol::MaxInputsPerCharacter, playerId);
		session.Run(2);
		const uint64_t applied = network.GetStatistics().InputsApplied - before;
		// The owner was silent before: the host kept half its window, and stepped the character meanwhile.
		const float window = std::floor(NetworkSystem::InputWindowSeconds / host.GetFixedTimestep());
		CHECK(applied >= 1);
		CHECK(static_cast<float>(applied) <= window / 2.0f + 3.0f);
		CHECK(network.GetStatistics().IdleSteps > 0);
		CHECK(host.GetEntityByUUID(playerId).GetTransform().Translation.x > startX);
		// Inputs for another client's character are refused, even while that client is connected.
		RawPeer second(session, session.Link().Connect(session.GetPort()));
		REQUIRE(second.Join() == 2);
		session.Run(2);
		const uint64_t rejected = network.GetStatistics().InputsRejected;
		const float playerX = host.GetEntityByUUID(playerId).GetTransform().Translation.x;
		second.Send(InputUpdate(playerId, 1000, 8, { 10.0f, 0.0f, 0.0f }), NetChannel::Unreliable);
		session.Run(2);
		// Every step of the batch.
		CHECK(network.GetStatistics().InputsRejected == rejected + 8);
		CHECK(host.GetEntityByUUID(playerId).GetTransform().Translation.x == doctest::Approx(playerX).epsilon(1e-3));
		// Handed to the second client, the character takes its inputs, numbered afresh.
		const float otherX = host.GetEntityByUUID(otherId).GetTransform().Translation.x;
		Patch(host.GetEntityByUUID(playerId), "Networked", { { "Owner", 2 } });
		session.Run(2);
		const uint64_t handed = network.GetStatistics().InputsApplied;
		second.Send(InputUpdate(playerId, 1, 4, { 10.0f, 0.0f, 0.0f }, 1001), NetChannel::Unreliable);
		session.Run(2);
		CHECK(network.GetStatistics().InputsApplied == handed + 4);
		CHECK(host.GetEntityByUUID(otherId).GetTransform().Translation.x == otherX);
		Patch(host.GetEntityByUUID(playerId), "Networked", { { "Owner", 1 } });
		session.Run(2);
		// Over a long run the inputs applied keep pace with the host's steps, not with what was sent.
		const uint64_t start = network.GetStatistics().InputsApplied;
		const uint64_t steps = host.GetPhysicsWorld()->GetStepCount();
		for (int i = 0; i < 60; i++)
		{
			sendInputs(NetProtocol::MaxInputsPerCharacter, playerId);
			session.Run(1);
		}
		const uint64_t pace = network.GetStatistics().InputsApplied - start;
		const uint64_t hostSteps = host.GetPhysicsWorld()->GetStepCount() - steps;
		// The hand-back above started its inputs afresh, with half the window ahead.
		CHECK(static_cast<float>(pace) <= static_cast<float>(hostSteps) + window / 2.0f + 2.0f);
		CHECK(pace + 2 >= hostSteps);
	}

	TEST_CASE("Remote characters on a client follow the host instead of simulating")
	{
		Session session({}, WithDelay(0.0f));
		UUID playerId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			AddGround(scene);
			playerId = CreatePredictedCharacter(scene, 0, { 0.0f, 5.0f, 0.0f }).GetUUID();
		});
		Scene& client = session.AddClient([](Scene& scene) { AddGround(scene); });
		REQUIRE(session.RunUntil([&] { return static_cast<bool>(client.GetEntityByUUID(playerId)); }, 60));
		Entity clientPlayer = client.GetEntityByUUID(playerId);
		// The host's own character (Owner 0): the client is not its owner, so it is driven.
		session.Run(5);
		CHECK(client.GetPhysicsWorld()->IsDrivenExternally(clientPlayer));
		CHECK_FALSE(host.GetPhysicsWorld()->IsDrivenExternally(host.GetEntityByUUID(playerId)));
		// Its own scripts' Move does nothing there: it follows the host's falls and walks.
		client.GetPhysicsWorld()->MoveCharacter(clientPlayer, { 0.0f, 0.0f, 10.0f });
		host.GetPhysicsWorld()->MoveCharacter(host.GetEntityByUUID(playerId), { 1.0f, 0.0f, 0.0f });
		session.Run(90);
		host.GetPhysicsWorld()->MoveCharacter(host.GetEntityByUUID(playerId), { 0.0f, 0.0f, 0.0f });
		session.Run(30);
		const glm::vec3 hostAt = host.GetEntityByUUID(playerId).GetTransform().Translation;
		CHECK(glm::length(clientPlayer.GetTransform().Translation - hostAt) < 1e-3f);
		CHECK(hostAt.y < 2.0f);
	}

	TEST_CASE("Lag compensation: a host raycasts against where networked bodies were when a client saw them")
	{
		Session session({}, WithDelay(0.1f));
		UUID targetId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			Entity target = CreateNetworked(scene, "Target", 0, { 0.0f, 1.0f, 0.0f });
			target.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
			target.AddComponent<BoxColliderComponent>().HalfExtents = { 0.5f, 0.5f, 0.5f };
			targetId = target.GetUUID();
		});
		session.AddClient();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(0).GetState() == NetConnectionState::Connected; }, 60));
		session.Run(30);
		NetworkSystem& network = session.HostNetwork();
		Entity target = host.GetEntityByUUID(targetId);
		// Moves along x at 6 m/s for half a second; remember a moment in the middle.
		double then = 0.0;
		float thenX = 0.0f;
		for (int frame = 0; frame < 30; frame++)
		{
			target.GetTransform().Translation.x += 6.0f * Step;
			session.Run(1);
			if (frame == 10)
			{
				then = network.GetTime();
				thenX = target.GetTransform().Translation.x;
			}
		}
		const float nowX = target.GetTransform().Translation.x;
		REQUIRE(nowX - thenX > 1.5f);
		const glm::vec3 down(0.0f, -1.0f, 0.0f);
		// Then it was at thenX: a ray there hits it, one where it is now does not.
		std::optional<RaycastHit> hit = network.RaycastAt(then, { thenX, 5.0f, 0.0f }, down, 10.0f);
		REQUIRE(hit);
		CHECK(hit->EntityID == targetId);
		CHECK_FALSE(network.RaycastAt(then, { nowX, 5.0f, 0.0f }, down, 10.0f));
		// Between two ticks it stands between their poses: the motion is linear, so exactly where it was then. Its edge is
		// half a meter from its center.
		const double between = then + Step / 2.0;
		const float betweenX = thenX + 6.0f * Step / 2.0f;
		// Probes half the half-step move inside and outside each edge: either tick's pose alone fails one of them.
		CHECK(network.RaycastAt(between, { betweenX - 0.475f, 5.0f, 0.0f }, down, 10.0f));
		CHECK_FALSE(network.RaycastAt(between, { betweenX - 0.525f, 5.0f, 0.0f }, down, 10.0f));
		CHECK(network.RaycastAt(between, { betweenX + 0.475f, 5.0f, 0.0f }, down, 10.0f));
		CHECK_FALSE(network.RaycastAt(between, { betweenX + 0.525f, 5.0f, 0.0f }, down, 10.0f));
		// The present, or a time in the future, is a plain raycast.
		hit = network.RaycastAt(network.GetTime() + 1.0, { nowX, 5.0f, 0.0f }, down, 10.0f);
		REQUIRE(hit);
		CHECK(hit->EntityID == targetId);
		// Older than MaxRewindSeconds is clamped there.
		session.Run(90);
		hit = network.RaycastAt(0.0, { nowX, 5.0f, 0.0f }, down, 10.0f);
		REQUIRE(hit);
		CHECK(hit->EntityID == targetId);
		// An entity that did not exist then is not hit.
		const double before = network.GetTime();
		session.Run(10);
		Entity late = CreateNetworked(host, "Late", 0, { 20.0f, 1.0f, 0.0f });
		late.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		late.AddComponent<BoxColliderComponent>();
		session.Run(10);
		CHECK(network.RaycastAt(network.GetTime(), { 20.0f, 5.0f, 0.0f }, down, 10.0f));
		CHECK_FALSE(network.RaycastAt(before, { 20.0f, 5.0f, 0.0f }, down, 10.0f));
		// A client has no history: a plain raycast, which hits the target where its copy is now.
		const Entity copy = session.Client(0).GetEntityByUUID(targetId);
		REQUIRE(copy);
		const glm::vec3 shown = copy.GetTransform().Translation;
		hit = session.ClientNetwork(0).RaycastAt(then, { shown.x, 5.0f, 0.0f }, down, 10.0f);
		REQUIRE(hit);
		CHECK(hit->EntityID == targetId);
	}

	TEST_CASE("Lua Network: messages, RPCs with their permissions, connection events and queries")
	{
		BrecciaTest::TempProject project("LuaNetwork");
		const std::string script = project.WriteFile("Assets/Scripts/Game.lua", R"(
			local Game = {}
			Game.Rpcs = { Hit = "Owner", Open = "Any", Secret = "Host" }
			function Game:OnCreate()
				if Network.IsHost() then
					Joined = {}
					Network.On("PeerConnected", function(peer) table.insert(Joined, peer) end)
					Network.On("PeerDisconnected", function(peer, reason) Left = peer .. ":" .. reason end)
					Network.On("Ping", function(sender, value, list)
						Pinged = sender .. ":" .. value .. ":" .. #list
						PongSent = Network.Send(sender, "Pong", value * 2, self.Entity)
					end)
				else
					Network.On("Pong", function(sender, value, entity) Ponged = sender .. ":" .. value .. ":" .. tostring(entity == self.Entity.ID) end)
				end
			end
			function Game:Hit(sender, amount) Hits = (Hits or 0) + amount; HitBy = sender end
			function Game:Open(sender) Opened = sender end
			function Game:Secret(sender, word) SecretWord = word end
			return Game
		)");
		Session session({}, WithDelay(0.0f));
		UUID gameId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			Entity game = CreateNetworked(scene, "Game");
			game.GetComponent<NetworkedComponent>().Owner = 1;
			game.AddComponent<ScriptComponent>().Script = script;
			gameId = game.GetUUID();
		});
		Scene& owner = session.AddClient();
		Scene& other = session.AddClient();
		REQUIRE(session.RunUntil([&] { return owner.GetScriptEngine()->HasInstance(owner.GetEntityByUUID(gameId)) && other.GetScriptEngine()->HasInstance(other.GetEntityByUUID(gameId)); }, 120));
		CHECK(BrecciaTest::Eval(host, "return #Joined") == "2");

		// Queries.
		CHECK(BrecciaTest::Eval(host, "return Network.GetRole()") == "Host");
		CHECK(BrecciaTest::Eval(owner, "return Network.GetRole()") == "Client");
		CHECK(BrecciaTest::Eval(owner, "return Network.GetState()") == "Connected");
		CHECK(BrecciaTest::Eval(owner, "return Network.GetPeerId()") == "1");
		CHECK(BrecciaTest::Eval(other, "return Network.GetPeerId()") == "2");
		CHECK(BrecciaTest::Eval(host, "return table.concat(Network.GetPeers(), ',')") == "1,2");
		CHECK(BrecciaTest::Eval(owner, "return Network.GetPeers()[1]") == "0");
		CHECK(BrecciaTest::Eval(host, "return Network.IsHost() and not Network.IsClient() and Network.IsActive()") == "true");
		CHECK(BrecciaTest::Eval(host, "return tostring(Network.GetDisconnectReason())") == "nil");
		const std::string game = "local g = Scene.FindEntityByName('Game') ";
		CHECK(BrecciaTest::Eval(owner, game + "return tostring(Network.IsOwner(g)) .. tostring(Network.HasAuthority(g)) .. tostring(Network.IsPredicted(g))") == "truefalsefalse");
		CHECK(BrecciaTest::Eval(other, game + "return tostring(Network.IsOwner(g))") == "false");
		CHECK(BrecciaTest::Eval(host, game + "return tostring(Network.IsOwner(g)) .. tostring(Network.HasAuthority(g))") == "falsetrue");
		CHECK(BrecciaTest::Eval(host, "return Network.GetTime() > 0 and Network.GetViewTime() == Network.GetTime()") == "true");

		// A message to the host and its reply; entities travel as their IDs.
		CHECK(BrecciaTest::Eval(owner, "return Network.Send('Server', 'Ping', 21, { 1, 2, 3 })") == "true");
		REQUIRE(session.RunUntil([&] { return BrecciaTest::Eval(owner, "return Ponged") != "nil"; }, 30));
		CHECK(BrecciaTest::Eval(host, "return Pinged") == "1:21:3");
		CHECK(BrecciaTest::Eval(host, "return PongSent") == "true");
		CHECK(BrecciaTest::Eval(owner, "return Ponged") == "0:42:true");
		CHECK(BrecciaTest::Eval(other, "return tostring(Ponged)") == "nil");

		// RPCs: the owner may call an Owner method, anyone an Any method, only the host a Host method.
		CHECK(BrecciaTest::Eval(owner, game + "return Network.Rpc(g, 'Hit', 'Server', 5)") == "true");
		CHECK(BrecciaTest::Eval(other, game + "return Network.Rpc(g, 'Hit', 'Server', 50)") == "true");
		CHECK(BrecciaTest::Eval(other, game + "return Network.RpcUnreliable(g, 'Open', 'Server')") == "true");
		CHECK(BrecciaTest::Eval(owner, game + "return Network.Rpc(g, 'Secret', 'Server', 'x')") == "true");
		CHECK(BrecciaTest::Eval(owner, game + "return Network.Rpc(g, 'OnCreate', 'Server')") == "true");
		session.Run(10);
		CHECK(BrecciaTest::Eval(host, "return Hits") == "5");
		CHECK(BrecciaTest::Eval(host, "return HitBy") == "1");
		CHECK(BrecciaTest::Eval(host, "return Opened") == "2");
		CHECK(BrecciaTest::Eval(host, "return tostring(SecretWord)") == "nil");
		CHECK(BrecciaTest::Eval(host, "return #Joined") == "2");
		// The host calls any declared method: on every client, or on the entity's owner.
		CHECK(BrecciaTest::Eval(host, game + "return Network.Rpc(g, 'Secret', 'Clients', 'all')") == "true");
		session.Run(5);
		CHECK(BrecciaTest::Eval(owner, "return SecretWord") == "all");
		CHECK(BrecciaTest::Eval(other, "return SecretWord") == "all");
		CHECK(BrecciaTest::Eval(host, game + "return Network.Rpc(g, 'Secret', 'Owner', 'mine')") == "true");
		CHECK(BrecciaTest::Eval(host, game + "return Network.Rpc(g, 'Secret', 2, 'two')") == "true");
		session.Run(5);
		CHECK(BrecciaTest::Eval(owner, "return SecretWord") == "mine");
		CHECK(BrecciaTest::Eval(other, "return SecretWord") == "two");
		CHECK(host.GetScriptEngine()->GetErrors().empty());
		CHECK(owner.GetScriptEngine()->GetErrors().empty());

		// What a script may not send, raised as errors, and what fails to go, returned.
		const std::pair<const char*, const char*> errors[] = {
			{ "Network.Send('Clients', 'X')", "only the host sends to clients" },
			{ "Network.Send('Server', 'Connected')", "built-in" },
			{ "Network.Send('Server', '')", "non-empty" },
			{ "Network.Send('Nowhere', 'X')", "unknown target" },
			{ "Network.Send({}, 'X')", "the target must be" },
			{ "Network.Send('Owner', 'X')", "RPCs only" },
			{ "Network.Send('Server', 'X', function() end)", "argument 1" },
			{ "Network.Rpc(5, 'Hit', 'Server')", "expected an Entity" },
			{ "Network.On('X', 5)", "expected a function" },
			{ "Network.Raycast(Vec3(0,0,0), Vec3(0,-1,0), 10, 'now')", "time" },
			{ "Network.Raycast(Vec3(0,0,0), Vec3(0,-1,0), 10, 0, { All = true })", "All" },
		};
		BrecciaTest::CheckLuaErrors(*owner.GetScriptEngine(), errors);
		const std::pair<const char*, const char*> hostErrors[] = {
			{ "Network.Send('Server', 'X')", "the host is the server" },
			{ "Network.Send(-1, 'X')", "the peer ID" },
		};
		BrecciaTest::CheckLuaErrors(*host.GetScriptEngine(), hostErrors);
		CHECK(BrecciaTest::Eval(host, "local ok, err = Network.Send(9, 'X') return tostring(ok) .. ' ' .. err") == "false client 9: no client 9 is connected");
		CHECK(BrecciaTest::Eval(owner, "local ok, err = Network.Send('Server', 'Big', string.rep('x', 40000)) return tostring(ok) .. ' ' .. err").find("false the message is larger") == 0);

		// A client leaves: the host hears it.
		other.StopNetwork();
		session.Run(5);
		CHECK(BrecciaTest::Eval(host, "return Left") == "2:ClientQuit");
		CHECK(BrecciaTest::Eval(other, "return tostring(Network.GetRole()) .. tostring(Network.IsActive())") == "nilfalse");
		CHECK(BrecciaTest::Eval(other, "local ok, err = Network.Send('Server', 'X') return err") == "no network session");
		// Without a session every entity is the peer's own.
		CHECK(BrecciaTest::Eval(other, game + "return tostring(Network.HasAuthority(g)) .. tostring(Network.IsOwner(g)) .. tostring(Network.IsPredicted(g))") == "truetruefalse");
	}

	TEST_CASE("Lua Network: scripts host, connect, disconnect and reconnect, changing sessions at the end of the frame")
	{
		BrecciaTest::TempProject project("LuaNetworkSessions");
		const std::string script = project.WriteFile("Assets/Scripts/Events.lua", R"(
			local Events = {}
			function Events:OnCreate()
				Log = ""
				Network.On("Connected", function(peer) Log = Log .. "C" .. peer end)
				Network.On("Disconnected", function(reason) Log = Log .. "D" .. reason end)
				Network.On("PeerConnected", function(peer) Log = Log .. "P" .. peer end)
				Network.On("PeerDisconnected", function(peer, reason) Log = Log .. "L" .. peer .. reason end)
			end
			return Events
		)");
		SimulatedNetwork network;
		const auto makePeer = [&](const char* name) {
			Ref<Scene> scene = CreateRef<Scene>(name);
			scene->CreateEntity("Events").AddComponent<ScriptComponent>().Script = script;
			scene->SetNetworkTransports(network.CreateFactory());
			return scene;
		};
		Ref<Scene> host = makePeer("Host");
		Ref<Scene> client = makePeer("Client");
		host->OnRuntimeStart();
		client->OnRuntimeStart();
		const auto run = [&](int frames) {
			for (int i = 0; i < frames; i++)
			{
				host->OnUpdate(Step);
				client->OnUpdate(Step);
				network.Advance(Step);
			}
		};
		CHECK(BrecciaTest::Eval(*host, "return Network.Host(0)") == "true");
		// The session starts at the end of the frame.
		CHECK(BrecciaTest::Eval(*host, "return Network.IsActive()") == "false");
		run(1);
		CHECK(BrecciaTest::Eval(*host, "return Network.IsHost()") == "true");
		const std::string port = BrecciaTest::Eval(*host, "return Network.GetPort()");
		CHECK(port != "nil");
		CHECK(BrecciaTest::Eval(*host, "local ok, err = Network.Host(0) return err").find("active") != std::string::npos);
		CHECK(BrecciaTest::Eval(*client, "return Network.Connect('localhost', " + port + ")") == "true");
		run(30);
		CHECK(BrecciaTest::Eval(*client, "return Network.GetState()") == "Connected");
		CHECK(BrecciaTest::Eval(*client, "return Log") == "C1");
		CHECK(BrecciaTest::Eval(*host, "return Log") == "P1");

		// Disconnect ends it at the end of the frame; Connect again joins as a new peer.
		// A load in the frame that ends the session is not refused for the session (this file is missing).
		CHECK(BrecciaTest::Eval(*client, "Network.Disconnect() local ok, err = pcall(Scene.Load, 'Assets/Scenes/Missing.bscene') return tostring(Network.IsActive()) .. ' ' .. tostring(string.find(err, 'network session') ~= nil)") == "false false");
		CHECK(client->GetNetworkSystem());
		run(5);
		CHECK_FALSE(client->GetNetworkSystem());
		CHECK(BrecciaTest::Eval(*host, "return Log") == "P1L1ClientQuit");
		CHECK(BrecciaTest::Eval(*client, "return Network.Connect('localhost', " + port + ")") == "true");
		run(30);
		CHECK(BrecciaTest::Eval(*client, "return Network.GetPeerId()") == "2");
		CHECK(BrecciaTest::Eval(*client, "return Log") == "C1C2");
		// The host stops: the client hears it, and may reconnect over its ended session.
		CHECK(BrecciaTest::Eval(*host, "Network.Disconnect() return true") == "true");
		run(10);
		CHECK(BrecciaTest::Eval(*client, "return Log") == "C1C2DServerShutdown");
		CHECK(BrecciaTest::Eval(*client, "return Network.GetDisconnectReason()") == "ServerShutdown");
		CHECK(BrecciaTest::Eval(*host, "return Network.Host(" + port + ")") == "true");
		run(1);
		CHECK(BrecciaTest::Eval(*client, "return Network.Connect('localhost', " + port + ")") == "true");
		run(30);
		CHECK(BrecciaTest::Eval(*client, "return Network.GetState()") == "Connected");
		// A port nobody listens on: the attempt fails, which the script hears.
		CHECK(BrecciaTest::Eval(*client, "Network.Disconnect() return true") == "true");
		run(1);
		CHECK(BrecciaTest::Eval(*client, "return Network.Connect('localhost', 9)") == "true");
		run(static_cast<int>(SimulatedLinkSettings{}.TimeoutSeconds / Step) + 30);
		CHECK(BrecciaTest::Eval(*client, "return Log:sub(-17)") == "DConnectionFailed");
		CHECK(host->GetScriptEngine()->GetErrors().empty());
		CHECK(client->GetScriptEngine()->GetErrors().empty());
		host->OnRuntimeStop();
		client->OnRuntimeStop();
	}

	TEST_CASE("net.* commands host, add in-process clients, step them with the host and compare their scenes")
	{
		using BrecciaTest::Run;
		using BrecciaTest::RunError;
		BrecciaTest::TempProject project("NetCommands");
		CommandRegistry registry;
		AutomationSession session;
		Run(registry, session, "project.set", { { "network", { { "InterpolationDelay", 0.05 } } } });
		project.WriteFile("Assets/Scripts/Count.lua", R"(
			local Count = {}
			function Count:OnCreate()
				Messages = 0
				HostSpace = 0
				StartedHolding = Input.IsKeyDown("Space")
				Network.On("Tick", function(sender) Messages = Messages + 1 end)
			end
			function Count:OnDestroy()
				if FailOnDestroy then error("destroy boom") end
			end
			function Count:OnUpdate(dt)
				if FailNow then error("boom") end
				if Network.IsClient() and Input.IsKeyDown("Space") then Network.Send("Server", "Tick") end
				if Network.IsHost() and Input.IsKeyDown("Space") then HostSpace = HostSpace + 1 end
			end
			return Count
		)");
		Run(registry, session, "entity.create", { { "name", "Crate" }, { "components", { { "Networked", json::object() }, { "Script", { { "Script", "Assets/Scripts/Count.lua" } } } } } });
		CHECK(Contains(RunError(registry, session, "net.host"), "not playing"));
		Run(registry, session, "play.start");
		CHECK(Contains(RunError(registry, session, "net.connect"), "not hosting"));
		const json hosted = Run(registry, session, "net.host");
		CHECK(hosted["transport"] == "simulated");
		CHECK(Contains(RunError(registry, session, "net.host"), "already"));
		CHECK(Run(registry, session, "net.connect")["client"] == 1);
		CHECK(Run(registry, session, "net.connect")["client"] == 2);
		Run(registry, session, "net.link", { { "latency", 0.03 }, { "loss", 0.1 } });
		Run(registry, session, "play.step", { { "frames", 90 } });

		json status = Run(registry, session, "net.status");
		CHECK(status["role"] == "Host");
		CHECK(status["connections"].size() == 2);
		CHECK(status["clients"].size() == 2);
		CHECK(status["clients"][0]["state"] == "Connected");
		CHECK(status["link"]["latency"].get<double>() == doctest::Approx(0.03));
		CHECK(status["inputPeer"] == 0);
		const json clientStatus = Run(registry, session, "net.status", { { "client", 1 } });
		CHECK(clientStatus["role"] == "Client");
		CHECK(clientStatus["peerId"] == 1);
		CHECK(clientStatus["remoteEntities"] == 1);

		// The host moves the crate; once it settles every peer hashes the same.
		Run(registry, session, "component.set", { { "entity", "Crate" }, { "component", "Transform" }, { "data", { { "Translation", { 3.0, 0.0, -2.0 } } } } });
		Run(registry, session, "play.step", { { "frames", 60 } });
		const std::string hash = Run(registry, session, "scene.hash")["hash"];
		CHECK(Run(registry, session, "scene.hash", { { "client", 1 } })["hash"] == hash);
		CHECK(Run(registry, session, "scene.hash", { { "client", 2 }, { "expectHash", hash } })["hash"] == hash);
		// net.command reads (or changes) a client's scene.
		const json crate = Run(registry, session, "net.command", { { "client", 2 }, { "command", "entity.transform" }, { "params", { { "entity", "Crate" } } } });
		CHECK(crate["local"]["Translation"][0].get<double>() == doctest::Approx(3.0));
		CHECK(Contains(RunError(registry, session, "net.command", { { "client", 1 }, { "command", "play.step" } }), "cannot run on a client"));
		CHECK(Contains(RunError(registry, session, "net.command", { { "client", 9 }, { "command", "scene.hash" } }), "no in-process client 9"));

		// Input goes to the peer net.focus names: client 1 presses Space, and only its messages reach the host.
		Run(registry, session, "net.focus", { { "client", 1 } });
		Run(registry, session, "input.key", { { "key", "Space" }, { "down", true } });
		Run(registry, session, "play.step", { { "frames", 3 } });
		// A client that starts while a key is held starts without it.
		const json third = Run(registry, session, "net.connect");
		CHECK(Run(registry, session, "net.command", { { "client", third["client"] }, { "command", "lua.exec" }, { "params", { { "code", "tostring(StartedHolding)" } } } })["result"] == "false");
		Run(registry, session, "input.key", { { "key", "Space" }, { "down", false } });
		Run(registry, session, "play.step", { { "frames", 30 } });
		CHECK(Run(registry, session, "lua.exec", { { "code", "Messages" } })["result"] == "3");
		// The play scene heard none of it.
		CHECK(Run(registry, session, "lua.exec", { { "code", "HostSpace" } })["result"] == "0");
		CHECK(Contains(RunError(registry, session, "net.focus", { { "client", 7 } }), "no in-process client"));

		// A client's script error fails play.step's assertNoErrors, and names the client.
		Run(registry, session, "net.command", { { "client", 2 }, { "command", "lua.exec" }, { "params", { { "code", "FailNow = true" } } } });
		CHECK(Contains(RunError(registry, session, "play.step", { { "assertNoErrors", true } }), "in-process clients"));
		CHECK(Run(registry, session, "play.step")["clientScriptErrors"][0]["client"] == 2);
		// The focused client quitting gives the input back to the play scene.
		Run(registry, session, "net.focus", { { "client", third["client"] } });
		// Its script fails as it is destroyed: play.step still reports that, marked as an ended client's, and fails its
		// assertNoErrors.
		Run(registry, session, "net.command", { { "client", third["client"] }, { "command", "lua.exec" }, { "params", { { "code", "FailOnDestroy = true Game.Quit()" } } } });
		Run(registry, session, "net.command", { { "client", 2 }, { "command", "lua.exec" }, { "params", { { "code", "FailNow = false" } } } });
		const std::string quitError = RunError(registry, session, "play.step", { { "assertNoErrors", true } });
		CHECK(Contains(quitError, "destroy boom"));
		CHECK(Contains(quitError, "\"ended\":true"));
		CHECK(Run(registry, session, "net.status")["inputPeer"] == 0);
		CHECK_FALSE(session.GetNetworkClient(third["client"].get<size_t>()));
		Run(registry, session, "net.focus", { { "client", 1 } });

		// Ending a client tells the host; ending the play scene's session tells the rest.
		Run(registry, session, "net.disconnect", { { "client", 1 } });
		CHECK(Run(registry, session, "net.status")["inputPeer"] == 0);
		// Its slot stays, but takes no input.
		CHECK(Contains(RunError(registry, session, "net.focus", { { "client", 1 } }), "no in-process client 1"));
		Run(registry, session, "play.step", { { "frames", 10 } });
		CHECK(Run(registry, session, "net.status")["connections"].size() == 1);
		CHECK(Contains(RunError(registry, session, "net.disconnect", { { "client", 1 } }), "no in-process client 1"));
		Run(registry, session, "net.disconnect");
		Run(registry, session, "play.step", { { "frames", 10 } });
		CHECK(Run(registry, session, "net.status", { { "client", 2 } })["disconnectReason"] == "ServerShutdown");
		CHECK(Run(registry, session, "net.status")["role"] == nullptr);
		CHECK(Contains(RunError(registry, session, "net.disconnect"), "no network session"));
		// A session a script asked for, which starts at the end of the frame, refuses another one meanwhile.
		CHECK(Run(registry, session, "lua.exec", { { "code", "tostring(Network.Host(0))" } })["result"] == "true");
		CHECK(Contains(RunError(registry, session, "net.host"), "has not happened yet"));
		Run(registry, session, "play.step");
		CHECK(Run(registry, session, "net.status")["role"] == "Host");
		// Play's end takes the clients and the in-process network with it.
		Run(registry, session, "play.stop");
		CHECK(session.GetNetworkClients().empty());
		CHECK_FALSE(session.HasSimulatedNetwork());
	}

	TEST_CASE("Peers cannot fake connection events, and a predicted state out of range is refused")
	{
		CHECK_FALSE(NetProtocol::CheckScript({ NetScriptKind::Message, 0, "PeerConnected", nlohmann::json::array() }).empty());
		CHECK_FALSE(NetProtocol::CheckScript({ NetScriptKind::Message, 0, "Disconnected", nlohmann::json::array() }).empty());
		// An RPC calls a method, which may have any name.
		CHECK(NetProtocol::CheckScript({ NetScriptKind::Rpc, 5, "Connected", nlohmann::json::array() }).empty());
		NetState snapshot;
		snapshot.Sequence = 1;
		snapshot.Extras.Predictions.push_back({ UUID(9), 1, { { 0.0f, NetProtocol::MaxPredictedCoordinate * 2.0f, 0.0f }, glm::vec3(0.0f), 0.0f, false } });
		std::string error;
		CHECK_FALSE(DecodeMessage(NetProtocol::EncodeState(NetMessageType::Snapshot, snapshot), &error));
		CHECK(error.find("out of range") != std::string::npos);

		// A client that sends one is disconnected, and the host's scripts hear only the real events.
		Session session;
		session.StartHost();
		RawPeer raw(session, session.Link().Connect(session.GetPort()));
		REQUIRE(raw.Join() == 1);
		session.Run(2);
		NetWriter writer;
		writer.WriteU8(static_cast<uint8_t>(NetMessageType::Script));
		writer.WriteU8(static_cast<uint8_t>(NetScriptKind::Message));
		writer.WriteU64(0);
		writer.WriteString("PeerDisconnected");
		writer.WriteBytes(Encode(nlohmann::json::array({ "Timeout" })));
		raw.Send(writer.GetBytes());
		std::vector<NetworkEvent> events = PumpAll(session, session.HostNetwork(), 10);
		CHECK(OfKind(events, NetworkEvent::Kind::Message).empty());
		REQUIRE(OfKind(events, NetworkEvent::Kind::PeerDisconnected).size() == 1);
		CHECK(OfKind(events, NetworkEvent::Kind::PeerDisconnected)[0].Reason == NetDisconnectReason::InvalidPacket);
	}

	TEST_CASE("A client's messages keep their order behind an RPC waiting for its entity, and its last ones reach the host")
	{
		Session session({ .Latency = 0.02f });
		Scene& host = session.StartHost();
		session.AddClient();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(0).GetState() == NetConnectionState::Connected; }, 60));
		NetworkSystem& client = session.ClientNetwork(0);
		// The RPC's entity spawns with a later snapshot; the message after it must still come after it.
		Entity crate = CreateNetworked(host, "Crate");
		std::string error;
		REQUIRE(session.HostNetwork().SendScript(1, { NetScriptKind::Rpc, crate.GetUUID(), "Open", nlohmann::json::array() }, true, error));
		REQUIRE(session.HostNetwork().SendScript(1, { NetScriptKind::Message, 0, "After", nlohmann::json::array() }, true, error));
		std::vector<NetworkEvent> events;
		for (int i = 0; i < 60 && events.size() < 2; i++)
		{
			session.Link().Advance(Step);
			client.Receive(Step);
			for (NetworkEvent& event : client.TakeEvents())
				if (event.Type == NetworkEvent::Kind::Rpc || event.Type == NetworkEvent::Kind::Message)
					events.push_back(std::move(event));
			// The snapshot that spawns the crate leaves only after the messages have arrived.
			if (i >= 3)
				session.HostNetwork().Send(Step);
			client.Send(Step);
			session.HostNetwork().Receive(Step);
		}
		REQUIRE(events.size() == 2);
		CHECK(events[0].Name == "Open");
		CHECK(events[1].Name == "After");

		// An RPC for an entity the host does not have is dropped and counted.
		const uint64_t dropped = session.HostNetwork().GetStatistics().ScriptMessagesDropped;
		REQUIRE(client.SendScript(0, { NetScriptKind::Rpc, UUID(987654321), "Ghost", nlohmann::json::array() }, true, error));
		session.Run(5);
		CHECK(session.HostNetwork().GetStatistics().ScriptMessagesDropped == dropped + 1);
	}

	TEST_CASE("A client that leaves gets its waiting messages handed over first; one disconnected for flooding does not")
	{
		NetworkSettings settings;
		settings.MaxMessagesPerSecond = 10;
		Session session({}, settings);
		session.StartHost();
		RawPeer raw(session, session.Link().Connect(session.GetPort()));
		REQUIRE(raw.Join() == 1);
		NetworkSystem& host = session.HostNetwork();
		host.TakeEvents();
		// Twice what the host hands over in a second, then a goodbye: they all arrive, then the departure.
		for (int i = 0; i < 20; i++)
			raw.Send(ScriptBytes(NetScriptKind::Message, 0, "Bye", { i }));
		raw.Transport().Disconnect(1, static_cast<uint32_t>(NetDisconnectReason::ClientQuit));
		std::vector<NetworkEvent> events = PumpAll(session, host, 10);
		REQUIRE(events.size() == 21);
		CHECK(events.back().Type == NetworkEvent::Kind::PeerDisconnected);
		CHECK(events[19].Arguments[0] == 19);
		CHECK(host.GetStatistics().ScriptMessagesDropped == 0);

		// One that does not decode: what came before it is handed over, it and what came after are dropped and counted.
		RawPeer broken(session, session.Link().Connect(session.GetPort()));
		REQUIRE(broken.Join() == 2);
		host.TakeEvents();
		for (int i = 0; i < 3; i++)
			broken.Send(ScriptBytes(NetScriptKind::Message, 0, "Before", { i }));
		broken.Send(Bytes({ static_cast<int>(NetMessageType::Script), 7 }));
		for (int i = 0; i < 2; i++)
			broken.Send(ScriptBytes(NetScriptKind::Message, 0, "After", { i }));
		broken.Transport().Disconnect(1, static_cast<uint32_t>(NetDisconnectReason::ClientQuit));
		const auto brokenSince = Log::GetHistory().GetTotalCount();
		events = PumpAll(session, host, 10);
		CHECK(OfKind(events, NetworkEvent::Kind::Message).size() == 3);
		CHECK(OfKind(events, NetworkEvent::Kind::PeerDisconnected).size() == 1);
		CHECK(host.GetStatistics().ScriptMessagesDropped == 3);
		CHECK(BrecciaTest::CountLogMessages(brokenSince, "does not decode before it left") == 1);
		const uint64_t droppedBefore = host.GetStatistics().ScriptMessagesDropped;

		// A client disconnected for flooding: what it queued is dropped and counted, never handed over.
		RawPeer flood(session, session.Link().Connect(session.GetPort()));
		REQUIRE(flood.Join() == 3);
		host.TakeEvents();
		for (size_t i = 0; i < NetworkSystem::MaxInboxMessages + 10; i++)
			flood.Send(ScriptBytes(NetScriptKind::Message, 0, "Flood", nlohmann::json::array()));
		events = PumpAll(session, host, 10);
		const auto left = std::ranges::find_if(events, [](const NetworkEvent& event) { return event.Type == NetworkEvent::Kind::PeerDisconnected; });
		REQUIRE(left != events.end());
		CHECK(left->Reason == NetDisconnectReason::InvalidPacket);
		CHECK(std::none_of(left, events.end(), [](const NetworkEvent& event) { return event.Type == NetworkEvent::Kind::Message; }));
		const size_t delivered = OfKind(events, NetworkEvent::Kind::Message).size();
		CHECK(delivered < NetworkSystem::MaxInboxMessages);
		CHECK(host.GetStatistics().ScriptMessagesDropped - droppedBefore == NetworkSystem::MaxInboxMessages - delivered);
	}

	TEST_CASE("A client's script messages count against its bandwidth, and a muted budget refuses them")
	{
		NetworkSettings settings;
		settings.MaxBytesPerSecond = NetworkSettings::MinBytesPerSecond;
		Session session({}, settings);
		session.StartHost();
		session.AddClient();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(0).GetState() == NetConnectionState::Connected; }, 60));
		std::string error;
		const std::string payload(700, 'p');
		bool refused = false;
		for (int i = 0; i < 5 && !refused; i++)
			refused = !session.ClientNetwork(0).SendScript(0, { NetScriptKind::Message, 0, "Big", { payload } }, true, error);
		CHECK(refused);
		CHECK(error.find("MaxBytesPerSecond") != std::string::npos);
	}

	TEST_CASE("Interpolation: a pause between moves, a new parent, and a change of rotation alone")
	{
		NetworkSettings settings = WithDelay(0.1f);
		settings.TickRate = 20.0f;
		Session session({}, settings);
		UUID moverId = 0;
		UUID anchorId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			moverId = CreateNetworked(scene, "Mover").GetUUID();
			anchorId = CreateNetworked(scene, "Anchor", 0, { 10.0f, 0.0f, 0.0f }).GetUUID();
		});
		Scene& client = session.AddClient();
		REQUIRE(session.RunUntil([&] { return client.GetEntityByUUID(moverId) && client.GetEntityByUUID(anchorId); }, 60));
		session.Run(30);
		Entity hostMover = host.GetEntityByUUID(moverId);
		Entity clientMover = client.GetEntityByUUID(moverId);
		const auto shownX = [&] { return clientMover.GetTransform().Translation.x; };

		// Moving, then still for one snapshot, then moving again: no frame jumps.
		std::vector<float> shown;
		for (int frame = 0; frame < 90; frame++)
		{
			if (frame < 30 || frame >= 33)
				hostMover.GetTransform().Translation.x += 3.0f * Step;
			session.Run(1);
			shown.push_back(shownX());
		}
		for (size_t i = 1; i < shown.size(); i++)
		{
			CHECK(shown[i] - shown[i - 1] >= -1e-5f);
			CHECK(shown[i] - shown[i - 1] <= 3.0f * Step * 1.6f);
		}
		session.Run(30);
		REQUIRE(shownX() == hostMover.GetTransform().Translation.x);

		// Only the rotation changes: the position it already reached stays.
		const float restX = shownX();
		for (int frame = 0; frame < 30; frame++)
		{
			hostMover.GetTransform().Rotation = glm::angleAxis(0.05f * static_cast<float>(frame + 1), glm::vec3(0, 1, 0));
			session.Run(1);
			CHECK(shownX() == restX);
		}
		session.Run(30);
		CHECK(glm::abs(glm::dot(clientMover.GetTransform().Rotation, hostMover.GetTransform().Rotation)) > 0.99999f);

		// A new parent: the world position jumps there, with nothing in between.
		const glm::vec3 before = glm::vec3(client.GetWorldTransform(clientMover)[3]);
		host.SetParent(hostMover, host.GetEntityByUUID(anchorId), false);
		const glm::vec3 after = glm::vec3(host.GetWorldTransform(hostMover)[3]);
		REQUIRE(glm::length(after - before) > 5.0f);
		for (int frame = 0; frame < 30; frame++)
		{
			session.Run(1);
			const glm::vec3 now = glm::vec3(client.GetWorldTransform(clientMover)[3]);
			CHECK((glm::length(now - before) < 1e-4f || glm::length(now - after) < 1e-4f));
		}
		CHECK(glm::length(glm::vec3(client.GetWorldTransform(clientMover)[3]) - after) < 1e-4f);
	}

	TEST_CASE("Prediction holds over a lossy, jittery link, through jumps, and keeps the client's rotation")
	{
		Session session({ .Latency = 0.04f, .Jitter = 0.02f, .Loss = 0.1f }, WithDelay(0.1f));
		UUID playerId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			AddGround(scene);
			playerId = CreatePredictedCharacter(scene, 1).GetUUID();
		});
		Scene& client = session.AddClient([](Scene& scene) { AddGround(scene); });
		REQUIRE(session.RunUntil([&] { return client.GetEntityByUUID(playerId) && session.ClientNetwork(0).IsPredicted(client.GetEntityByUUID(playerId)); }, 240));
		session.Run(60);
		Entity hostPlayer = host.GetEntityByUUID(playerId);
		Entity clientPlayer = client.GetEntityByUUID(playerId);
		PhysicsWorld& physics = *client.GetPhysicsWorld();
		const uint64_t corrections = session.ClientNetwork(0).GetStatistics().Corrections;
		for (int frame = 0; frame < 120; frame++)
		{
			const bool jump = frame % 40 == 0;
			physics.MoveCharacter(clientPlayer, { 2.0f, jump ? 5.0f : 0.0f, 1.0f });
			// The player turns as it goes: a script's rotation, which corrections keep.
			clientPlayer.GetTransform().Rotation = glm::angleAxis(0.01f * static_cast<float>(frame), glm::vec3(0, 1, 0));
			session.Run(1);
		}
		const glm::quat turned = clientPlayer.GetTransform().Rotation;
		physics.MoveCharacter(clientPlayer, glm::vec3(0.0f));
		session.Run(120);
		CHECK(glm::abs(glm::dot(clientPlayer.GetTransform().Rotation, turned)) > 0.99999f);
		CHECK(glm::length(clientPlayer.GetTransform().Translation - hostPlayer.GetTransform().Translation) < 1e-3f);
		CHECK(hostPlayer.GetTransform().Translation.x > 3.0f);
		// Nothing in this world disagrees, so nothing was visibly corrected.
		CHECK(session.ClientNetwork(0).GetStatistics().Corrections == corrections);
		CHECK(session.HostNetwork().GetStatistics().IdleSteps == 0);
	}

	TEST_CASE("A predicted character obeys the host's speed limit, which its owner's inputs keep too, and a fast client stays connected")
	{
		Session session({}, WithDelay(0.0f));
		UUID playerId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			AddGround(scene);
			Entity player = CreatePredictedCharacter(scene, 1);
			player.GetComponent<NetworkedComponent>().MaxInputSpeed = 4.0f;
			player.GetComponent<CharacterControllerComponent>().GravityFactor = 0.0f;
			playerId = player.GetUUID();
		});
		Scene& client = session.AddClient([](Scene& scene) { AddGround(scene); });
		REQUIRE(session.RunUntil([&] { return client.GetEntityByUUID(playerId) && session.ClientNetwork(0).IsPredicted(client.GetEntityByUUID(playerId)); }, 120));
		Entity hostPlayer = host.GetEntityByUUID(playerId);
		Entity clientPlayer = client.GetEntityByUUID(playerId);
		const float startX = hostPlayer.GetTransform().Translation.x;
		// Faster than the protocol's limit, in a direction where scaling to exactly the limit rounds above it: the client
		// clamps below rather than sending what the host refuses (which would end its session).
		glm::vec3 fast(0.0f);
		Random random(7);
		for (int attempt = 0; attempt < 10000; attempt++)
		{
			const glm::vec3 candidate(random.Range(0.5f, 1.0f), random.Range(-0.3f, 0.3f), random.Range(-1.0f, 1.0f));
			const glm::vec3 velocity = candidate * (2.0f * NetProtocol::MaxInputSpeed / glm::length(candidate));
			// What scaling it to exactly the limit gives, as a naive clamp would.
			if (glm::length(velocity * (NetProtocol::MaxInputSpeed / glm::length(velocity))) > NetProtocol::MaxInputSpeed)
			{
				fast = velocity;
				break;
			}
		}
		REQUIRE(glm::length(fast) > 0.0f);
		const auto since = Log::GetHistory().GetTotalCount();
		client.GetPhysicsWorld()->MoveCharacter(clientPlayer, fast);
		session.Run(30);
		client.GetPhysicsWorld()->MoveCharacter(clientPlayer, glm::vec3(0.0f));
		session.Run(30);
		CHECK(session.ClientNetwork(0).GetState() == NetConnectionState::Connected);
		// The owner clamped its inputs to its copy of Networked.MaxInputSpeed, not only to the wire's limit.
		CHECK(BrecciaTest::CountLogMessages(since, "faster than its Networked.MaxInputSpeed (4 m/s)") == 1);
		// The host moved it at its own limit: 4 m/s for half a second.
		CHECK(glm::length(hostPlayer.GetTransform().Translation - glm::vec3(startX, hostPlayer.GetTransform().Translation.y, 0.0f)) == doctest::Approx(2.0f).epsilon(0.05));
		CHECK(clientPlayer.GetTransform().Translation.x == doctest::Approx(hostPlayer.GetTransform().Translation.x).epsilon(1e-3));
	}

	TEST_CASE("A host steps a predicted character whose owner falls silent, so it does not hang in the air")
	{
		Session session({}, WithDelay(0.0f));
		UUID playerId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			AddGround(scene);
			playerId = CreatePredictedCharacter(scene, 1, { 0.0f, 6.0f, 0.0f }).GetUUID();
			// A long frame may credit more steps than the host takes for a silent owner in one frame.
			scene.GetPhysicsSettings().MaxStepsPerFrame = 30;
		});
		RawPeer raw(session, session.Link().Connect(session.GetPort()));
		REQUIRE(raw.Join() == 1);
		session.Run(120);
		CHECK(session.HostNetwork().GetStatistics().IdleSteps > 0);
		// It fell from 6 m to the ground.
		CHECK(host.GetEntityByUUID(playerId).GetTransform().Translation.y < 2.0f);
		CHECK(host.GetPhysicsWorld()->IsDrivenExternally(host.GetEntityByUUID(playerId)));
		// No more than MaxIdleStepsPerFrame at once, however long the frame, and long frames bank no more than the input
		// window: a burst of inputs after them moves the character at most that far ahead.
		// (A frame's steps are credited when the next frame receives.)
		const uint64_t beforeLong = session.HostNetwork().GetStatistics().IdleSteps;
		host.OnUpdate(0.4f);
		host.OnUpdate(Step);
		CHECK(session.HostNetwork().GetStatistics().IdleSteps - beforeLong <= NetworkSystem::MaxIdleStepsPerFrame + 1);
		for (int frame = 0; frame < 4; frame++)
			host.OnUpdate(0.4f);
		const float window = std::floor(NetworkSystem::InputWindowSeconds / host.GetFixedTimestep());
		{
			const uint64_t applied = session.HostNetwork().GetStatistics().InputsApplied;
			raw.Send(InputUpdate(playerId, 1, NetProtocol::MaxInputsPerCharacter, glm::vec3(0.0f)), NetChannel::Unreliable);
			session.Link().Advance(Step);
			session.HostNetwork().Receive(Step);
			CHECK(static_cast<float>(session.HostNetwork().GetStatistics().InputsApplied - applied) <= window);
		}
		session.Run(30);
		// The owner speaks again, an input per step: once its first input has arrived, the host takes no more steps of
		// its own.
		uint32_t sequence = NetProtocol::MaxInputsPerCharacter + 1;
		uint64_t speaking = 0;
		for (int frame = 0; frame < 30; frame++)
		{
			raw.Send(InputUpdate(playerId, sequence, 1), NetChannel::Unreliable);
			sequence++;
			session.Run(1);
			if (frame == 1)
				speaking = session.HostNetwork().GetStatistics().IdleSteps;
		}
		CHECK(session.HostNetwork().GetStatistics().IdleSteps == speaking);
		// Then in bursts (four steps every fourth frame, as a client sending at 15 updates per second does): still none.
		const uint64_t idle = session.HostNetwork().GetStatistics().IdleSteps;
		for (int frame = 0; frame < 60; frame++)
		{
			if (frame % 4 == 3)
			{
				raw.Send(InputUpdate(playerId, sequence, 4, glm::vec3(1.0f, 0.0f, 0.0f)), NetChannel::Unreliable);
				sequence += 4;
			}
			session.Run(1);
		}
		CHECK(session.HostNetwork().GetStatistics().IdleSteps == idle);
	}

	TEST_CASE("A host's steps for a silent owner keep a character's world rotation under a turned parent")
	{
		Session session({}, WithDelay(0.0f));
		UUID playerId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			AddGround(scene);
			Entity parent = scene.CreateEntity("Turned");
			parent.GetTransform().Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
			Entity player = CreatePredictedCharacter(scene, 1, { 0.0f, 3.0f, 0.0f });
			scene.SetParent(player, parent, false);
			playerId = player.GetUUID();
		});
		RawPeer raw(session, session.Link().Connect(session.GetPort()));
		REQUIRE(raw.Join() == 1);
		Entity player = host.GetEntityByUUID(playerId);
		const glm::quat world = glm::quat_cast(glm::mat3(host.GetWorldTransform(player)));
		session.Run(60);
		REQUIRE(session.HostNetwork().GetStatistics().IdleSteps > 0);
		const glm::quat after = glm::quat_cast(glm::mat3(host.GetWorldTransform(player)));
		CHECK(glm::abs(glm::dot(glm::normalize(after), glm::normalize(world))) > 0.9999f);
	}

	TEST_CASE("A Predicted entity without a character controller replicates like Server to its owner")
	{
		Session session({}, WithDelay(0.0f));
		UUID boxId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			Entity box = CreateNetworked(scene, "Box");
			box.GetComponent<NetworkedComponent>() = { 1, NetworkAuthority::Predicted };
			boxId = box.GetUUID();
		});
		Scene& client = session.AddClient();
		REQUIRE(session.RunUntil([&] { return static_cast<bool>(client.GetEntityByUUID(boxId)); }, 60));
		// Acknowledged: a change now travels as a field update, not inside a repeated spawn.
		session.Run(30);
		CHECK_FALSE(session.ClientNetwork(0).IsPredicted(client.GetEntityByUUID(boxId)));
		host.GetEntityByUUID(boxId).GetTransform().Translation.x = 5.0f;
		session.Run(10);
		CHECK(client.GetEntityByUUID(boxId).GetTransform().Translation.x == 5.0f);
	}

	TEST_CASE("The sender keeps a client update within its size limit with inputs on top")
	{
		ReplicationSender sender(ReplicationSenderOptions{ .Direction = ReplicationDirection::ClientToHost, .MaxMessageBytes = 2048 });
		std::vector<ReplicatedEntity> entities;
		for (uint64_t id = 1; id <= 60; id++)
			entities.push_back(TransformEntity(UUID(id), static_cast<float>(id)));
		const ReplicationWorld world(std::move(entities), {});
		NetStateExtras extras;
		extras.Inputs.push_back({ UUID(99), 1, std::vector<CharacterInput>(NetProtocol::MaxInputsPerCharacter) });
		const std::vector<uint8_t> message = sender.Build(world, 0, 4096, {}, extras);
		CHECK(message.size() <= 2048);
		CHECK(sender.GetDeferredRecordCount() > 0);
		const std::optional<NetMessage> decoded = DecodeMessage(message);
		REQUIRE(decoded);
		CHECK(std::get<NetState>(decoded->Body).Extras.Inputs.size() == 1);
	}

	TEST_CASE("Lua Network: handles cancel network handlers, round trips of unknown peers are nil, and one session change per frame")
	{
		BrecciaTest::TempProject project("LuaNetworkHandles");
		const std::string script = project.WriteFile("Assets/Scripts/Handler.lua", R"(
			local Handler = {}
			function Handler:OnCreate()
				Heard = 0
				LeftCalls = 0
				Network.On("PeerDisconnected", function(peer)
					LeftCalls = LeftCalls + 1
					Network.Disconnect()
				end)
			end
			return Handler
		)");
		Scene empty;
		project.WriteFile("Assets/Scenes/Empty.bscene", SceneSerializer::SerializeScene(empty).dump());
		SimulatedNetwork network;
		Ref<Scene> host = CreateRef<Scene>("Host");
		host->CreateEntity("Handler").AddComponent<ScriptComponent>().Script = script;
		host->SetNetworkTransports(network.CreateFactory());
		Ref<Scene> client = CreateRef<Scene>("Client");
		client->SetNetworkTransports(network.CreateFactory());
		host->OnRuntimeStart();
		client->OnRuntimeStart();
		const auto run = [&](int frames) {
			for (int i = 0; i < frames; i++)
			{
				host->OnUpdate(Step);
				client->OnUpdate(Step);
				network.Advance(Step);
			}
		};
		// A network handle cancels its own handler, not an Events handler with the same number.
		CHECK(BrecciaTest::Eval(*host, "local e = Events.On('X', function() end) local h = Network.On('X', function() Heard = Heard + 1 end) local cancelled = h:Cancel() return tostring(cancelled) .. tostring(h:IsActive()) .. tostring(e:IsActive())") == "truefalsetrue");
		// One change a frame: a second start, or a scene load, waits for the next frame.
		CHECK(BrecciaTest::Eval(*host, "return Network.Host(0)") == "true");
		CHECK(BrecciaTest::Eval(*host, "local ok, err = Network.Host(0) return err").find("already waiting") != std::string::npos);
		CHECK(BrecciaTest::Eval(*host, "local ok, err = pcall(Scene.Load, 'Assets/Scenes/Empty.bscene') return err").find("during a network session") != std::string::npos);
		run(1);
		CHECK(BrecciaTest::Eval(*client, "Scene.Load('Assets/Scenes/Empty.bscene') local ok, err = Network.Connect('localhost', 1) return err").find("scene load") != std::string::npos);
		run(1);
		CHECK(BrecciaTest::Eval(*client, "return Network.Connect('localhost', " + BrecciaTest::Eval(*host, "return Network.GetPort()") + ")") == "true");
		run(30);
		CHECK(BrecciaTest::Eval(*host, "return tostring(Network.GetRoundTripTime(1) ~= nil) .. tostring(Network.GetRoundTripTime(9))") == "truenil");
		CHECK(BrecciaTest::Eval(*client, "return tostring(Network.GetRoundTripTime() ~= nil)") == "true");
		// A handler that ends the session when a client leaves: it ends at the end of that frame, not inside the session.
		CHECK(BrecciaTest::Eval(*client, "Network.Disconnect() return true") == "true");
		run(10);
		CHECK(BrecciaTest::Eval(*host, "return LeftCalls") == "1");
		CHECK_FALSE(host->GetNetworkSystem());
		CHECK(host->GetScriptEngine()->GetErrors().empty());
		host->OnRuntimeStop();
		client->OnRuntimeStop();
	}

	TEST_CASE("A client's view of the host clock never runs back, whatever times a host sends")
	{
		FakeHost host;
		host.Advance(2);
		// Connected to the transport, but not yet accepted: no round trip to report.
		REQUIRE(host.Network().GetState() == NetConnectionState::Connecting);
		CHECK_FALSE(host.Network().GetRoundTripTime(0));
		host.Welcome();
		NetState state;
		state.Extras.Time = 10.0;
		host.Snapshot(state);
		const double first = host.Network().GetTime();
		REQUIRE(first >= 10.0);
		state.Extras.Time = 2.0;
		host.Snapshot(state);
		CHECK(host.Network().GetTime() >= first);
	}

	TEST_CASE("Lag compensation knows where bodies were before the first client joined")
	{
		Session session({}, WithDelay(0.0f));
		UUID targetId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			Entity target = CreateNetworked(scene, "Target", 0, { 0.0f, 1.0f, 0.0f });
			target.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
			target.AddComponent<BoxColliderComponent>().HalfExtents = { 0.5f, 0.5f, 0.5f };
			targetId = target.GetUUID();
		});
		Entity target = host.GetEntityByUUID(targetId);
		session.Run(10);
		const double then = session.HostNetwork().GetTime();
		// It moves away; then a client joins, which acted on what it saw before it arrived.
		for (int frame = 0; frame < 20; frame++)
		{
			target.GetTransform().Translation.x += 0.2f;
			session.Run(1);
		}
		session.AddClient();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(0).GetState() == NetConnectionState::Connected; }, 60));
		REQUIRE(session.HostNetwork().GetTime() - then < NetworkSystem::MaxRewindSeconds);
		const std::optional<RaycastHit> hit = session.HostNetwork().RaycastAt(then, { 0.0f, 5.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 10.0f);
		REQUIRE(hit);
		CHECK(hit->EntityID == targetId);
		CHECK_FALSE(session.HostNetwork().RaycastAt(session.HostNetwork().GetTime(), { 0.0f, 5.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 10.0f));
	}

	TEST_CASE("NetworkSettings::ParseAddress reads a host and an optional port, and says what is wrong")
	{
		std::string host;
		uint16_t port = 0;
		CHECK(NetworkSettings::ParseAddress("example.org", 7777, host, port).empty());
		CHECK(host == "example.org");
		CHECK(port == 7777);
		CHECK(NetworkSettings::ParseAddress("10.0.0.2:4000", 7777, host, port).empty());
		CHECK(host == "10.0.0.2");
		CHECK(port == 4000);
		CHECK(Contains(NetworkSettings::ParseAddress("", 7777, host, port), "no host"));
		CHECK(Contains(NetworkSettings::ParseAddress(":4000", 7777, host, port), "no host"));
		CHECK(Contains(NetworkSettings::ParseAddress("host:", 7777, host, port), "not an integer"));
		CHECK(Contains(NetworkSettings::ParseAddress("host:x1", 7777, host, port), "not an integer"));
		CHECK(Contains(NetworkSettings::ParseAddress("host:0", 7777, host, port), "[1, 65535]"));
		CHECK(Contains(NetworkSettings::ParseAddress("host:65536", 7777, host, port), "[1, 65535]"));
		// ENet has no IPv6: a second ':' is not taken as part of a host.
		CHECK(Contains(NetworkSettings::ParseAddress("a:b:7777", 7777, host, port), "more than one ':'"));
	}

	TEST_CASE("NetProtocol::CheckPrediction refuses what would break a client, and the decoder refuses it too")
	{
		const NetPrediction fine{ UUID(5), 10, { glm::vec3(1.0f), glm::vec3(2.0f), 0.5f, false } };
		CHECK(NetProtocol::CheckPrediction(fine).empty());
		std::vector<NetPrediction> invalid(5, fine);
		// The client would number its next input 0.
		invalid[0].Input = std::numeric_limits<uint32_t>::max();
		invalid[1].State.SinceGrounded = CharacterState::MaxSinceGrounded + 1.0f;
		invalid[2].State.SinceGrounded = -1.0f;
		invalid[3].State.Velocity.y = -2.0f * NetProtocol::MaxInputSpeed;
		invalid[4].State.Position.z = 2.0f * NetProtocol::MaxPredictedCoordinate;
		for (const NetPrediction& prediction : invalid)
		{
			CHECK_FALSE(NetProtocol::CheckPrediction(prediction).empty());
			NetState snapshot;
			snapshot.Sequence = 1;
			snapshot.Extras.Predictions.push_back(prediction);
			CHECK_FALSE(DecodeMessage(NetProtocol::EncodeState(NetMessageType::Snapshot, snapshot)));
		}
	}

	TEST_CASE("A script message that cannot be encoded says why: too deep, or not UTF-8")
	{
		Session session;
		session.StartHost();
		session.AddClient();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(0).GetState() == NetConnectionState::Connected; }, 60));
		nlohmann::json deep = 1;
		for (int i = 0; i < NetValue::MaxDepth + 4; i++)
			deep = nlohmann::json::array({ deep });
		std::string error;
		CHECK_FALSE(session.ClientNetwork(0).SendScript(0, { NetScriptKind::Message, 0, "Deep", nlohmann::json::array({ deep }) }, true, error));
		CHECK(Contains(error, "nested"));
		error.clear();
		CHECK_FALSE(session.ClientNetwork(0).SendScript(0, { NetScriptKind::Message, 0, "Text", nlohmann::json::array({ std::string("\xff\xfe") }) }, true, error));
		CHECK(Contains(error, "UTF-8"));
	}

	TEST_CASE("A client drops at once an RPC for an entity it lost or refused, and only reliable messages wait behind an RPC")
	{
		FakeHost host;
		host.Welcome();
		NetworkSystem& client = host.Network();
		const auto received = [&] { return client.GetStatistics().ScriptMessagesReceived; };
		const auto dropped = [&] { return client.GetStatistics().ScriptMessagesDropped; };
		// A spawn the client refuses (a file outside the project): an RPC for it is not worth waiting for.
		NetState refused;
		refused.Spawns.push_back(Spawn(10, { { "Mesh", { { "Mesh", "../../secret.gltf" } } } }));
		host.Snapshot(refused);
		REQUIRE_FALSE(host.Client().GetEntityByUUID(10));
		uint64_t droppedBefore = dropped();
		host.Send(ScriptBytes(NetScriptKind::Rpc, 10, "Open"));
		CHECK(dropped() == droppedBefore + 1);

		// An RPC for an entity not spawned yet waits; an unreliable message after it does not, a reliable one does.
		const uint64_t receivedBefore = received();
		host.Send(ScriptBytes(NetScriptKind::Rpc, 777, "Open"));
		host.Send(ScriptBytes(NetScriptKind::Message, 0, "Fast"), NetChannel::Unreliable);
		CHECK(received() == receivedBefore + 1);
		host.Send(ScriptBytes(NetScriptKind::Message, 0, "Slow"));
		CHECK(received() == receivedBefore + 1);
		// The spawn releases the RPC and what waited behind it.
		NetState spawn;
		spawn.Spawns.push_back(Spawn(777, { { "Transform", { { "Translation", { 1.0, 2.0, 3.0 } } } } }));
		host.Snapshot(spawn);
		REQUIRE(host.Client().GetEntityByUUID(777));
		CHECK(received() == receivedBefore + 3);

		// Despawned (its despawn overtook the RPC, on another channel): dropped at once too.
		NetState despawn;
		despawn.Despawns.emplace_back(777);
		host.Snapshot(despawn);
		REQUIRE_FALSE(host.Client().GetEntityByUUID(777));
		droppedBefore = dropped();
		host.Send(ScriptBytes(NetScriptKind::Rpc, 777, "Open"));
		host.Send(ScriptBytes(NetScriptKind::Message, 0, "After"));
		CHECK(dropped() == droppedBefore + 1);
		CHECK(received() == receivedBefore + 4);
	}

	TEST_CASE("A host counts the inputs lost on the way, up to the inputs a client keeps")
	{
		Session session({}, WithDelay(0.0f));
		UUID playerId = 0;
		session.StartHost([&](Scene& scene) {
			AddGround(scene);
			playerId = CreatePredictedCharacter(scene, 1).GetUUID();
		});
		RawPeer raw(session, session.Link().Connect(session.GetPort()));
		REQUIRE(raw.Join() == 1);
		session.Run(2);
		NetworkSystem& network = session.HostNetwork();
		const auto send = [&](uint32_t first, uint32_t count) {
			raw.Send(InputUpdate(playerId, first, count, glm::vec3(1.0f, 0.0f, 0.0f)), NetChannel::Unreliable);
			session.Run(2);
		};
		send(1, 4);
		CHECK(network.GetStatistics().InputsSkipped == 0);
		send(40, 4);
		CHECK(network.GetStatistics().InputsApplied == 8);
		CHECK(network.GetStatistics().InputsSkipped == 35);
		// A wild jump in the numbering counts at most what a client keeps unacknowledged.
		send(5000, 1);
		CHECK(network.GetStatistics().InputsSkipped == 35 + NetworkSystem::MaxUnacknowledgedInputs);
	}

	TEST_CASE("A host's inbox limits a burst, not what a client sends over a session")
	{
		NetworkSettings settings;
		settings.MaxMessagesPerSecond = 120;
		Session session({}, settings);
		session.StartHost();
		RawPeer raw(session, session.Link().Connect(session.GetPort()));
		REQUIRE(raw.Join() == 1);
		NetworkSystem& host = session.HostNetwork();
		host.TakeEvents();
		// Well past MaxInboxBytes in all, but within the rate.
		const std::string payload(size_t{ 30 } * 1024, 'p');
		const size_t count = NetworkSystem::MaxInboxBytes / payload.size() + 20;
		size_t delivered = 0;
		for (size_t i = 0; i < count + 30; i++)
		{
			if (i < count)
				raw.Send(ScriptBytes(NetScriptKind::Message, 0, "Steady", { payload }));
			session.Link().Advance(Step);
			host.Receive(Step);
			delivered += OfKind(host.TakeEvents(), NetworkEvent::Kind::Message).size();
		}
		CHECK(delivered == count);
		CHECK(host.GetPeers().size() == 1);
	}

	TEST_CASE("Connection events reach the scripts even past the event queue's limit")
	{
		NetworkSettings settings;
		settings.MaxMessagesPerSecond = NetworkSettings::MaxMessagesPerSecondLimit;
		Session session({}, settings);
		session.StartHost();
		RawPeer flood(session, session.Link().Connect(session.GetPort()));
		REQUIRE(flood.Join() == 1);
		RawPeer quiet(session, session.Link().Connect(session.GetPort()));
		REQUIRE(quiet.Join() == 2);
		NetworkSystem& host = session.HostNetwork();
		host.TakeEvents();
		const auto since = Log::GetHistory().GetTotalCount();
		// Nobody takes the events meanwhile: the queue fills with messages, and the rest wait in the inbox rather than
		// being dropped from a full queue.
		for (int frame = 0; frame < 6; frame++)
		{
			for (int i = 0; i < 1000; i++)
				flood.Send(ScriptBytes(NetScriptKind::Message, 0, "Flood", nlohmann::json::array()));
			session.Link().Advance(Step);
			host.Receive(Step);
		}
		quiet.Transport().Disconnect(1, static_cast<uint32_t>(NetDisconnectReason::ClientQuit));
		for (int frame = 0; frame < 3; frame++)
		{
			session.Link().Advance(Step);
			host.Receive(Step);
		}
		const std::vector<NetworkEvent> events = host.TakeEvents();
		CHECK(OfKind(events, NetworkEvent::Kind::Message).size() <= NetworkSystem::MaxQueuedEvents);
		CHECK(BrecciaTest::CountLogMessages(since, "events waited for the scripts in one frame") == 0);
		const std::vector<NetworkEvent> left = OfKind(events, NetworkEvent::Kind::PeerDisconnected);
		CHECK(std::ranges::any_of(left, [](const NetworkEvent& event) { return event.Peer == 2 && event.Reason == NetDisconnectReason::ClientQuit; }));
	}

	TEST_CASE("A host does not send a predicted state its client would refuse, and the client stays connected")
	{
		Session session({}, WithDelay(0.0f));
		UUID playerId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			AddGround(scene);
			playerId = CreatePredictedCharacter(scene, 1).GetUUID();
		});
		Scene& client = session.AddClient([](Scene& scene) { AddGround(scene); });
		REQUIRE(session.RunUntil([&] { return client.GetEntityByUUID(playerId) && session.ClientNetwork(0).IsPredicted(client.GetEntityByUUID(playerId)); }, 120));
		session.Run(10);
		const auto since = Log::GetHistory().GetTotalCount();
		// Falling faster than a predicted state may carry.
		Entity hostPlayer = host.GetEntityByUUID(playerId);
		REQUIRE(host.GetPhysicsWorld()->SetCharacterState(hostPlayer, { { 0.0f, 1000.0f, 0.0f }, { 0.0f, -2.0f * NetProtocol::MaxInputSpeed, 0.0f }, 1.0f, false }));
		session.Run(60);
		CHECK(session.ClientNetwork(0).GetState() == NetConnectionState::Connected);
		CHECK(BrecciaTest::CountLogMessages(since, "cannot be corrected") == 1);
	}

	TEST_CASE("Network events a script cannot take are logged once per sender and problem, within a cap per sender")
	{
		BrecciaTest::TempProject project("NetworkEventProblems");
		const std::string listener = project.WriteFile("Assets/Scripts/Listener.lua", R"(
			local Listener = {}
			function Listener:OnCreate()
			Network.On("Heard", function() Heard = (Heard or 0) + 1 end)
			Network.On("Kill", function() Scene.FindEntityByName("Doomed"):Destroy() end)
		end
			return Listener
		)");
		const std::string doomed = project.WriteFile("Assets/Scripts/Doomed.lua", R"(
			local Doomed = {}
			function Doomed:OnCreate() Network.On("Doomed", function() DoomedHeard = true end) end
			return Doomed
		)");
		const std::string broken = project.WriteFile("Assets/Scripts/Broken.lua", R"(
			local Broken = {}
			Broken.Rpcs = { Hit = "Any" }
			function Broken:OnCreate()
				Network.On("Orphan", function() OrphanHeard = true end)
				error("broken on purpose")
			end
			function Broken:Hit() HitRan = true end
			return Broken
		)");
		Scene scene;
		scene.CreateEntity("Listener").AddComponent<ScriptComponent>().Script = listener;
		Entity brokenEntity = scene.CreateEntity("Broken");
		brokenEntity.AddComponent<ScriptComponent>().Script = broken;
		scene.OnRuntimeStart();
		ScriptEngine& scripts = *scene.GetScriptEngine();
		const size_t errors = scripts.GetErrors().size();
		REQUIRE(errors >= 1);
		const auto since = Log::GetHistory().GetTotalCount();
		const auto deliver = [&](std::vector<NetworkEvent> events) { scripts.Update(Step, {}, events); };
		const auto message = [](uint32_t peer, std::string name) { return NetworkEvent{ .Type = NetworkEvent::Kind::Message, .Peer = peer, .Name = std::move(name) }; };

		deliver({ message(1, "Heard"), message(1, "Nobody"), message(1, "Nobody") });
		CHECK(BrecciaTest::Eval(scene, "return Heard") == "1");
		CHECK(BrecciaTest::CountLogMessages(since, "which no Network.On handler listens for") == 1);
		// A failed script's handlers are gone with it.
		deliver({ message(1, "Orphan") });
		CHECK(BrecciaTest::Eval(scene, "return tostring(OrphanHeard)") == "nil");
		CHECK(BrecciaTest::CountLogMessages(since, "client 1 sent message 'Orphan', which no Network.On handler") == 1);

		// An RPC for a script that is not running calls nothing and adds no script error.
		deliver({ NetworkEvent{ .Type = NetworkEvent::Kind::Rpc, .Peer = 1, .Entity = brokenEntity.GetUUID(), .Name = "Hit" } });
		CHECK(BrecciaTest::Eval(scene, "return tostring(HitRan)") == "nil");
		CHECK(BrecciaTest::CountLogMessages(since, "has no running script") == 1);
		CHECK(scripts.GetErrors().size() == errors);

		// One sender's flood of names fills only its own budget.
		std::vector<NetworkEvent> flood;
		flood.reserve(100);
		for (int i = 0; i < 100; i++)
			flood.push_back(message(1, "Miss" + std::to_string(i)));
		deliver(flood);
		CHECK(BrecciaTest::CountLogMessages(since, "client 1 sent message 'Miss") < 64);
		CHECK(BrecciaTest::CountLogMessages(since, "problems with script messages from client 1 were logged") == 1);
		deliver({ message(2, "Other") });
		CHECK(BrecciaTest::CountLogMessages(since, "client 2 sent message 'Other'") == 1);
		scene.OnRuntimeStop();

		// In a session: a handler destroys an entity, whose own handler then misses a message of the same frame. The log
		// says why rather than blaming the name.
		Session session;
		session.StartHost([&](Scene& host) {
			host.CreateEntity("Listener").AddComponent<ScriptComponent>().Script = listener;
			host.CreateEntity("Doomed").AddComponent<ScriptComponent>().Script = doomed;
		});
		session.AddClient();
		REQUIRE(session.RunUntil([&] { return session.ClientNetwork(0).GetState() == NetConnectionState::Connected; }, 60));
		std::string error;
		REQUIRE(session.ClientNetwork(0).SendScript(0, { NetScriptKind::Message, 0, "Kill", nlohmann::json::array() }, true, error));
		REQUIRE(session.ClientNetwork(0).SendScript(0, { NetScriptKind::Message, 0, "Doomed", nlohmann::json::array() }, true, error));
		session.Run(5);
		CHECK_FALSE(session.Host().FindEntityByName("Doomed"));
		CHECK(BrecciaTest::Eval(session.Host(), "return tostring(DoomedHeard)") == "nil");
		CHECK(BrecciaTest::CountLogMessages(since, "belong to entities being destroyed") == 1);
	}

	TEST_CASE("A peer's physics never moves another scene's character with the same UUID")
	{
		Session session({}, WithDelay(0.0f));
		UUID playerId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			AddGround(scene);
			playerId = CreatePredictedCharacter(scene, 1).GetUUID();
		});
		Scene& client = session.AddClient([](Scene& scene) { AddGround(scene); });
		REQUIRE(session.RunUntil([&] { return client.GetEntityByUUID(playerId) && session.ClientNetwork(0).IsPredicted(client.GetEntityByUUID(playerId)); }, 120));
		Entity hostPlayer = host.GetEntityByUUID(playerId);
		Entity clientPlayer = client.GetEntityByUUID(playerId);
		const std::optional<CharacterState> before = client.GetPhysicsWorld()->GetCharacterState(clientPlayer);
		REQUIRE(before);
		CHECK_FALSE(client.GetPhysicsWorld()->StepCharacter(hostPlayer, { glm::vec3(5.0f, 0.0f, 0.0f), glm::quat(1, 0, 0, 0) }));
		CHECK_FALSE(client.GetPhysicsWorld()->GetCharacterState(hostPlayer));
		CHECK(client.GetPhysicsWorld()->GetCharacterState(clientPlayer) == before);
	}

	TEST_CASE("Prediction across a long round trip sends the newest inputs under their own numbers")
	{
		// About 40 steps in flight: more than one update carries, so each sends only the newest.
		Session session({ .Latency = 0.3f }, WithDelay(0.1f));
		UUID playerId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			AddGround(scene);
			playerId = CreatePredictedCharacter(scene, 1).GetUUID();
		});
		Scene& client = session.AddClient([](Scene& scene) { AddGround(scene); });
		REQUIRE(session.RunUntil([&] { return client.GetEntityByUUID(playerId) && session.ClientNetwork(0).IsPredicted(client.GetEntityByUUID(playerId)); }, 240));
		session.Run(60);
		Entity hostPlayer = host.GetEntityByUUID(playerId);
		Entity clientPlayer = client.GetEntityByUUID(playerId);
		const uint64_t corrections = session.ClientNetwork(0).GetStatistics().Corrections;
		client.GetPhysicsWorld()->MoveCharacter(clientPlayer, { 2.0f, 0.0f, 0.0f });
		session.Run(120);
		client.GetPhysicsWorld()->MoveCharacter(clientPlayer, glm::vec3(0.0f));
		session.Run(120);
		CHECK(hostPlayer.GetTransform().Translation.x > 3.0f);
		CHECK(glm::length(clientPlayer.GetTransform().Translation - hostPlayer.GetTransform().Translation) < 1e-3f);
		CHECK(session.HostNetwork().GetStatistics().InputsSkipped == 0);
		CHECK(session.ClientNetwork(0).GetStatistics().Corrections == corrections);
	}

	TEST_CASE("A short outage does not make the host step a character its owner still drives")
	{
		// The owner's first input took ten steps to arrive; that trip must not use up the silence the input window allows,
		// or a few lost updates later would look like a silent owner.
		Session session({}, WithDelay(0.0f));
		UUID playerId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			AddGround(scene);
			playerId = CreatePredictedCharacter(scene, 1).GetUUID();
		});
		RawPeer raw(session, session.Link().Connect(session.GetPort()));
		REQUIRE(raw.Join() == 1);
		session.Run(10);
		const uint64_t idle = session.HostNetwork().GetStatistics().IdleSteps;
		uint32_t next = 1;
		const auto sendThrough = [&](uint32_t last) {
			raw.Send(InputUpdate(playerId, next, last - next + 1), NetChannel::Unreliable);
			next = last + 1;
		};
		// An input a step, then nothing for 7 steps (lost updates), then the inputs it missed with the next ones.
		for (uint32_t frame = 1; frame <= 30; frame++)
		{
			sendThrough(frame);
			session.Run(1);
		}
		session.Run(7);
		for (uint32_t frame = 38; frame <= 60; frame++)
		{
			sendThrough(frame);
			session.Run(1);
		}
		CHECK(session.HostNetwork().GetStatistics().IdleSteps == idle);
		CHECK(host.GetEntityByUUID(playerId).GetTransform().Translation.x > 0.8f);
	}

	TEST_CASE("Prediction at the lowest tick rate still sends every input")
	{
		NetworkSettings settings = WithDelay(0.1f);
		settings.TickRate = NetworkSettings::MinTickRate;
		Session session({}, settings);
		UUID playerId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			AddGround(scene);
			playerId = CreatePredictedCharacter(scene, 1).GetUUID();
		});
		Scene& client = session.AddClient([](Scene& scene) { AddGround(scene); });
		REQUIRE(session.RunUntil([&] { return client.GetEntityByUUID(playerId) && session.ClientNetwork(0).IsPredicted(client.GetEntityByUUID(playerId)); }, 600));
		session.Run(120);
		Entity hostPlayer = host.GetEntityByUUID(playerId);
		Entity clientPlayer = client.GetEntityByUUID(playerId);
		const float startX = hostPlayer.GetTransform().Translation.x;
		client.GetPhysicsWorld()->MoveCharacter(clientPlayer, { 2.0f, 0.0f, 0.0f });
		session.Run(120);
		client.GetPhysicsWorld()->MoveCharacter(clientPlayer, glm::vec3(0.0f));
		session.Run(180);
		// Two seconds at 2 m/s, all of them applied on the host.
		CHECK(hostPlayer.GetTransform().Translation.x - startX == doctest::Approx(4.0f).epsilon(0.03));
		CHECK(session.HostNetwork().GetStatistics().InputsSkipped == 0);
		CHECK(glm::length(clientPlayer.GetTransform().Translation - hostPlayer.GetTransform().Translation) < 1e-3f);
	}

	TEST_CASE("Prediction recovers from an outage longer than the inputs a client keeps")
	{
		Session session({ .Latency = 0.02f }, WithDelay(0.1f));
		UUID playerId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			AddGround(scene);
			playerId = CreatePredictedCharacter(scene, 1).GetUUID();
		});
		Scene& client = session.AddClient([](Scene& scene) { AddGround(scene); });
		REQUIRE(session.RunUntil([&] { return client.GetEntityByUUID(playerId) && session.ClientNetwork(0).IsPredicted(client.GetEntityByUUID(playerId)); }, 120));
		session.Run(30);
		Entity hostPlayer = host.GetEntityByUUID(playerId);
		Entity clientPlayer = client.GetEntityByUUID(playerId);
		// Five seconds of walking with nothing getting through: past MaxUnacknowledgedInputs.
		client.GetPhysicsWorld()->MoveCharacter(clientPlayer, { 1.0f, 0.0f, 0.0f });
		const uint64_t skipped = session.HostNetwork().GetStatistics().InputsSkipped;
		session.Link().SetLinkSettings({ .Latency = 0.02f, .Loss = 1.0f });
		session.Run(300);
		session.Link().SetLinkSettings({ .Latency = 0.02f });
		session.Run(30);
		REQUIRE(session.ClientNetwork(0).GetState() == NetConnectionState::Connected);
		// The client forgot the oldest of its inputs but kept numbering on: the host sees the whole gap (counted up to
		// what a client keeps), not a renumbered one.
		CHECK(session.HostNetwork().GetStatistics().InputsSkipped - skipped == NetworkSystem::MaxUnacknowledgedInputs);
		// The host takes the client's new inputs again.
		const float resumedX = hostPlayer.GetTransform().Translation.x;
		session.Run(60);
		CHECK(hostPlayer.GetTransform().Translation.x > resumedX + 0.8f);
		client.GetPhysicsWorld()->MoveCharacter(clientPlayer, glm::vec3(0.0f));
		session.Run(60);
		CHECK(glm::length(clientPlayer.GetTransform().Translation - hostPlayer.GetTransform().Translation) < 1e-3f);
	}

	TEST_CASE("A client whose prediction restarts numbers its inputs after what the host applied")
	{
		Session session({ .Latency = 0.02f }, WithDelay(0.1f));
		UUID playerId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			AddGround(scene);
			playerId = CreatePredictedCharacter(scene, 1).GetUUID();
		});
		Scene& client = session.AddClient([](Scene& scene) { AddGround(scene); });
		const auto predicted = [&] { return client.GetEntityByUUID(playerId) && session.ClientNetwork(0).IsPredicted(client.GetEntityByUUID(playerId)); };
		REQUIRE(session.RunUntil(predicted, 120));
		// The host applies a couple of seconds of inputs.
		session.Run(120);
		Entity hostPlayer = host.GetEntityByUUID(playerId);
		Entity clientPlayer = client.GetEntityByUUID(playerId);
		// The client's character is rebuilt (a script removed and added its controller): its inputs start again at 1.
		clientPlayer.RemoveComponent<CharacterControllerComponent>();
		session.Run(1);
		REQUIRE_FALSE(predicted());
		clientPlayer.AddComponent<CharacterControllerComponent>();
		REQUIRE(session.RunUntil(predicted, 30));
		session.Run(10);
		const float startX = hostPlayer.GetTransform().Translation.x;
		client.GetPhysicsWorld()->MoveCharacter(clientPlayer, { 2.0f, 0.0f, 0.0f });
		session.Run(60);
		CHECK(hostPlayer.GetTransform().Translation.x > startX + 1.0f);
	}

	TEST_CASE("Interpolation: fields a snapshot leaves out are held at their newest values, and a new parent mid-move starts afresh")
	{
		NetworkSettings settings = WithDelay(0.1f);
		settings.TickRate = 20.0f;
		Session session({}, settings);
		UUID moverId = 0;
		UUID anchorId = 0;
		Scene& host = session.StartHost([&](Scene& scene) {
			moverId = CreateNetworked(scene, "Mover").GetUUID();
			anchorId = CreateNetworked(scene, "Anchor", 0, { 10.0f, 0.0f, 0.0f }).GetUUID();
		});
		Scene& client = session.AddClient();
		REQUIRE(session.RunUntil([&] { return client.GetEntityByUUID(moverId) && client.GetEntityByUUID(anchorId); }, 60));
		session.Run(30);
		Entity hostMover = host.GetEntityByUUID(moverId);
		Entity clientMover = client.GetEntityByUUID(moverId);

		// It moves, then stops and only turns, while the client still shows it moving.
		for (int frame = 0; frame < 60; frame++)
		{
			if (frame < 30)
				hostMover.GetTransform().Translation.x += 3.0f * Step;
			else
				hostMover.GetTransform().Rotation = glm::angleAxis(0.05f * static_cast<float>(frame - 29), glm::vec3(0, 1, 0));
			session.Run(1);
		}
		session.Run(30);
		CHECK(clientMover.GetTransform().Translation.x == doctest::Approx(hostMover.GetTransform().Translation.x));
		CHECK(glm::abs(glm::dot(clientMover.GetTransform().Rotation, hostMover.GetTransform().Rotation)) > 0.99999f);

		// A new parent while moving: under it, the client never shows the older local offsets.
		std::optional<float> parentedAt;
		for (int frame = 0; frame < 60; frame++)
		{
			hostMover.GetTransform().Translation.x += 3.0f * Step;
			if (frame == 20)
			{
				host.SetParent(hostMover, host.GetEntityByUUID(anchorId), false);
				parentedAt = hostMover.GetTransform().Translation.x;
			}
			session.Run(1);
			if (parentedAt && clientMover.GetParent() && clientMover.GetParent().GetUUID() == anchorId)
				CHECK(clientMover.GetTransform().Translation.x >= *parentedAt - 1e-4f);
		}
		REQUIRE(clientMover.GetParent());
		CHECK(clientMover.GetParent().GetUUID() == anchorId);
	}
}
