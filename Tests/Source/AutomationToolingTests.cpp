#include <doctest/doctest.h>

#include <Basalt/Asset/ImageUtils.h>
#include <Basalt/Automation/AutomationConnection.h>
#include <Basalt/Automation/AutomationEvents.h>
#include <Basalt/Automation/AutomationServer.h>
#include <Basalt/Automation/AutomationSession.h>
#include <Basalt/Automation/CommandRegistry.h>
#include <Basalt/Core/JsonUtils.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Scene/Components.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>

#include "TestUtils.h"

#if defined(_WIN32)
	#include <winsock2.h>
	#include <ws2tcpip.h>
#else
	#include <arpa/inet.h>
	#include <netinet/in.h>
	#include <sys/socket.h>
	#include <sys/time.h>
	#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <set>
#include <thread>

using namespace Basalt;
using json = nlohmann::json;

namespace {

	json Run(const CommandRegistry& registry, AutomationSession& session, const std::string& command, const json& params = json::object())
	{
		const json response = registry.HandleRequest(session, { { "command", command }, { "params", params } });
		INFO(command << ": " << response.dump());
		REQUIRE(response["ok"] == true);
		return response["result"];
	}

	std::string RunError(const CommandRegistry& registry, AutomationSession& session, const std::string& command, const json& params = json::object())
	{
		const json response = registry.HandleRequest(session, { { "command", command }, { "params", params } });
		INFO(command << ": " << response.dump());
		REQUIRE(response["ok"] == false);
		return response["error"].get<std::string>();
	}

	bool Contains(const std::string& text, const std::string& part)
	{
		return text.find(part) != std::string::npos;
	}

	// A static ground, a trigger zone and a ball that falls through the zone onto the ground.
	void AddFallingBall(Scene& scene)
	{
		Entity ground = scene.CreateEntity("Ground");
		ground.GetTransform().Translation = { 0.0f, -0.5f, 0.0f };
		ground.AddComponent<RigidBodyComponent>();
		ground.AddComponent<BoxColliderComponent>().HalfExtents = { 10.0f, 0.5f, 10.0f };
		Entity zone = scene.CreateEntity("Zone");
		zone.GetTransform().Translation = { 0.0f, 2.0f, 0.0f };
		zone.AddComponent<RigidBodyComponent>().IsTrigger = true;
		zone.AddComponent<BoxColliderComponent>();
		Entity ball = scene.CreateEntity("Ball");
		ball.GetTransform().Translation = { 0.0f, 4.0f, 0.0f };
		ball.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		ball.AddComponent<SphereColliderComponent>();
	}

	std::vector<std::string> EventKinds(const json& events)
	{
		std::vector<std::string> kinds;
		kinds.reserve(events.size());
		for (const json& event : events)
			kinds.push_back(event["event"] == "collision" ? event["type"].get<std::string>() : event["event"].get<std::string>() + (event.contains("state") ? ":" + event["state"].get<std::string>() : ""));
		return kinds;
	}

	bool HasKind(const std::vector<std::string>& kinds, const std::string& kind)
	{
		return std::ranges::find(kinds, kind) != kinds.end();
	}

	// Writes a width x height PNG of one color.
	void WriteSolidPng(const std::filesystem::path& path, uint32_t width, uint32_t height, uint8_t red)
	{
		std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
		for (size_t i = 0; i < pixels.size(); i += 4)
		{
			pixels[i] = red;
			pixels[i + 1] = 40;
			pixels[i + 2] = 80;
			pixels[i + 3] = 255;
		}
		std::string error;
		REQUIRE_MESSAGE(WritePng(path, pixels, width, height, error), error);
	}

#if defined(_WIN32)
	using TestSocket = SOCKET;
	void CloseTestSocket(TestSocket socket)
	{
		closesocket(socket);
	}
#else
	using TestSocket = int;
	void CloseTestSocket(TestSocket socket)
	{
		close(socket);
	}
#endif

	// A client that keeps its connection open, for frames the server sends unprompted.
	class LineClient
	{
	public:
		explicit LineClient(uint16_t port)
			: m_Socket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP))
		{
			sockaddr_in address;
			std::memset(&address, 0, sizeof(address));
			address.sin_family = AF_INET;
			address.sin_port = htons(port);
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			REQUIRE(connect(m_Socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
			// Never hang the test run on a missing frame.
#if defined(_WIN32)
			DWORD timeout = 5000;
			setsockopt(m_Socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
			timeval timeout{ 5, 0 };
			setsockopt(m_Socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
		}
		~LineClient() { Close(); }
		LineClient(const LineClient&) = delete;
		LineClient& operator=(const LineClient&) = delete;

		void Send(const std::string& line)
		{
			const std::string payload = line + "\n";
			REQUIRE(send(m_Socket, payload.data(), static_cast<int>(payload.size()), 0) == static_cast<int>(payload.size()));
		}

		// The next frame that satisfies the predicate (skipping others, e.g. log lines), or null after 50 frames or a
		// timeout.
		template<typename Predicate>
		json ReadFrame(Predicate&& predicate)
		{
			for (int i = 0; i < 50; i++)
			{
				const std::string line = ReadLine();
				if (line.empty())
					return nullptr;
				json frame = ParseJson(line);
				if (predicate(frame))
					return frame;
			}
			return nullptr;
		}

		// The next line, or "" when none arrives in time.
		std::string ReadLine()
		{
			char buffer[4096];
			while (m_Buffer.find('\n') == std::string::npos)
			{
				const auto received = recv(m_Socket, buffer, sizeof(buffer), 0);
				if (received <= 0)
					return "";
				m_Buffer.append(buffer, static_cast<size_t>(received));
			}
			const size_t newline = m_Buffer.find('\n');
			std::string line = m_Buffer.substr(0, newline);
			m_Buffer.erase(0, newline + 1);
			return line;
		}

		void Close()
		{
			if (m_Closed)
				return;
			m_Closed = true;
			CloseTestSocket(m_Socket);
		}

	private:
		TestSocket m_Socket;
		std::string m_Buffer;
		bool m_Closed = false;
	};

}

TEST_SUITE("Automation")
{
	TEST_CASE("events.poll returns play-state changes, collisions and script errors in the order they happened")
	{
		BasaltTest::TempProject project("EventsPoll");
		const std::string script = project.WriteFile("Assets/Scripts/Faulty.lua", R"(
			local Faulty = {}
			function Faulty:OnUpdate(dt)
				if not self.Failed then self.Failed = true; error("boom") end
			end
			return Faulty
		)");
		CommandRegistry registry;
		AutomationSession session;
		Run(registry, session, "scene.new", { { "empty", true } });
		AddFallingBall(*session.GetEditScene());
		session.GetEditScene()->CreateEntity("Faulty").AddComponent<ScriptComponent>().Script = script;

		const json subscribed = Run(registry, session, "events.subscribe", { { "types", { "playState", "collision", "scriptError" } } });
		CHECK(subscribed["types"] == json({ "scriptError", "playState", "collision" }));
		CHECK(subscribed["streamed"] == false);
		const uint64_t all = subscribed["subscription"].get<uint64_t>();
		const uint64_t playOnly = Run(registry, session, "events.subscribe", { { "types", { "playState" } } })["subscription"].get<uint64_t>();
		CHECK(session.GetEvents()->GetSubscriptionCount() == 2);

		Run(registry, session, "play.start");
		Run(registry, session, "play.step", { { "frames", 150 } });
		Run(registry, session, "play.stop");

		const json polled = Run(registry, session, "events.poll", { { "subscription", all }, { "assertTypes", { "playState", "collision", "scriptError" } } });
		CHECK(polled["dropped"] == 0);
		const json& events = polled["events"];
		const std::vector<std::string> kinds = EventKinds(events);
		REQUIRE(kinds.size() >= 5);
		CHECK(kinds.front() == "playState:play");
		CHECK(kinds.back() == "playState:stop");
		CHECK(HasKind(kinds, "triggerEnter"));
		CHECK(HasKind(kinds, "triggerExit"));
		CHECK(HasKind(kinds, "collisionBegin"));
		for (size_t i = 1; i < events.size(); i++)
			CHECK(events[i]["seq"].get<uint64_t>() > events[i - 1]["seq"].get<uint64_t>());
		for (const json& event : events)
		{
			if (event["event"] == "scriptError")
			{
				CHECK(Contains(event["message"].get<std::string>(), "boom"));
				CHECK(event.contains("frame"));
			}
			if (event["event"] == "collision" && event["type"] == "collisionBegin")
			{
				// The pair is reported as scripts see it: the first entity has the lower UUID.
				const std::set<std::string> names = { event["a"]["name"].get<std::string>(), event["b"]["name"].get<std::string>() };
				CHECK(names == std::set<std::string>{ "Ball", "Ground" });
				CHECK(event["a"]["id"].get<uint64_t>() < event["b"]["id"].get<uint64_t>());
				CHECK(event["point"].size() == 3);
				CHECK(event["normal"].size() == 3);
				CHECK(event["impulse"].get<double>() > 0.0);
			}
		}

		// Polling drained the subscription; the other one kept its own copy of the play-state events only.
		CHECK(Run(registry, session, "events.poll", { { "subscription", all } })["events"].empty());
		CHECK(EventKinds(Run(registry, session, "events.poll", { { "subscription", playOnly } })["events"]) == std::vector<std::string>{ "playState:play", "playState:stop" });
		CHECK(Contains(RunError(registry, session, "events.poll", { { "subscription", playOnly }, { "assertTypes", { "collision" } } }), "assertion failed: no collision event among the 0 polled"));

		Run(registry, session, "events.unsubscribe", { { "subscription", playOnly } });
		CHECK(Contains(RunError(registry, session, "events.poll", { { "subscription", playOnly } }), "no event subscription"));
		CHECK(Contains(RunError(registry, session, "events.unsubscribe", { { "subscription", playOnly } }), "no event subscription"));
	}

	TEST_CASE("Simulate reports collisions to event subscriptions although no scripts run")
	{
		CommandRegistry registry;
		AutomationSession session;
		Run(registry, session, "scene.new", { { "empty", true } });
		AddFallingBall(*session.GetEditScene());
		const uint64_t id = Run(registry, session, "events.subscribe", { { "types", { "collision", "playState" } } })["subscription"].get<uint64_t>();
		std::string error;
		REQUIRE(session.StartPlay(error, true));
		session.Step(150, BasaltTest::Step);
		session.SetPaused(true);
		session.SetPaused(true);
		session.SetPaused(false);
		session.StopPlay();
		const std::vector<std::string> kinds = EventKinds(Run(registry, session, "events.poll", { { "subscription", id } })["events"]);
		CHECK(kinds.front() == "playState:simulate");
		CHECK(HasKind(kinds, "collisionBegin"));
		CHECK(HasKind(kinds, "triggerEnter"));
		// Pausing twice reports one change.
		CHECK(std::ranges::count(kinds, "playState:pause") == 1);
		CHECK(HasKind(kinds, "playState:resume"));
		CHECK(kinds.back() == "playState:stop");
	}

	TEST_CASE("Log event subscriptions filter by level and count events dropped beyond the queue limit")
	{
		CommandRegistry registry;
		AutomationSession session;
		const uint64_t errors = Run(registry, session, "events.subscribe", { { "types", { "log" } }, { "minLevel", "error" } })["subscription"].get<uint64_t>();
		const uint64_t everything = Run(registry, session, "events.subscribe", { { "types", { "log" } }, { "minLevel", "trace" } })["subscription"].get<uint64_t>();
		BS_CORE_WARN("events test: a warning");
		BS_CORE_ERROR("events test: an error");
		const json errorEvents = Run(registry, session, "events.poll", { { "subscription", errors } })["events"];
		REQUIRE(errorEvents.size() == 1);
		CHECK(errorEvents[0]["level"] == "error");
		CHECK(errorEvents[0]["text"] == "events test: an error");
		CHECK(errorEvents[0]["logger"] == "BASALT");
		CHECK(Run(registry, session, "events.poll", { { "subscription", everything }, { "max", 1 } })["events"][0]["text"] == "events test: a warning");

		const size_t extra = 5;
		for (size_t i = 0; i < AutomationEventHub::MaxQueuedEvents + extra; i++)
			BS_CORE_TRACE("events test: line {}", i);
		const json polled = Run(registry, session, "events.poll", { { "subscription", everything } });
		CHECK(polled["events"].size() == AutomationEventHub::MaxQueuedEvents);
		// The error left over from the first poll is the oldest event, so it is dropped first.
		CHECK(polled["dropped"] == extra + 1);
		CHECK(polled["events"].back()["text"] == "events test: line " + std::to_string(AutomationEventHub::MaxQueuedEvents + extra - 1));
		CHECK(Run(registry, session, "events.poll", { { "subscription", everything } })["dropped"] == 0);
	}

	TEST_CASE("Event subscriptions are limited and capture nothing nobody asked for")
	{
		CommandRegistry registry;
		AutomationSession session;
		CHECK_FALSE(session.GetEvents()->Wants(AutomationEventType::Log));
		std::vector<uint64_t> ids;
		ids.reserve(AutomationEventHub::MaxSubscriptions);
		for (size_t i = 0; i < AutomationEventHub::MaxSubscriptions; i++)
			ids.push_back(Run(registry, session, "events.subscribe", { { "types", { "playState" } } })["subscription"].get<uint64_t>());
		CHECK(session.GetEvents()->Wants(AutomationEventType::PlayState));
		CHECK_FALSE(session.GetEvents()->Wants(AutomationEventType::Collision));
		CHECK(Contains(RunError(registry, session, "events.subscribe"), "too many event subscriptions"));
		for (uint64_t id : ids)
			Run(registry, session, "events.unsubscribe", { { "subscription", id } });
		CHECK_FALSE(session.GetEvents()->Wants(AutomationEventType::PlayState));
		CHECK(Run(registry, session, "events.subscribe")["types"] == json({ "log", "scriptError", "playState", "collision" }));
		CHECK(Contains(RunError(registry, session, "events.subscribe", { { "types", { "frames" } } }), "types"));
	}

	TEST_CASE("events.subscribe streams to its TCP connection and ends with it")
	{
		CommandRegistry registry;
		AutomationSession session;
		Run(registry, session, "scene.new", { { "empty", true } });
		AutomationServer server;
		std::string error;
		// The test has no main thread to hand requests to: the server thread handles them directly.
		REQUIRE(server.StartWithFactory(0, [&]() {
			auto connection = std::make_shared<AutomationConnection>();
			return AutomationServer::ClientHandler{ [&, connection](const std::string& request) { return connection->HandleRequest(registry, session, request); }, [connection]() { return connection->TakePendingEvents(); } }; }, error));
		{
			LineClient client(server.GetPort());
			client.Send(R"({"id":1,"command":"events.subscribe","params":{"types":["playState","log"],"minLevel":"warn"}})");
			const json subscribed = ParseJson(client.ReadLine());
			REQUIRE(subscribed["ok"] == true);
			CHECK(subscribed["result"]["streamed"] == true);
			const uint64_t id = subscribed["result"]["subscription"].get<uint64_t>();

			client.Send(R"({"id":2,"command":"play.start"})");
			// The response comes before the events the request raised.
			CHECK(ParseJson(client.ReadLine())["id"] == 2);
			const json frame = client.ReadFrame([](const json& f) { return f.value("event", "") == "playState"; });
			REQUIRE(frame.is_object());
			CHECK(frame["state"] == "play");
			CHECK(frame["subscription"] == id);
			CHECK_FALSE(frame.contains("ok"));

			// Events raised between requests arrive on the idle tick, without a request.
			BS_CORE_WARN("streamed warning");
			const json warning = client.ReadFrame([](const json& f) { return f.value("text", "") == "streamed warning"; });
			REQUIRE(warning.is_object());
			CHECK(warning["event"] == "log");
			CHECK(warning["level"] == "warn");

			// Its events go to the connection, so another client cannot poll them away.
			const json poll = registry.HandleRequest(session, { { "command", "events.poll" }, { "params", { { "subscription", id } } } });
			CHECK(Contains(poll["error"].get<std::string>(), "streams to its connection"));
		}
		// Closing the connection ends its subscription.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (session.GetEvents()->GetSubscriptionCount() != 0 && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		CHECK(session.GetEvents()->GetSubscriptionCount() == 0);
		server.Stop();
		session.StopPlay();
	}

	TEST_CASE("scene.diff compares snapshots, scene files and the current scene by entity ID")
	{
		BasaltTest::TempProject project("SceneDiff");
		CommandRegistry registry;
		AutomationSession session;
		Run(registry, session, "scene.new", { { "name", "Diff" }, { "empty", true } });
		Run(registry, session, "entity.create", { { "name", "Kept" } });
		Run(registry, session, "entity.create", { { "name", "Gone" } });
		CHECK(Run(registry, session, "scene.snapshot", { { "name", "base" } }) == json({ { "name", "base" }, { "entities", 2 } }));
		Run(registry, session, "scene.save", { { "path", "Assets/Scenes/Base.bscene" } });
		CHECK(Run(registry, session, "scene.diff", { { "from", "base" } })["equal"] == true);
		CHECK(Run(registry, session, "scene.diff", { { "from", "Assets/Scenes/Base.bscene" }, { "to", "base" } })["equal"] == true);

		Run(registry, session, "entity.destroy", { { "entity", "Gone" } });
		Run(registry, session, "entity.create", { { "name", "New" } });
		Run(registry, session, "component.set", { { "entity", "Kept" }, { "component", "Transform" }, { "data", { { "Translation", { 1.0, 2.0, 3.0 } } } } });
		Run(registry, session, "component.set", { { "entity", "Kept" }, { "component", "PointLight" }, { "data", json::object() } });
		session.GetEditScene()->GetRendererSettings().Exposure = 2.0f;

		const json diff = Run(registry, session, "scene.diff", { { "from", "base" } });
		CHECK(diff["equal"] == false);
		REQUIRE(diff["removed"].size() == 1);
		CHECK(diff["removed"][0]["name"] == "Gone");
		REQUIRE(diff["added"].size() == 1);
		CHECK(diff["added"][0]["name"] == "New");
		REQUIRE(diff["settings"].size() == 1);
		CHECK(diff["settings"][0] == json({ { "path", "Renderer.Exposure" }, { "from", 1.0 }, { "to", 2.0 } }));
		REQUIRE(diff["changed"].size() == 1);
		const json& kept = diff["changed"][0];
		CHECK(kept["name"] == "Kept");
		std::map<std::string, json> changes;
		for (const json& change : kept["changes"])
			changes[change["path"].get<std::string>()] = change;
		REQUIRE(changes.contains("Transform.Translation"));
		CHECK(changes["Transform.Translation"]["from"] == json({ 0.0, 0.0, 0.0 }));
		CHECK(changes["Transform.Translation"]["to"] == json({ 1.0, 2.0, 3.0 }));
		REQUIRE(changes.contains("PointLight"));
		CHECK(changes["PointLight"]["from"].is_null());
		CHECK(changes["PointLight"]["to"].is_object());

		// The reverse diff swaps added and removed; a file reads like the snapshot it was saved from.
		Run(registry, session, "scene.snapshot", { { "name", "edited" } });
		const json reverse = Run(registry, session, "scene.diff", { { "from", "edited" }, { "to", "Assets/Scenes/Base.bscene" } });
		CHECK(reverse["added"][0]["name"] == "Gone");
		CHECK(reverse["removed"][0]["name"] == "New");

		const std::string differ = RunError(registry, session, "scene.diff", { { "from", "base" }, { "assertEqual", true } });
		CHECK(Contains(differ, "scenes differ:"));
		CHECK(Contains(differ, "removed 'Gone'"));
		CHECK(Contains(differ, "'Kept' Transform.Translation: [0.0,0.0,0.0] -> [1.0,2.0,3.0]"));
		CHECK(Contains(differ, "scene Renderer.Exposure: 1.0 -> 2.0"));

		// Tolerance hides float noise.
		Run(registry, session, "scene.snapshot", { { "name", "edited" } });
		Run(registry, session, "component.set", { { "entity", "Kept" }, { "component", "Transform" }, { "data", { { "Translation", { 1.0, 2.0, 3.0001 } } } } });
		CHECK(Run(registry, session, "scene.diff", { { "from", "edited" } })["changed"].size() == 1);
		CHECK(Run(registry, session, "scene.diff", { { "from", "edited" }, { "tolerance", 0.001 }, { "assertEqual", true } })["equal"] == true);

		CHECK(Contains(RunError(registry, session, "scene.diff", { { "from", "missing" } }), "no snapshot named 'missing'"));
		CHECK(Contains(RunError(registry, session, "scene.diff", { { "from", "Assets/Scenes/Missing.bscene" } }), "Missing.bscene"));
		CHECK(Contains(RunError(registry, session, "scene.diff", { { "from", "../Outside.bscene" } }), "'..'"));
		CHECK(Contains(RunError(registry, session, "scene.snapshot", { { "name", "Level.bscene" } }), "cannot end in '.bscene'"));
	}

	TEST_CASE("Snapshots survive play stop and show what playing changed")
	{
		CommandRegistry registry;
		AutomationSession session;
		Run(registry, session, "scene.new", { { "empty", true } });
		AddFallingBall(*session.GetEditScene());
		Run(registry, session, "scene.snapshot", { { "name", "edit" } });
		Run(registry, session, "play.start");
		Run(registry, session, "play.step", { { "frames", 60 } });
		Run(registry, session, "scene.snapshot", { { "name", "played" } });
		Run(registry, session, "play.stop");
		const json diff = Run(registry, session, "scene.diff", { { "from", "edit" }, { "to", "played" } });
		REQUIRE(diff["changed"].size() == 1);
		CHECK(diff["changed"][0]["name"] == "Ball");
		CHECK(Run(registry, session, "scene.diff", { { "from", "edit" } })["equal"] == true);
	}

	TEST_CASE("Snapshot names are limited")
	{
		CommandRegistry registry;
		AutomationSession session;
		for (size_t i = 0; i < AutomationSession::MaxSnapshots; i++)
			Run(registry, session, "scene.snapshot", { { "name", "s" + std::to_string(i) } });
		CHECK(Contains(RunError(registry, session, "scene.snapshot", { { "name", "one more" } }), "too many snapshots"));
		Run(registry, session, "scene.snapshot", { { "name", "s0" } });
	}

	TEST_CASE("assert.entity_exists checks names, IDs and counts")
	{
		CommandRegistry registry;
		AutomationSession session;
		Run(registry, session, "scene.new", { { "empty", true } });
		const uint64_t enemy = Run(registry, session, "entity.create", { { "name", "Enemy" } })["id"].get<uint64_t>();
		Run(registry, session, "entity.create", { { "name", "Enemy" } });

		const json found = Run(registry, session, "assert.entity_exists", { { "entity", "Enemy" } });
		CHECK(found["count"] == 2);
		CHECK(found["ids"].size() == 2);
		CHECK(Run(registry, session, "assert.entity_exists", { { "entity", enemy } })["ids"] == json({ enemy }));
		Run(registry, session, "assert.entity_exists", { { "entity", "Enemy" }, { "count", 2 } });
		Run(registry, session, "assert.entity_exists", { { "entity", "Boss" }, { "exists", false } });
		Run(registry, session, "assert.entity_exists", { { "entity", "Boss" }, { "count", 0 } });

		CHECK(RunError(registry, session, "assert.entity_exists", { { "entity", "Boss" } }) == "assertion failed: no entity named 'Boss'");
		CHECK(RunError(registry, session, "assert.entity_exists", { { "entity", 12345 } }) == "assertion failed: no entity with ID 12345");
		CHECK(RunError(registry, session, "assert.entity_exists", { { "entity", "Enemy" }, { "exists", false } }) == "assertion failed: an entity named 'Enemy' exists");
		CHECK(RunError(registry, session, "assert.entity_exists", { { "entity", "Enemy" }, { "count", 3 } }) == "assertion failed: 2 entities named 'Enemy', expected 3");
		CHECK(RunError(registry, session, "assert.entity_exists", { { "entity", enemy }, { "count", 0 } }) == "assertion failed: 1 entity with ID " + std::to_string(enemy) + ", expected 0");
		CHECK(Contains(RunError(registry, session, "assert.entity_exists", { { "entity", "Enemy" }, { "count", 2 }, { "exists", true } }), "either 'exists' or 'count'"));
	}

	TEST_CASE("assert.component compares the given fields within a tolerance and lists every mismatch")
	{
		CommandRegistry registry;
		AutomationSession session;
		Run(registry, session, "scene.new", { { "empty", true } });
		Run(registry, session, "entity.create", { { "name", "Door" } });
		Run(registry, session, "entity.create", { { "name", "Frame" } });
		Run(registry, session, "component.set", { { "entity", "Door" }, { "component", "Transform" }, { "data", { { "Translation", { 1.0, 2.0, 3.0 } } } } });
		Run(registry, session, "component.set", { { "entity", "Door" }, { "component", "RigidBody" }, { "data", { { "Type", "Dynamic" } } } });
		Run(registry, session, "component.set", { { "entity", "Door" }, { "component", "BoxCollider" }, { "data", json::object() } });
		Run(registry, session, "component.set", { { "entity", "Frame" }, { "component", "RigidBody" }, { "data", json::object() } });
		Run(registry, session, "component.set", { { "entity", "Frame" }, { "component", "BoxCollider" }, { "data", json::object() } });
		Run(registry, session, "component.set", { { "entity", "Door" }, { "component", "Joint" }, { "data", { { "ConnectedEntity", "Frame" } } } });

		const json transform = Run(registry, session, "assert.component", { { "entity", "Door" }, { "component", "Transform" }, { "fields", { { "Translation", { 1.0, 2.00001, 3.0 } } } } });
		CHECK(transform["Translation"] == json({ 1.0, 2.0, 3.0 }));
		Run(registry, session, "assert.component", { { "entity", "Door" }, { "component", "RigidBody" } });
		Run(registry, session, "assert.component", { { "entity", "Door" }, { "component", "Camera" }, { "exists", false } });
		// Entity references take names, like component.set.
		Run(registry, session, "assert.component", { { "entity", "Door" }, { "component", "Joint" }, { "fields", { { "ConnectedEntity", "Frame" } } } });

		const std::string mismatch = RunError(registry, session, "assert.component", { { "entity", "Door" }, { "component", "Transform" }, { "fields", { { "Translation", { 1.0, 2.1, 3.0 } }, { "Scale", { 2.0, 2.0, 2.0 } } } } });
		CHECK(Contains(mismatch, "assertion failed: 'Door' "));
		CHECK(Contains(mismatch, "Transform.Translation is [1.0,2.0,3.0], expected [1.0,2.1,3.0]"));
		CHECK(Contains(mismatch, "Transform.Scale is [1.0,1.0,1.0], expected [2.0,2.0,2.0]"));
		Run(registry, session, "assert.component", { { "entity", "Door" }, { "component", "Transform" }, { "fields", { { "Translation", { 1.0, 2.1, 3.0 } } } }, { "tolerance", 0.2 } });
		CHECK(RunError(registry, session, "assert.component", { { "entity", "Door" }, { "component", "RigidBody" }, { "fields", { { "Type", "Static" } } } }) == "assertion failed: 'Door' RigidBody.Type is \"Dynamic\", expected \"Static\"");
		CHECK(RunError(registry, session, "assert.component", { { "entity", "Frame" }, { "component", "Joint" } }) == "assertion failed: 'Frame' has no Joint component");
		CHECK(RunError(registry, session, "assert.component", { { "entity", "Door" }, { "component", "Joint" }, { "exists", false } }) == "assertion failed: 'Door' has a Joint component");
		CHECK(Contains(RunError(registry, session, "assert.component", { { "entity", "Door" }, { "component", "Transform" }, { "fields", { { "Position", { 0, 0, 0 } } } } }), "unknown field 'Position' for Transform (valid: Translation, Rotation, Scale)"));
		CHECK(Contains(RunError(registry, session, "assert.component", { { "entity", "Door" }, { "component", "Nope" } }), "unknown component 'Nope'"));
		CHECK(Contains(RunError(registry, session, "assert.component", { { "entity", "Door" }, { "component", "Joint" }, { "exists", false }, { "fields", json::object() } }), "'fields' cannot be checked with exists=false"));
	}

	TEST_CASE("render.compare renders through the host and compares with the reference")
	{
		BasaltTest::TempProject project("RenderCompare");
		CommandRegistry registry;
		AutomationSession session;
		CHECK(Contains(RunError(registry, session, "render.compare", { { "reference", "Reference.png" } }), "render.compare needs a GPU host"));

		uint8_t red = 200;
		int renders = 0;
		session.Screenshot = [&](const std::filesystem::path& path, uint32_t width, uint32_t height, std::string&) {
			renders++;
			WriteSolidPng(path, width, height, red);
			return true;
		};
		WriteSolidPng(project.GetDirectory() / "Reference.png", 64, 32, 200);
		CHECK(Contains(RunError(registry, session, "render.compare", { { "reference", "Missing.png" } }), "reference image not found"));
		CHECK(renders == 0);

		const json same = Run(registry, session, "render.compare", { { "reference", "Reference.png" }, { "width", 64 }, { "height", 32 }, { "assertMatch", true } });
		CHECK(renders == 1);
		CHECK(same["matches"] == true);
		CHECK(same["similarity"] == 1.0);
		CHECK(same["actual"] == "Cache/RenderCompare.png");
		CHECK(FileSystem::Exists(project.GetDirectory() / "Cache" / "RenderCompare.png"));

		red = 251;
		const json different = Run(registry, session, "render.compare", { { "reference", "Reference.png" }, { "width", 64 }, { "height", 32 }, { "output", "Out/Render.png" }, { "diff", "Out/Diff.png" } });
		CHECK(different["matches"] == false);
		CHECK(different["maxDelta"] == 51);
		CHECK(different["similarity"].get<double>() == doctest::Approx(1.0 - 51.0 / 255.0));
		CHECK(FileSystem::Exists(project.GetDirectory() / "Out" / "Render.png"));
		CHECK(FileSystem::Exists(project.GetDirectory() / "Out" / "Diff.png"));
		CHECK(Contains(RunError(registry, session, "render.compare", { { "reference", "Reference.png" }, { "width", 64 }, { "height", 32 }, { "assertMatch", true } }), "images differ"));

		// A size mismatch never matches and scores 0.
		const json resized = Run(registry, session, "render.compare", { { "reference", "Reference.png" } });
		CHECK(resized["sizeMatches"] == false);
		CHECK(resized["similarity"] == 0.0);

		session.Screenshot = [](const std::filesystem::path&, uint32_t, uint32_t, std::string& outError) {
			outError = "no camera";
			return false;
		};
		CHECK(RunError(registry, session, "render.compare", { { "reference", "Reference.png" } }) == "no camera");
	}

	TEST_CASE("component.types reports field types, enum values and entity fields")
	{
		CommandRegistry registry;
		AutomationSession session;
		const json components = Run(registry, session, "component.types")["components"];
		std::map<std::string, json> byName;
		for (const json& component : components)
		{
			CHECK(component["types"].size() == component["fields"].size());
			byName[component["name"].get<std::string>()] = component;
		}
		CHECK(byName["Transform"]["types"] == json({ { "Translation", "number[3]" }, { "Rotation", "number[3]" }, { "Scale", "number[3]" } }));
		CHECK(byName["RigidBody"]["types"]["Type"] == "enum");
		CHECK(byName["RigidBody"]["enums"]["Type"] == json({ "Static", "Dynamic", "Kinematic" }));
		CHECK(byName["RigidBody"]["types"]["Mass"] == "number");
		CHECK(byName["RigidBody"]["types"]["IsTrigger"] == "boolean");
		CHECK(byName["Camera"]["types"]["FOV"] == "number");
		CHECK(byName["Joint"]["types"]["ConnectedEntity"] == "entity");
		CHECK(byName["Joint"]["entityFields"].size() == 2);
		CHECK(byName["Script"]["types"]["Properties"] == "object");
		CHECK(byName["Tag"]["types"]["Tag"] == "string");
	}
}
