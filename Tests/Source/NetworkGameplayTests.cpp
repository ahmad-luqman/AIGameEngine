#include <doctest/doctest.h>

#include <Breccia/Automation/AutomationSession.h>
#include <Breccia/Automation/CommandRegistry.h>
#include <Breccia/Core/FileSystem.h>
#include <Breccia/Network/NetBuffer.h>
#include <Breccia/Network/NetProtocol.h>
#include <Breccia/Network/NetworkSettings.h>
#include <Breccia/Network/NetworkSystem.h>
#include <Breccia/Network/ReplicationSchema.h>
#include <Breccia/Network/SimulatedNetwork.h>
#include <Breccia/Physics/PhysicsWorld.h>
#include <Breccia/Scene/Components.h>
#include <Breccia/Scene/Entity.h>
#include <Breccia/Scene/Scene.h>

#include "AutomationTestUtils.h"
#include "NetworkTestUtils.h"
#include "TestUtils.h"

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <span>
#include <limits>
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
		Scene& client = session.AddClient();
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
		hostEvents.clear();
		for (int i = 0; i < 30 && hostEvents.empty(); i++)
		{
			session.Link().Advance(Step);
			host.Receive(Step);
			hostEvents = host.TakeEvents();
		}
		REQUIRE(OfKind(hostEvents, NetworkEvent::Kind::PeerDisconnected).size() == 1);
		CHECK(OfKind(hostEvents, NetworkEvent::Kind::PeerDisconnected)[0].Peer == 2);
		CHECK(OfKind(hostEvents, NetworkEvent::Kind::PeerDisconnected)[0].Reason == NetDisconnectReason::ClientQuit);

		// The host stops: the client hears ServerShutdown.
		session.Host().StopNetwork();
		clientEvents.clear();
		for (int i = 0; i < 30 && clientEvents.empty(); i++)
		{
			session.Link().Advance(Step);
			network.Receive(Step);
			clientEvents = network.TakeEvents();
		}
		REQUIRE(OfKind(clientEvents, NetworkEvent::Kind::Disconnected).size() == 1);
		CHECK(OfKind(clientEvents, NetworkEvent::Kind::Disconnected)[0].Reason == NetDisconnectReason::ServerShutdown);
		CHECK(network.GetPeers().empty());
		(void)client;
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
		std::vector<NetworkEvent> received;
		for (int i = 0; i < 10 && received.empty(); i++)
		{
			session.Link().Advance(Step);
			host.Receive(Step);
			received = OfKind(host.TakeEvents(), NetworkEvent::Kind::Message);
		}
		REQUIRE(received.size() == 1);
		CHECK(received[0].Peer == 2);
		CHECK(received[0].Name == "Chat");
		CHECK(received[0].Arguments == nlohmann::json::array({ "hello", 3 }));

		REQUIRE_MESSAGE(host.SendScript(2, { NetScriptKind::Message, 0, "Welcome", { true } }, false, error), error);
		received.clear();
		for (int i = 0; i < 10 && received.empty(); i++)
		{
			session.Link().Advance(Step);
			client.Receive(Step);
			received = OfKind(client.TakeEvents(), NetworkEvent::Kind::Message);
		}
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
		SimulatedLinkSettings lossy{ .Latency = 0.02f };
		lossy.Loss = 1.0f;
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
		// Smooth: every frame moves, never by much more than the host's step (snapshots come every third frame).
		int changes = 0;
		for (size_t i = 31; i < shown.size(); i++)
		{
			const float delta = shown[i] - shown[i - 1];
			CHECK(delta >= -1e-5f);
			CHECK(delta <= 3.0f * Step * 1.6f);
			changes += delta > 1e-5f ? 1 : 0;
		}
		CHECK(changes >= 55);
		// Behind the host by about the delay (0.1 s at 3 m/s is 0.3 m), give or take a snapshot interval.
		const float lag = hostMover.GetTransform().Translation.x - clientMover.GetTransform().Translation.x;
		CHECK(lag > 0.2f);
		CHECK(lag < 0.6f);
		CHECK(session.ClientNetwork(0).GetViewTime() < session.ClientNetwork(0).GetTime());

		// Once the host stops, the client arrives exactly.
		session.Run(30);
		CHECK(clientMover.GetTransform().Translation.x == hostMover.GetTransform().Translation.x);

		// A local edit stops the interpolation until the next update.
		clientMover.GetTransform().Translation.y = 9.0f;
		hostMover.GetTransform().Translation.x += 1.0f;
		session.Run(1);
		session.Run(30);
		CHECK(clientMover.GetTransform().Translation.x == hostMover.GetTransform().Translation.x);
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
		// A second's worth of inputs at once (a speed hack): at most MaxInputLeadSeconds of steps apply now.
		uint32_t next = 1;
		const auto sendInputs = [&](uint32_t count, UUID entity) {
			NetState update;
			update.Sequence = next;
			update.Extras.Inputs.push_back({ entity, next, std::vector<CharacterInput>(count, { { 10.0f, 0.0f, 0.0f }, glm::quat(1, 0, 0, 0) }) });
			raw.Send(NetProtocol::EncodeState(NetMessageType::ClientUpdate, update), NetChannel::Unreliable);
			next += count;
		};
		const uint64_t before = network.GetStatistics().InputsApplied;
		sendInputs(NetProtocol::MaxInputsPerCharacter, playerId);
		session.Run(2);
		const uint64_t applied = network.GetStatistics().InputsApplied - before;
		const float lead = std::floor(NetworkSystem::MaxInputLeadSeconds / host.GetFixedTimestep());
		CHECK(applied >= 1);
		CHECK(static_cast<float>(applied) <= lead + 2.0f);
		CHECK(host.GetEntityByUUID(playerId).GetTransform().Translation.x > startX);
		// Inputs for another client's character, or a character not predicted, are refused.
		const uint64_t rejected = network.GetStatistics().RejectedRecords;
		const float otherX = host.GetEntityByUUID(otherId).GetTransform().Translation.x;
		sendInputs(4, otherId);
		session.Run(2);
		CHECK(network.GetStatistics().RejectedRecords > rejected);
		CHECK(host.GetEntityByUUID(otherId).GetTransform().Translation.x == otherX);
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
		CHECK(pace <= hostSteps + 2);
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
		// A client has no history: a plain raycast.
		hit = session.ClientNetwork(0).RaycastAt(then, { nowX, 5.0f, 0.0f }, down, 10.0f);
		(void)hit;
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
		CHECK(BrecciaTest::Eval(*client, "Network.Disconnect() return Network.IsActive()") == "false");
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
				Network.On("Tick", function(sender) Messages = Messages + 1 end)
			end
			function Count:OnUpdate(dt)
				if Network.IsClient() and Input.IsKeyDown("Space") then Network.Send("Server", "Tick") end
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
		Run(registry, session, "input.key", { { "key", "Space" }, { "down", false } });
		Run(registry, session, "play.step", { { "frames", 30 } });
		CHECK(Run(registry, session, "lua.exec", { { "code", "Messages" } })["result"] == "3");
		CHECK(Contains(RunError(registry, session, "net.focus", { { "client", 7 } }), "no in-process client"));

		// Ending a client tells the host; ending the play scene's session tells the rest.
		Run(registry, session, "net.disconnect", { { "client", 1 } });
		CHECK(Run(registry, session, "net.status")["inputPeer"] == 0);
		Run(registry, session, "play.step", { { "frames", 10 } });
		CHECK(Run(registry, session, "net.status")["connections"].size() == 1);
		CHECK(Contains(RunError(registry, session, "net.disconnect", { { "client", 1 } }), "no in-process client 1"));
		Run(registry, session, "net.disconnect");
		Run(registry, session, "play.step", { { "frames", 10 } });
		CHECK(Run(registry, session, "net.status", { { "client", 2 } })["disconnectReason"] == "ServerShutdown");
		CHECK(Run(registry, session, "net.status")["role"] == nullptr);
		CHECK(Contains(RunError(registry, session, "net.disconnect"), "no network session"));
		// Play's end takes the clients and the in-process network with it.
		Run(registry, session, "play.stop");
		CHECK(session.GetNetworkClients().empty());
		CHECK_FALSE(session.HasSimulatedNetwork());
	}
}
