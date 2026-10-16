#include <doctest/doctest.h>

#include <Basalt/Asset/ImageUtils.h>
#include <Basalt/Automation/AutomationConnection.h>
#include <Basalt/Automation/AutomationEvents.h>
#include <Basalt/Automation/AutomationServer.h>
#include <Basalt/Automation/AutomationSession.h>
#include <Basalt/Automation/CommandRegistry.h>
#include <Basalt/Automation/McpServer.h>
#include <Basalt/Core/JsonUtils.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Components.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>
#include <Basalt/Scene/SceneSerializer.h>

#include "AutomationTestUtils.h"
#include "TestUtils.h"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <map>
#include <set>
#include <sstream>
#include <thread>

using namespace Basalt;
using json = nlohmann::json;
using BasaltTest::Contains;
using BasaltTest::Run;
using BasaltTest::RunError;

namespace {

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

	// "playState:play", "collisionBegin", "scriptError", ... per event.
	std::vector<std::string> EventKinds(const json& events)
	{
		std::vector<std::string> kinds;
		kinds.reserve(events.size());
		for (const json& event : events)
		{
			if (event["event"] == "collision")
				kinds.push_back(event["type"].get<std::string>());
			else if (event.contains("state"))
				kinds.push_back(event["event"].get<std::string>() + ":" + event["state"].get<std::string>());
			else
				kinds.push_back(event["event"].get<std::string>());
		}
		return kinds;
	}

	// Index of the first event of a kind, or the count when there is none.
	size_t IndexOf(const std::vector<std::string>& kinds, const std::string& kind)
	{
		return static_cast<size_t>(std::ranges::find(kinds, kind) - kinds.begin());
	}

	// Index of the last event of a kind, or the count when there is none.
	size_t LastIndexOf(const std::vector<std::string>& kinds, const std::string& kind)
	{
		const auto last = std::ranges::find(kinds.rbegin(), kinds.rend(), kind);
		return last == kinds.rend() ? kinds.size() : static_cast<size_t>(kinds.rend() - last - 1);
	}

	bool HasKind(const std::vector<std::string>& kinds, const std::string& kind)
	{
		return IndexOf(kinds, kind) < kinds.size();
	}

	uint64_t Subscribe(const CommandRegistry& registry, AutomationSession& session, const json& types, const char* minLevel = nullptr)
	{
		json params = { { "types", types } };
		if (minLevel)
			params["minLevel"] = minLevel;
		return Run(registry, session, "events.subscribe", params)["subscription"].get<uint64_t>();
	}

	json Poll(const CommandRegistry& registry, AutomationSession& session, uint64_t subscription)
	{
		return Run(registry, session, "events.poll", { { "subscription", subscription } });
	}

	// The texts of log events from this file's tests (other code may log meanwhile).
	std::vector<std::string> TestLogTexts(const json& events)
	{
		std::vector<std::string> texts;
		for (const json& event : events)
		{
			if (event.value("text", "").starts_with("events test:"))
				texts.push_back(event["text"].get<std::string>());
		}
		return texts;
	}

	std::vector<json> Lines(const std::string& frames)
	{
		std::vector<json> lines;
		std::istringstream stream(frames);
		std::string line;
		while (std::getline(stream, line))
			lines.push_back(ParseJson(line));
		return lines;
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

	json McpRequest(int id, const std::string& method, const json& params = json::object())
	{
		return { { "jsonrpc", "2.0" }, { "id", id }, { "method", method }, { "params", params } };
	}

	// The structured result of an MCP tools/call through a connection.
	json McpCall(AutomationConnection& connection, const CommandRegistry& registry, AutomationSession& session, int id, const std::string& tool, const json& arguments)
	{
		const json response = ParseJson(connection.HandleRequest(registry, session, McpRequest(id, "tools/call", { { "name", tool }, { "arguments", arguments } }).dump()));
		INFO(tool << ": " << response.dump());
		REQUIRE(response.contains("result"));
		REQUIRE(response["result"]["isError"] == false);
		return response["result"]["structuredContent"];
	}

	void InitializeMcp(AutomationConnection& connection, const CommandRegistry& registry, AutomationSession& session)
	{
		const json initialize = McpRequest(1, "initialize", { { "protocolVersion", McpServer::ProtocolVersion }, { "capabilities", json::object() }, { "clientInfo", { { "name", "test" }, { "version", "1" } } } });
		REQUIRE(ParseJson(connection.HandleRequest(registry, session, initialize.dump())).contains("result"));
		connection.HandleRequest(registry, session, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
	}

}

TEST_SUITE("Automation")
{
	TEST_CASE("events.poll returns play-state changes, collisions and script errors in the order they happened")
	{
		BasaltTest::TempProject project("EventsPoll");
		const std::string faulty = project.WriteFile("Assets/Scripts/Faulty.lua", R"(
			local Faulty = {}
			function Faulty:OnUpdate(dt)
				if not self.Failed then self.Failed = true; error("update boom") end
			end
			return Faulty
		)");
		const std::string creates = project.WriteFile("Assets/Scripts/FailsOnCreate.lua", R"(
			local FailsOnCreate = {}
			function FailsOnCreate:OnCreate() error("create boom") end
			return FailsOnCreate
		)");
		// A separate script: an instance whose OnCreate fails never gets OnDestroy.
		const std::string destroys = project.WriteFile("Assets/Scripts/FailsOnDestroy.lua", R"(
			local FailsOnDestroy = {}
			function FailsOnDestroy:OnDestroy() error("destroy boom") end
			return FailsOnDestroy
		)");
		CommandRegistry registry;
		AutomationSession session;
		Run(registry, session, "scene.new", { { "empty", true } });
		AddFallingBall(*session.GetEditScene());
		session.GetEditScene()->CreateEntity("Faulty").AddComponent<ScriptComponent>().Script = faulty;
		session.GetEditScene()->CreateEntity("FailsOnCreate").AddComponent<ScriptComponent>().Script = creates;
		session.GetEditScene()->CreateEntity("FailsOnDestroy").AddComponent<ScriptComponent>().Script = destroys;

		const json subscribed = Run(registry, session, "events.subscribe", { { "types", { "playState", "collision", "scriptError" } } });
		CHECK(subscribed["types"] == json({ "scriptError", "playState", "collision" }));
		CHECK(subscribed["streamed"] == false);
		const uint64_t all = subscribed["subscription"].get<uint64_t>();
		const uint64_t playOnly = Subscribe(registry, session, { "playState" });
		CHECK(session.GetEvents()->GetSubscriptionCount() == 2);

		Run(registry, session, "play.start");
		Run(registry, session, "play.step", { { "frames", 150 } });
		Run(registry, session, "play.stop");

		const json polled = Run(registry, session, "events.poll", { { "subscription", all }, { "assertTypes", { "playState", "collision", "scriptError" } } });
		CHECK(polled["dropped"] == 0);
		const json& events = polled["events"];
		const std::vector<std::string> kinds = EventKinds(events);
		REQUIRE(kinds.size() >= 7);
		CHECK(kinds.front() == "playState:play");
		CHECK(kinds.back() == "playState:stop");
		// The ball enters and leaves the zone before it lands.
		CHECK(IndexOf(kinds, "triggerEnter") < IndexOf(kinds, "triggerExit"));
		CHECK(IndexOf(kinds, "triggerExit") < IndexOf(kinds, "collisionBegin"));
		CHECK(HasKind(kinds, "collisionBegin"));
		for (size_t i = 1; i < events.size(); i++)
			CHECK(events[i]["seq"].get<uint64_t>() > events[i - 1]["seq"].get<uint64_t>());

		std::vector<std::string> errors;
		for (const json& event : events)
		{
			if (event["event"] == "scriptError")
			{
				const std::string message = event["message"].get<std::string>();
				if (Contains(message, "create boom"))
					errors.emplace_back("create");
				else if (Contains(message, "destroy boom"))
					errors.emplace_back("destroy");
				else if (Contains(message, "update boom"))
					errors.emplace_back("update");
				else
					errors.push_back(message);
				CHECK(event.contains("frame"));
			}
			if (event["event"] == "collision" && event["type"] == "collisionBegin")
			{
				// Ordered as the physics world keys the pair: lower UUID first.
				const std::set<std::string> names = { event["a"]["name"].get<std::string>(), event["b"]["name"].get<std::string>() };
				CHECK(names == std::set<std::string>{ "Ball", "Ground" });
				CHECK(event["a"]["id"].get<uint64_t>() < event["b"]["id"].get<uint64_t>());
				CHECK(event["point"].size() == 3);
				CHECK(event["normal"].size() == 3);
				CHECK(event["impulse"].get<double>() > 0.0);
			}
		}
		// OnCreate's error comes after the play event, OnDestroy's (raised while stopping) before the stop event.
		CHECK(errors == std::vector<std::string>{ "create", "update", "destroy" });
		CHECK(IndexOf(kinds, "scriptError") == 1);
		// Between the OnDestroy error and the stop event only the contacts the removed bodies end are reported.
		for (size_t i = LastIndexOf(kinds, "scriptError") + 1; i + 1 < kinds.size(); i++)
			CHECK_MESSAGE((kinds[i] == "collisionEnd" || kinds[i] == "triggerExit"), kinds[i]);

		// Polling drained the subscription; the other one kept its own copy of the play-state events only.
		CHECK(Poll(registry, session, all)["events"].empty());
		CHECK(EventKinds(Poll(registry, session, playOnly)["events"]) == std::vector<std::string>{ "playState:play", "playState:stop" });

		Run(registry, session, "events.unsubscribe", { { "subscription", playOnly } });
		CHECK(RunError(registry, session, "events.poll", { { "subscription", playOnly } }) == "no event subscription " + std::to_string(playOnly));
		CHECK(RunError(registry, session, "events.unsubscribe", { { "subscription", playOnly } }) == "no event subscription " + std::to_string(playOnly));
	}

	TEST_CASE("events.poll with assertTypes keeps the events when they lack a type")
	{
		CommandRegistry registry;
		AutomationSession session;
		const uint64_t id = Subscribe(registry, session, { "playState", "collision" });
		Run(registry, session, "play.start");
		Run(registry, session, "play.stop");
		const std::string error = RunError(registry, session, "events.poll", { { "subscription", id }, { "assertTypes", { "playState", "collision" } } });
		CHECK(error == "assertion failed: no collision event among the 2 queued (2 playState; 0 dropped); they stay queued");
		// A later poll still gets them.
		CHECK(EventKinds(Poll(registry, session, id)["events"]) == std::vector<std::string>{ "playState:play", "playState:stop" });
		CHECK(RunError(registry, session, "events.poll", { { "subscription", id }, { "assertTypes", { "playState" } } }) == "assertion failed: no playState event among the 0 queued (none; 0 dropped); they stay queued");
		Run(registry, session, "play.start");
		Run(registry, session, "play.stop");
		// Only the first 'max' events count.
		CHECK(RunError(registry, session, "events.poll", { { "subscription", id }, { "max", 1 }, { "assertTypes", { "collision" } } }) == "assertion failed: no collision event among the first 1 queued (1 playState; 0 dropped); they stay queued");
	}

	TEST_CASE("events.poll with expect matches event patterns, naming a contact's bodies in either order")
	{
		CommandRegistry registry;
		AutomationSession session;
		Run(registry, session, "scene.new", { { "empty", true } });
		AddFallingBall(*session.GetEditScene());
		const json types = { "collision", "playState" };
		const uint64_t raw = Subscribe(registry, session, types);
		const uint64_t listed = Subscribe(registry, session, types);
		const uint64_t swapped = Subscribe(registry, session, types);
		const uint64_t unswappedNormal = Subscribe(registry, session, types);
		const uint64_t missing = Subscribe(registry, session, types);
		Run(registry, session, "play.start");
		Run(registry, session, "play.step", { { "frames", 150 } });
		Run(registry, session, "play.stop");

		json landing;
		const json rawEvents = Poll(registry, session, raw)["events"];
		for (const json& event : rawEvents)
		{
			if (event["event"] == "collision" && event["type"] == "collisionBegin")
				landing = event;
		}
		REQUIRE(landing.is_object());
		const json& normal = landing["normal"];
		REQUIRE(std::abs(normal[1].get<double>()) > 0.5);

		// Patterns name a subset of the fields; floats match within a tolerance.
		const json asListed = { { "event", "collision" }, { "type", "collisionBegin" }, { "a", { { "name", landing["a"]["name"] } } }, { "b", { { "name", landing["b"]["name"] } } }, { "normal", normal } };
		const json stopped = { { "event", "playState" }, { "state", "stop" } };
		const json polled = Run(registry, session, "events.poll", { { "subscription", listed }, { "expect", { asListed, stopped } } });
		CHECK(polled["events"].size() > 2);

		// The bodies swapped: the normal (from a to b) must be reversed too.
		json reversedNormal = json::array();
		for (const json& component : normal)
			reversedNormal.push_back(-component.get<double>());
		json bodiesSwapped = { { "event", "collision" }, { "type", "collisionBegin" }, { "a", { { "name", landing["b"]["name"] } } }, { "b", { { "name", landing["a"]["name"] } } }, { "normal", reversedNormal } };
		Run(registry, session, "events.poll", { { "subscription", swapped }, { "expect", { bodiesSwapped } } });
		bodiesSwapped["normal"] = normal;
		CHECK(Contains(RunError(registry, session, "events.poll", { { "subscription", unswappedNormal }, { "expect", { bodiesSwapped } } }), "assertion failed: no event matches"));

		// A pattern nothing matches fails, names the pattern and keeps the events.
		const json goal = { { "event", "collision" }, { "type", "triggerEnter" }, { "a", { { "name", "Goal" } } } };
		const std::string error = RunError(registry, session, "events.poll", { { "subscription", missing }, { "expect", { stopped, goal } } });
		CHECK(Contains(error, "assertion failed: no event matches " + goal.dump() + " among the "));
		CHECK(Contains(error, "they stay queued"));
		// Only the first 'max' events count: the stop event comes last.
		CHECK(Contains(RunError(registry, session, "events.poll", { { "subscription", missing }, { "max", 1 }, { "expect", { stopped } } }), "no event matches " + stopped.dump() + " among the first 1 queued (1 playState; 0 dropped)"));
		// A missing type is reported before a pattern.
		CHECK(Contains(RunError(registry, session, "events.poll", { { "subscription", missing }, { "assertTypes", { "log" } }, { "expect", { goal } } }), "assertion failed: no log event among the "));
		// Patterns are objects, at least one.
		CHECK(Contains(RunError(registry, session, "events.poll", { { "subscription", missing }, { "expect", { "triggerEnter" } } }), "expect[0]"));
		CHECK(Contains(RunError(registry, session, "events.poll", { { "subscription", missing }, { "expect", json::array() } }), "expect"));
		const json all = Poll(registry, session, missing);
		CHECK(all["events"].size() == polled["events"].size());
		CHECK(all["events"].back()["state"] == "stop");
	}

	TEST_CASE("Simulate reports collisions to event subscriptions although no scripts run")
	{
		CommandRegistry registry;
		AutomationSession session;
		Run(registry, session, "scene.new", { { "empty", true } });
		AddFallingBall(*session.GetEditScene());
		const uint64_t id = Subscribe(registry, session, { "collision", "playState" });
		// Pausing outside a session reports nothing.
		session.SetPaused(true);
		std::string error;
		REQUIRE(session.StartPlay(error, true));
		session.Step(150, BasaltTest::Step);
		session.SetPaused(true);
		session.SetPaused(true);
		session.SetPaused(false);
		session.SetPaused(true);
		// Stopping while paused reports only the stop.
		session.StopPlay();
		const std::vector<std::string> kinds = EventKinds(Poll(registry, session, id)["events"]);
		REQUIRE_FALSE(kinds.empty());
		CHECK(kinds.front() == "playState:simulate");
		CHECK(HasKind(kinds, "collisionBegin"));
		CHECK(HasKind(kinds, "triggerEnter"));
		CHECK(HasKind(kinds, "triggerExit"));
		// Pausing an already paused scene reports nothing.
		CHECK(std::ranges::count(kinds, "playState:pause") == 2);
		CHECK(std::ranges::count(kinds, "playState:resume") == 1);
		CHECK(kinds.back() == "playState:stop");
	}

	TEST_CASE("Destroying an entity in contact reports the contact's end")
	{
		CommandRegistry registry;
		AutomationSession session;
		Run(registry, session, "scene.new", { { "empty", true } });
		AddFallingBall(*session.GetEditScene());
		const uint64_t id = Subscribe(registry, session, { "collision" });
		Run(registry, session, "play.start");
		Run(registry, session, "play.step", { { "frames", 150 } });
		REQUIRE(HasKind(EventKinds(Poll(registry, session, id)["events"]), "collisionBegin"));
		Run(registry, session, "entity.destroy", { { "entity", "Ball" } });
		Run(registry, session, "play.step");
		const json events = Poll(registry, session, id)["events"];
		REQUIRE(events.size() == 1);
		CHECK(events[0]["type"] == "collisionEnd");
		const std::set<std::string> names = { events[0]["a"]["name"].get<std::string>(), events[0]["b"]["name"].get<std::string>() };
		CHECK(names == std::set<std::string>{ "Ball", "Ground" });
		// A body that is gone has no separation point.
		CHECK_FALSE(events[0].contains("point"));
		Run(registry, session, "play.stop");
	}

	TEST_CASE("Event subscriptions keep reporting after Lua Scene.Load switches the playing scene")
	{
		BasaltTest::TempProject project("EventsSceneLoad");
		Scene next("Next");
		AddFallingBall(next);
		std::string error;
		REQUIRE(SceneSerializer::SaveScene(next, project.GetDirectory() / "Assets/Scenes/Next.bscene", error));
		const std::string loader = project.WriteFile("Assets/Scripts/Loader.lua", R"(
			local Loader = {}
			function Loader:OnUpdate(dt)
				if not self.Requested then self.Requested = true; Scene.Load("Assets/Scenes/Next.bscene") end
			end
			return Loader
		)");
		CommandRegistry registry;
		AutomationSession session;
		Run(registry, session, "scene.new", { { "empty", true } });
		session.GetEditScene()->CreateEntity("Loader").AddComponent<ScriptComponent>().Script = loader;
		const uint64_t id = Subscribe(registry, session, { "collision", "scriptError" });
		Run(registry, session, "play.start");
		const json step = Run(registry, session, "play.step", { { "frames", 150 } });
		CHECK(step["scene"] == "Next");
		const std::vector<std::string> kinds = EventKinds(Poll(registry, session, id)["events"]);
		CHECK(HasKind(kinds, "collisionBegin"));
		CHECK_FALSE(HasKind(kinds, "scriptError"));
		Run(registry, session, "play.stop");
	}

	TEST_CASE("Log event subscriptions filter by level and count events dropped beyond the queue limit")
	{
		CommandRegistry registry;
		AutomationSession session;
		const uint64_t errors = Subscribe(registry, session, { "log" }, "error");
		const uint64_t everything = Subscribe(registry, session, { "log" }, "trace");
		BS_CORE_WARN("events test: a warning");
		BS_CORE_ERROR("events test: an error");
		const json errorEvents = Poll(registry, session, errors)["events"];
		CHECK(TestLogTexts(errorEvents) == std::vector<std::string>{ "events test: an error" });
		for (const json& event : errorEvents)
		{
			CHECK((event["level"] == "error" || event["level"] == "critical"));
			if (event["text"] == "events test: an error")
				CHECK(event["logger"] == "BASALT");
		}
		CHECK(TestLogTexts(Poll(registry, session, everything)["events"]) == std::vector<std::string>{ "events test: a warning", "events test: an error" });

		const size_t extra = 5;
		for (size_t i = 0; i < AutomationEventHub::MaxQueuedEvents + extra; i++)
			BS_CORE_TRACE("events test: line {}", i);
		const json polled = Poll(registry, session, everything);
		CHECK(polled["events"].size() == AutomationEventHub::MaxQueuedEvents);
		CHECK(polled["dropped"].get<uint64_t>() >= extra);
		const std::vector<std::string> lines = TestLogTexts(polled["events"]);
		REQUIRE_FALSE(lines.empty());
		CHECK(lines.back() == "events test: line " + std::to_string(AutomationEventHub::MaxQueuedEvents + extra - 1));
		CHECK(Poll(registry, session, everything)["dropped"] == 0);
	}

	TEST_CASE("Event subscriptions are limited and capture nothing nobody asked for")
	{
		CommandRegistry registry;
		AutomationSession session;
		CHECK_FALSE(session.GetEvents()->Wants(AutomationEventType::Log));
		std::vector<uint64_t> ids;
		ids.reserve(AutomationEventHub::MaxSubscriptions);
		for (size_t i = 0; i < AutomationEventHub::MaxSubscriptions; i++)
			ids.push_back(Subscribe(registry, session, { "playState" }));
		CHECK(session.GetEvents()->Wants(AutomationEventType::PlayState));
		CHECK_FALSE(session.GetEvents()->Wants(AutomationEventType::Collision));
		CHECK(Contains(RunError(registry, session, "events.subscribe"), "too many event subscriptions (at most 64)"));
		for (uint64_t id : ids)
			Run(registry, session, "events.unsubscribe", { { "subscription", id } });
		CHECK_FALSE(session.GetEvents()->Wants(AutomationEventType::PlayState));
		CHECK(Run(registry, session, "events.subscribe")["types"] == json({ "log", "scriptError", "playState", "collision" }));
		CHECK(Contains(RunError(registry, session, "events.subscribe", { { "types", { "frames" } } }), "parameter 'types[0]' must be one of"));
	}

	TEST_CASE("The event hub streams frames per owner, reports drops and keeps frames valid JSON")
	{
		AutomationEventHub hub;
		const uint64_t streamedA = *hub.Subscribe({}, 7, AutomationEventHub::Delivery::Streamed);
		const uint64_t polledA = *hub.Subscribe({}, 7, AutomationEventHub::Delivery::Polled);
		const uint64_t streamedB = *hub.Subscribe({}, 8, AutomationEventHub::Delivery::Streamed);
		const uint64_t unowned = *hub.Subscribe({}, 0, AutomationEventHub::Delivery::Polled);
		CHECK(hub.TakeStreamed(7).empty());

		hub.Publish(AutomationEventType::ScriptError, { { "message", "bad \xff byte" } });
		const std::vector<json> frames = Lines(hub.TakeStreamed(7));
		REQUIRE(frames.size() == 1);
		REQUIRE_FALSE(frames[0].is_discarded());
		CHECK(frames[0]["subscription"] == streamedA);
		CHECK(frames[0]["message"] == "bad \xEF\xBF\xBD byte");
		CHECK(hub.TakeStreamed(7).empty());
		// Owner 0 streams nothing and leaves polled subscriptions alone.
		CHECK(hub.TakeStreamed(0).empty());
		std::vector<json> events;
		uint64_t dropped = 0;
		REQUIRE(hub.Poll(polledA, 10, events, dropped) == AutomationEventHub::PollStatus::Ok);
		CHECK(events.size() == 1);
		CHECK(hub.Poll(streamedA, 10, events, dropped) == AutomationEventHub::PollStatus::Streamed);

		// Past the queue limit the oldest events go, and the stream says how many first.
		const size_t extra = 6;
		for (size_t i = 0; i < AutomationEventHub::MaxQueuedEvents + extra; i++)
			hub.Publish(AutomationEventType::PlayState, { { "state", "play" }, { "index", i } });
		const std::vector<json> burst = Lines(hub.TakeStreamed(8));
		REQUIRE(burst.size() == AutomationEventHub::MaxQueuedEvents + 1);
		CHECK(burst[0] == json({ { "event", "dropped" }, { "subscription", streamedB }, { "count", extra + 1 } }));
		CHECK(burst[1]["index"] == extra);
		CHECK(burst.back()["index"] == AutomationEventHub::MaxQueuedEvents + extra - 1);
		CHECK(hub.TakeStreamed(8).empty());

		// An owner's subscriptions end with it; others stay.
		hub.RemoveOwner(0);
		CHECK(hub.GetSubscriptionCount() == 4);
		hub.RemoveOwner(7);
		CHECK(hub.GetSubscriptionCount() == 2);
		CHECK(hub.Poll(unowned, 1, events, dropped) == AutomationEventHub::PollStatus::Ok);
		CHECK(hub.Unsubscribe(streamedB));
		// An ended stream sends nothing, even what was queued.
		hub.Publish(AutomationEventType::PlayState, { { "state", "stop" } });
		CHECK(hub.TakeStreamed(8).empty());
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
			BasaltTest::LineClient client(server.GetPort());
			REQUIRE(client.Send(R"({"id":1,"command":"events.subscribe","params":{"types":["playState","log"],"minLevel":"warn"}})"));
			const json subscribed = ParseJson(client.ReadLine());
			REQUIRE(subscribed["ok"] == true);
			CHECK(subscribed["result"]["streamed"] == true);
			const uint64_t id = subscribed["result"]["subscription"].get<uint64_t>();

			REQUIRE(client.Send(R"({"id":2,"command":"play.start"})"));
			// The response comes before the events the request raised.
			CHECK(ParseJson(client.ReadLine())["id"] == 2);
			const json frame = client.ReadFrame([](const json& f) { return f.value("event", "") == "playState"; });
			REQUIRE(frame.is_object());
			CHECK(frame["state"] == "play");
			CHECK(frame["subscription"] == id);
			CHECK_FALSE(frame.contains("ok"));

			// Events raised between requests arrive on the idle tick, without a request.
			BS_CORE_WARN("events test: streamed warning");
			const json warning = client.ReadFrame([](const json& f) { return f.value("text", "") == "events test: streamed warning"; });
			REQUIRE(warning.is_object());
			CHECK(warning["event"] == "log");
			CHECK(warning["level"] == "warn");

			// Its events go to the connection, so another client cannot poll them away. Safe from this thread: the
			// server thread is idle and only the (thread-safe) hub is touched.
			const json poll = registry.HandleRequest(session, { { "command", "events.poll" }, { "params", { { "subscription", id } } } });
			CHECK(Contains(poll["error"].get<std::string>(), "streams to its connection"));

			// After unsubscribing, nothing more arrives on the connection: each response is the next line.
			REQUIRE(client.Send(R"({"id":3,"command":"events.unsubscribe","params":{"subscription":)" + std::to_string(id) + "}}"));
			CHECK(ParseJson(client.ReadLine())["id"] == 3);
			REQUIRE(client.Send(R"({"id":4,"command":"play.stop"})"));
			CHECK(ParseJson(client.ReadLine())["id"] == 4);
			BS_CORE_WARN("events test: after unsubscribing");
			REQUIRE(client.Send(R"({"id":5,"command":"scene.info"})"));
			CHECK(ParseJson(client.ReadLine())["id"] == 5);
			REQUIRE(client.Send(R"({"id":6,"command":"events.subscribe","params":{"types":["playState"]}})"));
			CHECK(ParseJson(client.ReadLine())["id"] == 6);
		}
		// Closing the connection ends its subscription.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (session.GetEvents()->GetSubscriptionCount() != 0 && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		CHECK(session.GetEvents()->GetSubscriptionCount() == 0);
		server.Stop();
	}

	TEST_CASE("A connection attributes subscriptions only while it handles a request, and ends them when it goes")
	{
		CommandRegistry registry;
		AutomationSession session;
		{
			AutomationConnection connection;
			const json response = ParseJson(connection.HandleRequest(registry, session, R"({"command":"events.subscribe","params":{"types":["playState"]}})"));
			CHECK(response["result"]["streamed"] == true);
			// Back to no client: commands the host runs itself get polled subscriptions that do not end with it.
			CHECK(session.GetEventClient().Owner == 0);
			CHECK(Run(registry, session, "events.subscribe")["streamed"] == false);
			CHECK(session.GetEvents()->GetSubscriptionCount() == 2);
		}
		CHECK(session.GetEvents()->GetSubscriptionCount() == 1);
	}

	TEST_CASE("MCP clients get polled subscriptions that end with their connection")
	{
		CommandRegistry registry;
		AutomationSession session;
		{
			AutomationConnection connection;
			InitializeMcp(connection, registry, session);
			const json subscribed = McpCall(connection, registry, session, 2, "events.subscribe", { { "types", { "playState" } } });
			CHECK(subscribed["streamed"] == false);
			McpCall(connection, registry, session, 3, "play.start", json::object());
			// Nothing streams to an MCP connection; the event waits for events.poll.
			CHECK(connection.TakePendingEvents().empty());
			const json polled = McpCall(connection, registry, session, 4, "events.poll", { { "subscription", subscribed["subscription"] } });
			CHECK(EventKinds(polled["events"]) == std::vector<std::string>{ "playState:play" });
			CHECK(session.GetEventClient().Owner == 0);
			CHECK(session.GetEvents()->GetSubscriptionCount() == 1);
		}
		// A client that reconnects every time cannot use up the subscription limit.
		CHECK(session.GetEvents()->GetSubscriptionCount() == 0);
		session.StopPlay();
	}

	TEST_CASE("A connection that moves to another session ends its subscriptions in the first one")
	{
		CommandRegistry registry;
		AutomationSession first;
		AutomationSession second;
		AutomationConnection connection;
		connection.HandleRequest(registry, first, R"({"command":"events.subscribe","params":{"types":["playState"]}})");
		CHECK(first.GetEvents()->GetSubscriptionCount() == 1);
		connection.HandleRequest(registry, second, R"({"command":"events.subscribe","params":{"types":["playState"]}})");
		CHECK(first.GetEvents()->GetSubscriptionCount() == 0);
		connection.HandleRequest(registry, second, R"({"command":"play.start"})");
		const std::vector<json> frames = Lines(connection.TakePendingEvents());
		REQUIRE(frames.size() == 1);
		CHECK(frames[0]["state"] == "play");
		second.StopPlay();
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
		CHECK(diff["reordered"] == false);
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
		Run(registry, session, "component.set", { { "entity", "Kept" }, { "component", "Transform" }, { "data", { { "Translation", { 1.0, 2.0, 3.0001 } } } } });
		CHECK(Run(registry, session, "scene.diff", { { "from", "edited" } })["changed"].size() == 1);
		CHECK(Run(registry, session, "scene.diff", { { "from", "edited" }, { "tolerance", 0.001 }, { "assertEqual", true } })["equal"] == true);

		// Moving an entity changes the order the scene lists (and draws) it in.
		Run(registry, session, "scene.snapshot", { { "name", "ordered" } });
		Run(registry, session, "entity.set_parent", { { "entity", "Kept" }, { "parent", "New" } });
		Run(registry, session, "entity.set_parent", { { "entity", "Kept" }, { "parent", nullptr } });
		const json reordered = Run(registry, session, "scene.diff", { { "from", "ordered" } });
		CHECK(reordered["reordered"] == true);
		CHECK(reordered["equal"] == false);
		CHECK(Contains(RunError(registry, session, "scene.diff", { { "from", "ordered" }, { "assertEqual", true } }), "the entities are in a different order"));

		CHECK(Contains(RunError(registry, session, "scene.diff", { { "from", "missing" } }), "no snapshot named 'missing'"));
		CHECK(Contains(RunError(registry, session, "scene.diff", { { "from", "Assets/Scenes/Missing.bscene" } }), "Missing.bscene"));
		project.WriteFile("Assets/Scenes/Broken.bscene", "{ not a scene");
		CHECK(Contains(RunError(registry, session, "scene.diff", { { "from", "Assets/Scenes/Broken.bscene" } }), "Broken.bscene"));
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
		CHECK(RunError(registry, session, "scene.snapshot", { { "name", "one more" } }) == "too many snapshots (at most 32); reuse a name");
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
		CHECK(RunError(registry, session, "assert.entity_exists", { { "entity", "Enemy" }, { "count", 2 }, { "exists", true } }) == "pass either 'exists' or 'count', not both");
	}

	TEST_CASE("assert.component compares the given fields within a tolerance and lists every mismatch")
	{
		CommandRegistry registry;
		AutomationSession session;
		Run(registry, session, "scene.new", { { "empty", true } });
		const uint64_t door = Run(registry, session, "entity.create", { { "name", "Door" } })["id"].get<uint64_t>();
		const uint64_t frame = Run(registry, session, "entity.create", { { "name", "Frame" } })["id"].get<uint64_t>();
		Run(registry, session, "entity.create", { { "name", "Twin" } });
		Run(registry, session, "entity.create", { { "name", "Twin" } });
		Run(registry, session, "component.set", { { "entity", "Door" }, { "component", "Transform" }, { "data", { { "Translation", { 1.0, 2.0, 3.0 } } } } });
		Run(registry, session, "component.set", { { "entity", "Door" }, { "component", "RigidBody" }, { "data", { { "Type", "Dynamic" } } } });
		Run(registry, session, "component.set", { { "entity", "Door" }, { "component", "BoxCollider" }, { "data", json::object() } });
		Run(registry, session, "component.set", { { "entity", "Frame" }, { "component", "RigidBody" }, { "data", json::object() } });
		Run(registry, session, "component.set", { { "entity", "Frame" }, { "component", "BoxCollider" }, { "data", json::object() } });
		Run(registry, session, "component.set", { { "entity", "Door" }, { "component", "Joint" }, { "data", { { "ConnectedEntity", "Frame" } } } });
		const std::string doorLabel = "'Door' (ID " + std::to_string(door) + ")";

		const json transform = Run(registry, session, "assert.component", { { "entity", "Door" }, { "component", "Transform" }, { "fields", { { "Translation", { 1.0, 2.00001, 3.0 } } } } });
		CHECK(transform["Translation"] == json({ 1.0, 2.0, 3.0 }));
		Run(registry, session, "assert.component", { { "entity", "Door" }, { "component", "RigidBody" } });
		Run(registry, session, "assert.component", { { "entity", "Door" }, { "component", "Camera" }, { "exists", false } });
		// Entity references take names, like component.set.
		Run(registry, session, "assert.component", { { "entity", "Door" }, { "component", "Joint" }, { "fields", { { "ConnectedEntity", "Frame" } } } });
		Run(registry, session, "assert.component", { { "entity", "Door" }, { "component", "Joint" }, { "fields", { { "ConnectedEntity", frame } } } });

		const std::string mismatch = RunError(registry, session, "assert.component", { { "entity", "Door" }, { "component", "Transform" }, { "fields", { { "Translation", { 1.0, 2.1, 3.0 } }, { "Scale", { 2.0, 2.0, 2.0 } } } } });
		CHECK(mismatch == "assertion failed: " + doorLabel + " Transform.Scale is [1.0,1.0,1.0], expected [2.0,2.0,2.0]; Transform.Translation is [1.0,2.0,3.0], expected [1.0,2.1,3.0]");
		Run(registry, session, "assert.component", { { "entity", "Door" }, { "component", "Transform" }, { "fields", { { "Translation", { 1.0, 2.1, 3.0 } } } }, { "tolerance", 0.2 } });
		CHECK(RunError(registry, session, "assert.component", { { "entity", "Door" }, { "component", "RigidBody" }, { "fields", { { "Type", "Static" } } } }) == "assertion failed: " + doorLabel + " RigidBody.Type is \"Dynamic\", expected \"Static\"");
		CHECK(RunError(registry, session, "assert.component", { { "entity", "Frame" }, { "component", "Joint" } }) == "assertion failed: 'Frame' (ID " + std::to_string(frame) + ") has no Joint component");
		CHECK(RunError(registry, session, "assert.component", { { "entity", "Door" }, { "component", "Joint" }, { "exists", false } }) == "assertion failed: " + doorLabel + " has a Joint component");
		// A missing or ambiguous entity is a failed assertion too.
		CHECK(RunError(registry, session, "assert.component", { { "entity", "Window" }, { "component", "Transform" } }) == "assertion failed: no entity named 'Window'");
		CHECK(RunError(registry, session, "assert.component", { { "entity", "Twin" }, { "component", "Transform" } }) == "assertion failed: several entities are named 'Twin'; use the numeric ID");
		CHECK(RunError(registry, session, "assert.component", { { "entity", "Door" }, { "component", "Joint" }, { "fields", { { "ConnectedEntity", "Wall" } } } }) == "assertion failed: no entity named 'Wall'");
		CHECK(RunError(registry, session, "assert.component", { { "entity", "Door" }, { "component", "Transform" }, { "fields", { { "Position", { 0, 0, 0 } } } } }) == "unknown field 'Position' for Transform (valid: Translation, Rotation, Scale)");
		CHECK(RunError(registry, session, "assert.component", { { "entity", "Door" }, { "component", "Nope" } }).starts_with("unknown component 'Nope' (valid: "));
		CHECK(RunError(registry, session, "assert.component", { { "entity", "Door" }, { "component", "Joint" }, { "exists", false }, { "fields", json::object() } }) == "'fields' cannot be checked with exists=false");
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
		CHECK(FileSystem::PathFromUtf8(same["actual"].get<std::string>()) == project.GetDirectory() / "Cache" / "RenderCompare.png");
		CHECK(same["diff"].is_null());
		CHECK(FileSystem::Exists(project.GetDirectory() / "Cache" / "RenderCompare.png"));

		red = 251;
		const json different = Run(registry, session, "render.compare", { { "reference", "Reference.png" }, { "width", 64 }, { "height", 32 }, { "output", "Out/Render.png" }, { "diff", "Diffs/Diff.png" } });
		CHECK(different["matches"] == false);
		CHECK(different["maxDelta"] == 51);
		CHECK(different["similarity"].get<double>() == doctest::Approx(1.0 - 51.0 / 255.0));
		CHECK(FileSystem::PathFromUtf8(different["diff"].get<std::string>()) == project.GetDirectory() / "Diffs" / "Diff.png");
		CHECK(FileSystem::Exists(project.GetDirectory() / "Out" / "Render.png"));
		CHECK(FileSystem::Exists(project.GetDirectory() / "Diffs" / "Diff.png"));
		const std::string differ = RunError(registry, session, "render.compare", { { "reference", "Reference.png" }, { "width", 64 }, { "height", 32 }, { "assertMatch", true } });
		CHECK(Contains(differ, "images differ"));
		CHECK(Contains(differ, "RenderCompare.png"));

		// A size mismatch never matches, scores 0 and leaves no diff (not even an old one).
		const json resized = Run(registry, session, "render.compare", { { "reference", "Reference.png" }, { "diff", "Diffs/Diff.png" } });
		CHECK(resized["sizeMatches"] == false);
		CHECK(resized["similarity"] == 0.0);
		CHECK(resized["diff"].is_null());
		CHECK_FALSE(FileSystem::Exists(project.GetDirectory() / "Diffs" / "Diff.png"));

		// Neither the render nor the diff may replace a compared image.
		const int rendersBefore = renders;
		CHECK(Contains(RunError(registry, session, "render.compare", { { "reference", "Reference.png" }, { "output", "Reference.png" } }), "'output' is the reference image"));
		CHECK(Contains(RunError(registry, session, "render.compare", { { "reference", "Reference.png" }, { "output", "Sub/../Reference.png" } }), "'output' is the reference image"));
		CHECK(renders == rendersBefore);
		const std::string referenceBytes = *FileSystem::ReadTextFile(project.GetDirectory() / "Reference.png");
		CHECK(RunError(registry, session, "render.compare", { { "reference", "Reference.png" }, { "width", 64 }, { "height", 32 }, { "diff", "Reference.png" } }) == "'diff' must not be one of the compared images");
		CHECK(RunError(registry, session, "image.compare", { { "actual", "Out/Render.png" }, { "reference", "Reference.png" }, { "diff", "Out/Render.png" } }) == "'diff' must not be one of the compared images");
		CHECK(*FileSystem::ReadTextFile(project.GetDirectory() / "Reference.png") == referenceBytes);

		session.Screenshot = [](const std::filesystem::path&, uint32_t, uint32_t, std::string& outError) {
			outError = "no camera";
			return false;
		};
		CHECK(RunError(registry, session, "render.compare", { { "reference", "Reference.png" } }) == "no camera");
	}

	TEST_CASE("component.types reports every field's type, enum values and entity fields")
	{
		CommandRegistry registry;
		AutomationSession session;
		const json components = Run(registry, session, "component.types")["components"];
		std::map<std::string, json> byName;
		for (const json& component : components)
		{
			INFO(component["name"]);
			std::set<std::string> typed;
			for (const auto& [field, type] : component["types"].items())
				typed.insert(field);
			CHECK(typed == component["fields"].get<std::set<std::string>>());
			for (const auto& [field, type] : component["types"].items())
			{
				if (type.get<std::string>().starts_with("enum"))
					CHECK(component["enums"].contains(field));
				if (type == "entity")
					CHECK(std::ranges::find(component["entityFields"], json(field)) != component["entityFields"].end());
			}
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

	TEST_CASE("A rejected poll keeps the drop count and every queued event")
	{
		AutomationEventHub hub;
		const uint64_t id = *hub.Subscribe({}, 0, AutomationEventHub::Delivery::Polled);
		const size_t extra = 3;
		for (size_t i = 0; i < AutomationEventHub::MaxQueuedEvents + extra; i++)
			hub.Publish(AutomationEventType::PlayState, { { "state", "play" } });
		std::vector<json> events;
		uint64_t dropped = 0;
		size_t checked = 0;
		const AutomationEventHub::PollCheck refuse = [&checked](std::span<const json> queued) {
			checked = queued.size();
			return false;
		};
		CHECK(hub.Poll(id, 10, events, dropped, refuse) == AutomationEventHub::PollStatus::Rejected);
		// The check sees exactly the events the poll would return.
		CHECK(checked == 10);
		CHECK(events.size() == 10);
		CHECK(dropped == extra);
		CHECK(hub.Poll(id, AutomationEventHub::MaxQueuedEvents, events, dropped, refuse) == AutomationEventHub::PollStatus::Rejected);
		CHECK(checked == AutomationEventHub::MaxQueuedEvents);
		CHECK(dropped == extra);
		// An accepting check takes the events (and resets the drop count) like a poll without one.
		REQUIRE(hub.Poll(id, 2, events, dropped, [](std::span<const json> queued) { return queued.size() == 2; }) == AutomationEventHub::PollStatus::Ok);
		CHECK(events.size() == 2);
		CHECK(dropped == extra);
		REQUIRE(hub.Poll(id, AutomationEventHub::MaxQueuedEvents, events, dropped) == AutomationEventHub::PollStatus::Ok);
		CHECK(events.size() == AutomationEventHub::MaxQueuedEvents - 2);
		CHECK(dropped == 0);
		REQUIRE(hub.Poll(id, 1, events, dropped) == AutomationEventHub::PollStatus::Ok);
		CHECK(events.empty());
		CHECK(dropped == 0);
	}

	TEST_CASE("The event hub stays consistent while other threads log")
	{
		CommandRegistry registry;
		AutomationSession session;
		const uint64_t whole = Subscribe(registry, session, { "log" }, "trace");
		std::atomic<bool> running = true;
		std::atomic<uint64_t> published = 0;
		std::vector<std::thread> loggers;
		loggers.reserve(2);
		for (int t = 0; t < 2; t++)
		{
			loggers.emplace_back([&]() {
				while (running)
				{
					BS_CORE_TRACE("events test: stress");
					published++;
				}
			});
		}
		uint64_t received = 0;
		uint64_t dropped = 0;
		uint64_t lastSeq = 0;
		for (int i = 0; i < 300; i++)
		{
			const std::optional<uint64_t> extra = session.GetEvents()->Subscribe({}, 9, AutomationEventHub::Delivery::Streamed);
			REQUIRE(extra);
			// Draining a stream while others publish yields whole, valid frames.
			for (const json& frame : Lines(session.GetEvents()->TakeStreamed(9)))
				CHECK_FALSE(frame.is_discarded());
			session.GetEvents()->Unsubscribe(*extra);
			std::vector<json> events;
			uint64_t lost = 0;
			REQUIRE(session.GetEvents()->Poll(whole, AutomationEventHub::MaxQueuedEvents, events, lost) == AutomationEventHub::PollStatus::Ok);
			dropped += lost;
			for (const json& event : events)
			{
				CHECK(event["seq"].get<uint64_t>() > lastSeq);
				lastSeq = event["seq"].get<uint64_t>();
				if (event["text"] == "events test: stress")
					received++;
			}
		}
		running = false;
		for (std::thread& logger : loggers)
			logger.join();
		std::vector<json> rest;
		uint64_t lost = 0;
		REQUIRE(session.GetEvents()->Poll(whole, AutomationEventHub::MaxQueuedEvents, rest, lost) == AutomationEventHub::PollStatus::Ok);
		dropped += lost;
		for (const json& event : rest)
		{
			if (event["text"] == "events test: stress")
				received++;
		}
		CHECK(published.load() > 0);
		// Every line the subscription saw was either delivered or counted as dropped (other code may log too).
		CHECK(received + dropped >= published.load());
	}

	TEST_CASE("A client that stops reading its event stream is disconnected")
	{
		CommandRegistry registry;
		AutomationSession session;
		AutomationServer server;
		server.SetSendTimeout(200);
		std::string error;
		REQUIRE(server.StartWithFactory(0, [&]() {
			auto connection = std::make_shared<AutomationConnection>();
			return AutomationServer::ClientHandler{ [&, connection](const std::string& request) { return connection->HandleRequest(registry, session, request); }, [connection]() { return connection->TakePendingEvents(); } }; }, error));
		const uint64_t logCursor = Log::GetHistory().GetTotalCount();
		{
			BasaltTest::LineClient client(server.GetPort(), 1024);
			REQUIRE(client.Send(R"({"id":1,"command":"events.subscribe","params":{"types":["log"],"minLevel":"trace"}})"));
			REQUIRE(ParseJson(client.ReadLine())["ok"] == true);
			// Never read again: the server's sends back up until the timeout disconnects the client.
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
			const std::string padding(200, 'x');
			while (session.GetEvents()->GetSubscriptionCount() != 0 && std::chrono::steady_clock::now() < deadline)
			{
				for (int i = 0; i < 200; i++)
					BS_CORE_TRACE("events test: flood {}", padding);
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			CHECK(session.GetEvents()->GetSubscriptionCount() == 0);
		}
		bool warned = false;
		uint64_t next = 0;
		for (const LogMessage& message : Log::GetHistory().GetMessagesSince(logCursor, next))
			warned = warned || Contains(message.Text, "Automation: closing connection; cannot send");
		CHECK(warned);
		const auto stopStart = std::chrono::steady_clock::now();
		server.Stop();
		CHECK(std::chrono::steady_clock::now() - stopStart < std::chrono::seconds(2));
	}

	TEST_CASE("A session that goes while a host still holds its play copy detaches its listener")
	{
		Ref<Scene> copy;
		{
			CommandRegistry registry;
			AutomationSession session;
			Run(registry, session, "scene.new", { { "empty", true } });
			AddFallingBall(*session.GetEditScene());
			Subscribe(registry, session, { "collision" });
			Run(registry, session, "play.start");
			copy = session.GetScene();
			REQUIRE(copy->GetEventListener() != nullptr);
		}
		CHECK(copy->GetEventListener() == nullptr);
		// Contacts after the session is gone reach no listener (ASan would catch a dangling one).
		for (int i = 0; i < 150; i++)
			copy->OnUpdate(BasaltTest::Step);
		copy->OnRuntimeStop();
	}

	TEST_CASE("Event client scopes nest and restore the previous client, even when a command fails")
	{
		CommandRegistry registry;
		AutomationSession session;
		{
			const AutomationSession::EventClientScope outer(session, { 2, AutomationEventDelivery::Polled });
			{
				const AutomationSession::EventClientScope inner(session, { 1, AutomationEventDelivery::Streamed });
				CHECK(session.GetEventClient().Owner == 1);
			}
			CHECK(session.GetEventClient().Owner == 2);
			CHECK(session.GetEventClient().Delivery == AutomationEventDelivery::Polled);
		}
		CHECK(session.GetEventClient().Owner == 0);
		AutomationConnection connection;
		const json failed = ParseJson(connection.HandleRequest(registry, session, R"({"command":"entity.get","params":{"entity":"Nobody"}})"));
		CHECK(failed["ok"] == false);
		CHECK(session.GetEventClient().Owner == 0);
	}
}
