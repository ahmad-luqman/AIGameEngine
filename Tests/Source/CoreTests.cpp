#include <doctest/doctest.h>

#include <Basalt/Core/FileSystem.h>
#include <Basalt/Core/GamepadCodes.h>
#include <Basalt/Core/Input.h>
#include <Basalt/Core/JsonUtils.h>
#include <Basalt/Core/KeyCodes.h>
#include <Basalt/Core/LayerStack.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Core/MouseCodes.h>
#include <Basalt/Core/Process.h>
#include <Basalt/Core/UUID.h>
#include <Basalt/Events/ApplicationEvent.h>
#include <Basalt/Events/KeyEvent.h>
#include <Basalt/Events/MouseEvent.h>

#include "TestUtils.h"

#include <csignal>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <unordered_set>
#include <vector>

#if !defined(_WIN32)
	#include <unistd.h>
#endif

using namespace Basalt;

TEST_SUITE("Core")
{
	TEST_CASE("JsonToUInt32 accepts exactly the integers 0..4294967295")
	{
		using json = nlohmann::json;
		uint32_t out = 7;
		CHECK((JsonToUInt32(json(0u), out) && out == 0u));
		CHECK((JsonToUInt32(json::parse("4294967295"), out) && out == 4294967295u));
		CHECK((JsonToUInt32(json(int64_t{ 5 }), out) && out == 5u)); // signed, in range
		out = 9;
		for (const json& bad : { json::parse("4294967296"), json::parse("18446744073709551615"), json(int64_t{ 4294967296 }), json(-1), json(1.0), json(2.5), json("1"), json(true), json(nullptr) })
		{
			INFO(bad.dump());
			CHECK_FALSE(JsonToUInt32(bad, out));
			CHECK(out == 9u); // untouched on failure
		}
	}

	TEST_CASE("UUID generates unique non-zero values")
	{
		std::unordered_set<uint64_t> values;
		for (int i = 0; i < 10000; i++)
		{
			UUID uuid;
			CHECK(uuid.IsValid());
			values.insert(uuid);
		}
		CHECK(values.size() == 10000);

		constexpr UUID fixed(42);
		CHECK(static_cast<uint64_t>(fixed) == 42);
		CHECK_FALSE(UUID(0).IsValid());
	}

	TEST_CASE("LogHistory keeps the most recent messages in order")
	{
		LogHistory history(4);
		for (int i = 0; i < 6; i++)
			history.Push({ i == 5 ? LogLevel::Error : LogLevel::Info, "Test", std::to_string(i) });

		const auto all = history.GetAll();
		REQUIRE(all.size() == 4);
		CHECK(all[0].Text == "2");
		CHECK(all[3].Text == "5");
		CHECK(history.GetTotalCount() == 6);
		CHECK(history.GetErrorCount() == 1);

		uint64_t next = 0;
		const auto since = history.GetMessagesSince(4, next);
		REQUIRE(since.size() == 2);
		CHECK(since[0].Text == "4");
		CHECK(next == 6);

		const auto none = history.GetMessagesSince(next, next);
		CHECK(none.empty());

		history.Clear();
		CHECK(history.GetAll().empty());
		CHECK(history.GetErrorCount() == 0);
		CHECK(history.GetTotalCount() == 6);
	}

	TEST_CASE("LogHistory stays consistent across multiple wraps and clears")
	{
		LogHistory history(4);
		for (int i = 0; i < 10; i++)
			history.Push({ LogLevel::Info, "Test", std::to_string(i) });

		auto all = history.GetAll();
		REQUIRE(all.size() == 4);
		CHECK(all[0].Text == "6");
		CHECK(all[3].Text == "9");

		// A cursor older than the oldest stored message returns everything still stored.
		uint64_t next = 0;
		auto since = history.GetMessagesSince(2, next);
		REQUIRE(since.size() == 4);
		CHECK(since[0].Text == "6");
		CHECK(next == 10);

		history.Clear();
		history.Push({ LogLevel::Warn, "Test", "after clear" });
		since = history.GetMessagesSince(next, next);
		REQUIRE(since.size() == 1);
		CHECK(since[0].Text == "after clear");
		CHECK(next == 11);
		CHECK(history.GetAll().size() == 1);
	}

	TEST_CASE("Engine log messages reach the global history")
	{
		const uint64_t before = Log::GetHistory().GetTotalCount();
		BS_CORE_INFO("history probe {}", 123);
		uint64_t next = 0;
		const auto messages = Log::GetHistory().GetMessagesSince(before, next);
		REQUIRE_FALSE(messages.empty());
		CHECK(messages.back().Text == "history probe 123");
		CHECK(messages.back().LoggerName == "BASALT");
	}

	TEST_CASE("KeyCode and MouseCode names round-trip")
	{
		CHECK(KeyCodeToString(KeyCode::Space) == "Space");
		CHECK(KeyCodeFromString("space") == KeyCode::Space);
		CHECK(KeyCodeFromString("A") == KeyCode::A);
		CHECK(KeyCodeFromString("leftshift") == KeyCode::LeftShift);
		CHECK(KeyCodeFromString("F12") == KeyCode::F12);
		CHECK_FALSE(KeyCodeFromString("NotAKey").has_value());
		CHECK(KeyCodeToString(static_cast<KeyCode>(1)) == "Unknown");

		CHECK(MouseCodeToString(MouseCode::Right) == "Right");
		CHECK(MouseCodeFromString("middle") == MouseCode::Middle);
		CHECK_FALSE(MouseCodeFromString("Wheel").has_value());
	}

	TEST_CASE("Input tracks down, pressed and released transitions")
	{
		Input::Reset();
		Input::EndFrame();

		KeyPressedEvent press(KeyCode::W);
		Input::OnEvent(press);
		CHECK(Input::IsKeyDown(KeyCode::W));
		CHECK(Input::IsKeyPressed(KeyCode::W));
		CHECK_FALSE(Input::IsKeyReleased(KeyCode::W));
		CHECK_FALSE(press.Handled);

		Input::EndFrame();
		CHECK(Input::IsKeyDown(KeyCode::W));
		CHECK_FALSE(Input::IsKeyPressed(KeyCode::W));

		KeyReleasedEvent release(KeyCode::W);
		Input::OnEvent(release);
		CHECK_FALSE(Input::IsKeyDown(KeyCode::W));
		CHECK(Input::IsKeyReleased(KeyCode::W));

		Input::EndFrame();
		CHECK_FALSE(Input::IsKeyReleased(KeyCode::W));
	}

	TEST_CASE("Input tracks mouse position, delta, scroll and focus loss")
	{
		Input::Reset();
		MouseMovedEvent first(100.0f, 50.0f);
		Input::OnEvent(first);
		Input::EndFrame();

		MouseMovedEvent second(110.0f, 45.0f);
		Input::OnEvent(second);
		CHECK(Input::GetMousePosition() == glm::vec2(110.0f, 45.0f));
		CHECK(Input::GetMouseDelta() == glm::vec2(10.0f, -5.0f));

		MouseScrolledEvent scroll(0.0f, 2.0f);
		Input::OnEvent(scroll);
		Input::OnEvent(scroll);
		CHECK(Input::GetMouseScroll() == glm::vec2(0.0f, 4.0f));

		MouseButtonPressedEvent click(MouseCode::Left);
		Input::OnEvent(click);
		CHECK(Input::IsMouseButtonPressed(MouseCode::Left));

		Input::EndFrame();
		CHECK(Input::GetMouseScroll() == glm::vec2(0.0f, 0.0f));
		CHECK(Input::GetMouseDelta() == glm::vec2(0.0f, 0.0f));

		WindowLostFocusEvent lostFocus;
		Input::OnEvent(lostFocus);
		CHECK_FALSE(Input::IsMouseButtonDown(MouseCode::Left));
		CHECK(Input::GetMousePosition() == glm::vec2(110.0f, 45.0f));
	}

	TEST_CASE("Gamepad button and axis names round-trip")
	{
		for (uint32_t i = 0; i < GamepadButtonCount; i++)
		{
			const auto button = static_cast<GamepadButton>(i);
			CHECK(GamepadButtonFromString(GamepadButtonToString(button)) == button);
		}
		for (uint32_t i = 0; i < GamepadAxisCount; i++)
		{
			const auto axis = static_cast<GamepadAxis>(i);
			CHECK(GamepadAxisFromString(GamepadAxisToString(axis)) == axis);
		}
		CHECK(GamepadButtonFromString("dpadup") == GamepadButton::DPadUp);
		CHECK(GamepadAxisFromString("lefttrigger") == GamepadAxis::LeftTrigger);
		CHECK_FALSE(GamepadButtonFromString("Turbo").has_value());
		CHECK_FALSE(GamepadAxisFromString("").has_value());
		CHECK(GamepadButtonToString(static_cast<GamepadButton>(GamepadButtonCount)) == "Unknown");
		CHECK(GamepadAxisToString(static_cast<GamepadAxis>(GamepadAxisCount)) == "Unknown");
		CHECK(IsGamepadTrigger(GamepadAxis::RightTrigger));
		CHECK_FALSE(IsGamepadTrigger(GamepadAxis::RightY));
	}

	TEST_CASE("Input tracks injected gamepads: connection, buttons, axes and their transitions")
	{
		Input::Reset();
		Input::ForgetInjectedGamepads();
		Input::EndFrame();

		// Disconnected gamepads read as released and centered, and cannot be driven.
		CHECK_FALSE(Input::IsGamepadConnected(1));
		CHECK_FALSE(Input::SetGamepadButtonState(1, GamepadButton::A, true));
		CHECK_FALSE(Input::SetGamepadAxis(1, GamepadAxis::LeftX, 1.0f));
		CHECK(Input::GetGamepadName(1).empty());
		CHECK_FALSE(Input::IsGamepadConnected(Input::MaxGamepads));
		CHECK_FALSE(Input::SetGamepadButtonState(Input::MaxGamepads, GamepadButton::A, true));

		CHECK(Input::GetGamepadConnection(1) == 0);
		Input::SetGamepadConnected(1, true, "Pad");
		CHECK(Input::IsGamepadConnected(1));
		const uint32_t connection = Input::GetGamepadConnection(1);
		CHECK(connection != 0);
		CHECK(Input::GetGamepadName(1) == "Pad");
		CHECK(Input::SetGamepadButtonState(1, GamepadButton::A, true));
		CHECK(Input::IsGamepadButtonDown(1, GamepadButton::A));
		CHECK(Input::IsGamepadButtonPressed(1, GamepadButton::A));
		CHECK_FALSE(Input::IsGamepadButtonDown(0, GamepadButton::A));
		Input::EndFrame();
		CHECK_FALSE(Input::IsGamepadButtonPressed(1, GamepadButton::A));
		CHECK(Input::IsGamepadButtonDown(1, GamepadButton::A));

		// Axes are clamped to their range; NaN centers them.
		CHECK(Input::SetGamepadAxis(1, GamepadAxis::LeftX, -3.0f));
		CHECK(Input::GetGamepadAxis(1, GamepadAxis::LeftX) == -1.0f);
		CHECK(Input::SetGamepadAxis(1, GamepadAxis::LeftTrigger, -0.5f));
		CHECK(Input::GetGamepadAxis(1, GamepadAxis::LeftTrigger) == 0.0f);
		CHECK(Input::SetGamepadAxis(1, GamepadAxis::RightY, 0.25f));
		CHECK(Input::GetGamepadAxis(1, GamepadAxis::RightY) == 0.25f);
		CHECK(Input::SetGamepadAxis(1, GamepadAxis::RightY, std::numeric_limits<float>::quiet_NaN()));
		CHECK(Input::GetGamepadAxis(1, GamepadAxis::RightY) == 0.0f);

		// A reset (focus loss) releases buttons and centers sticks without a release or a disconnect.
		CHECK(Input::SetGamepadAxis(1, GamepadAxis::LeftX, 1.0f));
		Input::Reset();
		CHECK(Input::IsGamepadConnected(1));
		// Renaming an injected gamepad is the same connection.
		Input::SetGamepadConnected(1, true, "Renamed");
		CHECK(Input::GetGamepadConnection(1) == connection);
		CHECK_FALSE(Input::IsGamepadButtonDown(1, GamepadButton::A));
		CHECK_FALSE(Input::IsGamepadButtonReleased(1, GamepadButton::A));
		CHECK(Input::GetGamepadAxis(1, GamepadAxis::LeftX) == 0.0f);

		// Disconnecting while a button is held reports the release and the disconnect on that frame.
		CHECK(Input::SetGamepadButtonState(1, GamepadButton::B, true));
		Input::EndFrame();
		Input::SetGamepadConnected(1, false);
		CHECK_FALSE(Input::IsGamepadConnected(1));
		CHECK(Input::GetGamepadConnection(1) == 0);
		CHECK(Input::IsGamepadButtonReleased(1, GamepadButton::B));
		CHECK_FALSE(Input::IsGamepadButtonDown(1, GamepadButton::B));
		Input::EndFrame();
		CHECK_FALSE(Input::IsGamepadButtonReleased(1, GamepadButton::B));
		// Connecting again is a new connection.
		Input::SetGamepadConnected(1, true);
		CHECK(Input::GetGamepadConnection(1) != connection);
		Input::SetGamepadConnected(1, false);
	}

	TEST_CASE("Device polls feed gamepads but leave injected ones alone")
	{
		Input::Reset();
		Input::ForgetInjectedGamepads();
		Input::EndFrame();

		GamepadDeviceState device;
		device.Name = "Controller";
		device.Buttons[static_cast<size_t>(GamepadButton::Start)] = true;
		device.Axes[static_cast<size_t>(GamepadAxis::LeftY)] = 2.0f;
		Input::UpdateGamepadFromDevice(2, &device);
		CHECK(Input::IsGamepadConnected(2));
		const uint32_t deviceConnection = Input::GetGamepadConnection(2);
		CHECK(deviceConnection != 0);
		CHECK(Input::GetGamepadName(2) == "Controller");
		CHECK(Input::IsGamepadButtonPressed(2, GamepadButton::Start));
		CHECK(Input::GetGamepadAxis(2, GamepadAxis::LeftY) == 1.0f);
		// Later polls are the same connection.
		Input::UpdateGamepadFromDevice(2, &device);
		CHECK(Input::GetGamepadConnection(2) == deviceConnection);
		Input::EndFrame();
		Input::UpdateGamepadFromDevice(2, nullptr);
		CHECK(Input::GetGamepadConnection(2) == 0);
		CHECK(Input::IsGamepadButtonReleased(2, GamepadButton::Start));

		// An injected gamepad keeps its state through polls that see no device there, or another device.
		Input::SetGamepadConnected(3, true, "Injected");
		CHECK(Input::SetGamepadButtonState(3, GamepadButton::X, true));
		Input::UpdateGamepadFromDevice(3, nullptr);
		Input::UpdateGamepadFromDevice(3, &device);
		CHECK(Input::IsGamepadConnected(3));
		CHECK(Input::GetGamepadName(3) == "Injected");
		CHECK(Input::IsGamepadButtonDown(3, GamepadButton::X));
		CHECK_FALSE(Input::IsGamepadButtonDown(3, GamepadButton::Start));
		// Once disconnected, the device poll owns the slot again.
		Input::SetGamepadConnected(3, false);
		Input::UpdateGamepadFromDevice(3, &device);
		CHECK(Input::GetGamepadName(3) == "Controller");
		Input::UpdateGamepadFromDevice(3, nullptr);
		Input::UpdateGamepadFromDevice(Input::MaxGamepads, &device);
		Input::EndFrame();

		// Forgetting injected gamepads reports no release; device gamepads are not forgotten. A gamepad connected
		// again is a new connection.
		Input::SetGamepadConnected(4, true);
		CHECK(Input::SetGamepadButtonState(4, GamepadButton::Y, true));
		const uint32_t injected = Input::GetGamepadConnection(4);
		Input::UpdateGamepadFromDevice(5, &device);
		Input::EndFrame();
		Input::ForgetInjectedGamepads();
		CHECK_FALSE(Input::IsGamepadConnected(4));
		CHECK_FALSE(Input::IsGamepadButtonReleased(4, GamepadButton::Y));
		CHECK(Input::IsGamepadConnected(5));
		Input::SetGamepadConnected(4, true);
		CHECK(Input::GetGamepadConnection(4) != injected);
		Input::SetGamepadConnected(4, false);
		Input::UpdateGamepadFromDevice(5, nullptr);
		Input::EndFrame();
	}

	TEST_CASE("A physical gamepad held through a focus loss stays released until it is let go")
	{
		Input::Reset();
		Input::ForgetInjectedGamepads();
		GamepadDeviceState device;
		device.Name = "Pad";
		const auto hold = [&device](bool a, float leftX) {
			device.Buttons[static_cast<size_t>(GamepadButton::A)] = a;
			device.Axes[static_cast<size_t>(GamepadAxis::LeftX)] = leftX;
		};
		hold(true, 0.9f);
		Input::UpdateGamepadFromDevice(0, &device);
		CHECK(Input::GetGamepadSource(0) == GamepadSource::Device);
		Input::EndFrame();
		CHECK(Input::IsGamepadButtonDown(0, GamepadButton::A));

		// What the window does on focus loss: Reset, then polls while unfocused (A and the stick still held).
		Input::Reset();
		Input::UpdateGamepadFromDevice(0, &device, false);
		CHECK_FALSE(Input::IsGamepadButtonDown(0, GamepadButton::A));
		CHECK(Input::GetGamepadAxis(0, GamepadAxis::LeftX) == 0.0f);
		CHECK(Input::IsGamepadConnected(0));
		Input::EndFrame();
		Input::UpdateGamepadFromDevice(0, &device, false);
		Input::EndFrame();
		// Focus is back, still held: no new press.
		Input::UpdateGamepadFromDevice(0, &device);
		CHECK_FALSE(Input::IsGamepadButtonDown(0, GamepadButton::A));
		CHECK_FALSE(Input::IsGamepadButtonPressed(0, GamepadButton::A));
		CHECK(Input::GetGamepadAxis(0, GamepadAxis::LeftX) == 0.0f);
		Input::EndFrame();
		// Let go (the stick back near the center), then pressed again: a real press.
		hold(false, 0.1f);
		Input::UpdateGamepadFromDevice(0, &device);
		CHECK(Input::GetGamepadAxis(0, GamepadAxis::LeftX) == doctest::Approx(0.1f));
		Input::EndFrame();
		hold(true, 0.9f);
		Input::UpdateGamepadFromDevice(0, &device);
		CHECK(Input::IsGamepadButtonPressed(0, GamepadButton::A));
		CHECK(Input::GetGamepadAxis(0, GamepadAxis::LeftX) == doctest::Approx(0.9f));
		Input::EndFrame();

		// A button first pressed while unfocused is blocked too; a reset while focused (stopping play) blocks held
		// buttons as well.
		hold(false, 0.0f);
		Input::UpdateGamepadFromDevice(0, &device);
		Input::EndFrame();
		hold(true, 0.0f);
		Input::UpdateGamepadFromDevice(0, &device, false);
		Input::UpdateGamepadFromDevice(0, &device);
		CHECK_FALSE(Input::IsGamepadButtonDown(0, GamepadButton::A));
		hold(false, 0.0f);
		Input::UpdateGamepadFromDevice(0, &device);
		hold(true, 0.0f);
		Input::UpdateGamepadFromDevice(0, &device);
		Input::EndFrame();
		Input::Reset();
		Input::UpdateGamepadFromDevice(0, &device);
		CHECK_FALSE(Input::IsGamepadButtonDown(0, GamepadButton::A));

		// Injecting over a physical gamepad takes it over, released; it cannot be driven while it is the device's.
		Input::EndFrame();
		Input::UpdateGamepadFromDevice(1, &device);
		CHECK_FALSE(Input::SetGamepadButtonState(1, GamepadButton::B, true));
		const uint32_t deviceConnection = Input::GetGamepadConnection(1);
		Input::SetGamepadConnected(1, true, "Test");
		CHECK(Input::GetGamepadSource(1) == GamepadSource::Injected);
		CHECK(Input::GetGamepadConnection(1) != deviceConnection);
		CHECK_FALSE(Input::IsGamepadButtonDown(1, GamepadButton::A));
		CHECK(Input::SetGamepadButtonState(1, GamepadButton::B, true));

		// With device gamepads off (replays), physical ones are forgotten and polls ignored; a reset keeps the setting.
		Input::SetDeviceGamepadsEnabled(false);
		CHECK_FALSE(Input::IsDeviceGamepadsEnabled());
		CHECK_FALSE(Input::IsGamepadConnected(0));
		CHECK_FALSE(Input::IsGamepadButtonReleased(0, GamepadButton::A));
		Input::Reset();
		Input::UpdateGamepadFromDevice(0, &device);
		CHECK_FALSE(Input::IsGamepadConnected(0));
		CHECK(Input::IsGamepadConnected(1));
		Input::SetDeviceGamepadsEnabled(true);
		Input::UpdateGamepadFromDevice(0, &device);
		CHECK(Input::IsGamepadConnected(0));
		Input::UpdateGamepadFromDevice(0, nullptr);
		Input::ForgetInjectedGamepads();
		Input::EndFrame();
	}

	TEST_CASE("The mouse position can be forgotten, as before the first one arrived")
	{
		Input::SetMousePosition({ 10.0f, 20.0f });
		CHECK(Input::HasMousePosition());
		Input::ResetMousePosition();
		CHECK_FALSE(Input::HasMousePosition());
		CHECK(Input::GetMousePosition() == glm::vec2(0.0f));
		// The next position is the first again: no delta from the origin.
		Input::SetMousePosition({ 300.0f, 200.0f });
		CHECK(Input::GetMouseDelta() == glm::vec2(0.0f));
		Input::EndFrame();
	}

	TEST_CASE("Gamepad name lists match the enums")
	{
		REQUIRE(GetGamepadButtonNames().size() == GamepadButtonCount);
		REQUIRE(GetGamepadAxisNames().size() == GamepadAxisCount);
		CHECK(GetGamepadButtonNames()[static_cast<size_t>(GamepadButton::DPadLeft)] == "DPadLeft");
		CHECK(GetGamepadAxisNames()[static_cast<size_t>(GamepadAxis::RightTrigger)] == "RightTrigger");
	}

	TEST_CASE("EventDispatcher routes by type and records handling")
	{
		WindowResizeEvent resize(800, 600);
		CHECK(resize.IsInCategory(EventCategoryApplication));
		CHECK_FALSE(resize.IsInCategory(EventCategoryInput));

		EventDispatcher dispatcher(resize);
		bool keyCalled = false;
		CHECK_FALSE(dispatcher.Dispatch<KeyPressedEvent>([&](KeyPressedEvent&) { keyCalled = true; return true; }));
		CHECK_FALSE(keyCalled);

		uint32_t width = 0;
		CHECK(dispatcher.Dispatch<WindowResizeEvent>([&](WindowResizeEvent& e) { width = e.GetWidth(); return true; }));
		CHECK(width == 800);
		CHECK(resize.Handled);
	}

	TEST_CASE("LayerStack orders layers below overlays and owns them")
	{
		struct TrackingLayer : Layer
		{
			TrackingLayer(std::string name, std::vector<std::string>& log)
				: Layer(std::move(name))
				, Log(log)
			{
			}
			~TrackingLayer() override { Log.push_back("delete " + m_DebugName); }
			void OnAttach() override { Log.push_back("attach " + m_DebugName); }
			void OnDetach() override { Log.push_back("detach " + m_DebugName); }
			std::vector<std::string>& Log;
		};

		std::vector<std::string> log;
		{
			LayerStack stack;
			auto* a = new TrackingLayer("A", log);
			auto* overlay = new TrackingLayer("Overlay", log);
			auto* b = new TrackingLayer("B", log);
			stack.PushLayer(a);
			stack.PushOverlay(overlay);
			stack.PushLayer(b);

			REQUIRE(stack.GetSize() == 3);
			CHECK(stack[0]->GetName() == "A");
			CHECK(stack[1]->GetName() == "B");
			CHECK(stack[2]->GetName() == "Overlay");

			CHECK_FALSE(stack.PopOverlay(a));
			CHECK(stack.PopLayer(a));
			delete a;
			CHECK(stack.GetSize() == 2);
			CHECK_FALSE(stack.PopLayer(overlay));
		}

		const std::vector<std::string> expected = {
			"attach A", "attach Overlay", "attach B", "detach A", "delete A",
			"detach Overlay", "delete Overlay", "detach B", "delete B"
		};
		CHECK(log == expected);
	}

	TEST_CASE("FileSystem writes atomically and reads back")
	{
		const auto directory = std::filesystem::temp_directory_path() / "BasaltTests" / "FileSystem";
		std::filesystem::remove_all(directory);
		const auto path = directory / "nested" / "file.txt";

		CHECK_FALSE(FileSystem::ReadTextFile(path).has_value());
		REQUIRE(FileSystem::WriteTextFile(path, "hello basalt"));
		CHECK(FileSystem::Exists(path));
		CHECK_FALSE(FileSystem::Exists(path.string() + ".tmp"));
		CHECK(FileSystem::ReadTextFile(path).value() == "hello basalt");

		REQUIRE(FileSystem::WriteTextFile(path, "replaced"));
		CHECK(FileSystem::ReadTextFile(path).value() == "replaced");

		const uint8_t bytes[] = { 0, 1, 2, 255 };
		REQUIRE(FileSystem::WriteBinaryFile(directory / "data.bin", bytes, sizeof(bytes)));
		const auto readBytes = FileSystem::ReadBinaryFile(directory / "data.bin");
		REQUIRE(readBytes.has_value());
		CHECK(*readBytes == std::vector<uint8_t>(std::begin(bytes), std::end(bytes)));

		CHECK(std::filesystem::exists(FileSystem::GetExecutableDirectory()));
		std::filesystem::remove_all(directory);
	}

	TEST_CASE("FileSystem write failure leaves the target untouched and no temp file")
	{
		const auto directory = std::filesystem::temp_directory_path() / "BasaltTests" / "FileSystemFailure";
		std::filesystem::remove_all(directory);
		std::filesystem::create_directories(directory);

		// The target's "parent directory" is a regular file, so the temp file cannot be created.
		const auto blocker = directory / "blocker";
		REQUIRE(FileSystem::WriteTextFile(blocker, "original"));
		CHECK_FALSE(FileSystem::WriteTextFile(blocker / "child.txt", "data"));
		CHECK(FileSystem::ReadTextFile(blocker).value() == "original");

		// A directory occupying the target path cannot be replaced; the temp file must be cleaned up.
		const auto occupied = directory / "occupied";
		std::filesystem::create_directories(occupied / "inner");
		CHECK_FALSE(FileSystem::WriteTextFile(occupied, "data"));
		CHECK(std::filesystem::is_directory(occupied));
		CHECK_FALSE(FileSystem::Exists(occupied.string() + ".tmp"));

		std::filesystem::remove_all(directory);
	}
}

#include <Basalt/Core/CommandLine.h>

TEST_SUITE("Core")
{
	TEST_CASE("CommandLine parses flags, values, positionals and errors")
	{
		const char* argv[] = { "app", "--project", "Game", "--headless", "scene.bscene", "--frames=10", "--width", "abc", "--screenshot" };
		const CommandLine commandLine(9, argv, { "project", "frames", "width", "screenshot" });
		CHECK(commandLine.GetValue("project") == "Game");
		CHECK(commandLine.HasFlag("headless"));
		CHECK_FALSE(commandLine.HasFlag("project"));
		CHECK(commandLine.GetInteger("frames") == 10);
		CHECK_FALSE(commandLine.GetInteger("width").has_value());
		CHECK_FALSE(commandLine.GetValue("missing").has_value());
		REQUIRE(commandLine.GetPositional().size() == 1);
		CHECK(commandLine.GetPositional()[0] == "scene.bscene");
		REQUIRE(commandLine.GetErrors().size() == 1);
		CHECK(commandLine.GetErrors()[0].find("screenshot") != std::string::npos);
	}

	TEST_CASE("CommandLine reads finite floats and rejects everything else")
	{
		const char* argv[] = { "app", "--a", "0.5", "--b", "2", "--c", "1e-1", "--d", "0.5x", "--e", "nan", "--f", "inf", "--g", "1e60", "--h", " 1", "--i", "" };
		const CommandLine commandLine(19, argv, { "a", "b", "c", "d", "e", "f", "g", "h", "i" });
		CHECK(commandLine.GetFloat("a") == 0.5f);
		CHECK(commandLine.GetFloat("b") == 2.0f);
		CHECK(commandLine.GetFloat("c") == doctest::Approx(0.1f));
		for (const char* name : { "d", "e", "f", "g", "h", "i", "missing" })
		{
			INFO(name);
			CHECK_FALSE(commandLine.GetFloat(name).has_value());
		}
	}

	// Regression: untrusted JSON (automation requests, scene files) could nest deeply enough to overflow
	// the stack when copied or dumped.
	TEST_CASE("ParseJson rejects syntax errors and excessive nesting")
	{
		CHECK(ParseJson(R"({"a": [1, 2, {"b": true}]})")["a"][2]["b"] == true);
		CHECK(ParseJson("{ broken").is_discarded());

		auto nested = [](int depth) { return std::string(static_cast<size_t>(depth), '[') + std::string(static_cast<size_t>(depth), ']'); };
		CHECK_FALSE(ParseJson(nested(MaxJsonDepth)).is_discarded());
		CHECK(ParseJson(nested(MaxJsonDepth + 1)).is_discarded());
		CHECK(ParseJson(nested(1000000)).is_discarded());
	}

	// The test binary doubles as the child (TestMain.cpp: --basalt-child), so no shell is needed on any platform.
	ProcessSpec ChildProcess(std::vector<std::string> actions)
	{
		ProcessSpec spec;
#if defined(_WIN32)
		spec.Executable = FileSystem::GetExecutableDirectory() / "BasaltTests.exe";
#else
		spec.Executable = FileSystem::GetExecutableDirectory() / "BasaltTests";
#endif
		spec.Arguments = { "--basalt-child" };
		spec.Arguments.insert(spec.Arguments.end(), actions.begin(), actions.end());
		spec.Timeout = std::chrono::milliseconds(30000);
		return spec;
	}

	TEST_CASE("RunProcess reports the exit code and captures stdout and stderr")
	{
		const ProcessResult result = RunProcess(ChildProcess({ "print", "to stdout", "error", "to stderr", "exit", "7" }));
		CHECK(result.Outcome == ProcessOutcome::Exited);
		CHECK(result.ExitCode == 7);
		CHECK_FALSE(result.Succeeded());
		CHECK(result.Output.find("to stdout") != std::string::npos);
		CHECK(result.Output.find("to stderr") != std::string::npos);
		CHECK(result.Error.empty());

		const ProcessResult success = RunProcess(ChildProcess({ "exit", "0" }));
		CHECK(success.Succeeded());
	}

#if defined(_WIN32)
	TEST_CASE("RunProcess refuses arguments that are not UTF-8 on Windows")
	{
		// Windows command lines are UTF-16: invalid bytes cannot be passed on unchanged, so nothing starts.
		const ProcessResult result = RunProcess(ChildProcess({ "print", "\xFF\xFE" }));
		CHECK(result.Outcome == ProcessOutcome::Failed);
		CHECK(result.Error.find("is not valid UTF-8 text") != std::string::npos);
	}
#endif

	TEST_CASE("RunProcess passes arguments with spaces, quotes and backslashes unchanged")
	{
		const std::vector<std::string> arguments = { "two words", "quote\"inside", "trailing\\", "back\\\"slash", "", "plain" };
		std::vector<std::string> actions = { "args" };
		actions.insert(actions.end(), arguments.begin(), arguments.end());
		const ProcessResult result = RunProcess(ChildProcess(actions));
		REQUIRE(result.Succeeded());
		std::string expected;
		for (const std::string& argument : arguments)
			expected += "[" + argument + "]\n";
		std::string output = result.Output;
		std::erase(output, '\r');
		CHECK(output == expected);
	}

	TEST_CASE("RunProcess runs in the requested working directory")
	{
		BasaltTest::TempDirectory directory("ProcessWorkingDirectory");
		ProcessSpec spec = ChildProcess({ "cwd" });
		spec.WorkingDirectory = directory.GetPath();
		const ProcessResult result = RunProcess(spec);
		REQUIRE(result.Succeeded());
		CHECK(std::filesystem::equivalent(std::filesystem::path(result.Output.substr(0, result.Output.find_first_of("\r\n"))), directory.GetPath()));

		spec.WorkingDirectory = directory.GetPath() / "missing";
		const ProcessResult missing = RunProcess(spec);
		CHECK(missing.Outcome == ProcessOutcome::Failed);
		CHECK_FALSE(missing.Error.empty());
	}

	TEST_CASE("RunProcess passes the environment with overrides")
	{
		const BasaltTest::ScopedEnvVar kept("BASALT_PROCESS_TEST_KEPT", "kept");
		const BasaltTest::ScopedEnvVar unicode("BASALT_PROCESS_TEST_UNICODE", "J\xC3\xBCrgen");
		const BasaltTest::ScopedEnvVar replaced("BASALT_PROCESS_TEST_REPLACED", "old");
		ProcessSpec spec = ChildProcess({ "env", "BASALT_PROCESS_TEST_KEPT", "env", "BASALT_PROCESS_TEST_REPLACED", "env", "BASALT_PROCESS_TEST_ADDED", "env", "BASALT_PROCESS_TEST_UNICODE" });
		spec.Environment = { { "BASALT_PROCESS_TEST_REPLACED", "new value" }, { "BASALT_PROCESS_TEST_ADDED", "added \xC3\xA9" } };
		const ProcessResult result = RunProcess(spec);
		REQUIRE(result.Succeeded());
		CHECK(result.Output.find("BASALT_PROCESS_TEST_KEPT=kept") != std::string::npos);
		CHECK(result.Output.find("BASALT_PROCESS_TEST_REPLACED=new value") != std::string::npos);
		CHECK(result.Output.find("BASALT_PROCESS_TEST_ADDED=added \xC3\xA9") != std::string::npos);
		CHECK(result.Output.find("BASALT_PROCESS_TEST_UNICODE=J\xC3\xBCrgen") != std::string::npos);
		CHECK(result.Output.find("=old") == std::string::npos);
	}

	TEST_CASE("RunProcess stops a child whose output passes the file limit")
	{
		// 4 GiB would fill a small disk: the runner must stop it long before (the timeout is only a safety net).
		ProcessSpec spec = ChildProcess({ "flood", "4294967296" });
		spec.MaxOutputFileBytes = uint64_t{ 1 } * 1024 * 1024;
		spec.Timeout = std::chrono::seconds(20);
		const ProcessResult result = RunProcess(spec);
		CHECK(result.Outcome == ProcessOutcome::OutputLimit);
		CHECK(result.Error.find("more than 1048576 bytes of output") != std::string::npos);
		CHECK(result.Output.size() == spec.MaxOutputBytes);
		CHECK(result.Output.find_first_not_of('y') == std::string::npos);
	}

	TEST_CASE("RunProcess keeps the end of long output")
	{
		ProcessSpec spec = ChildProcess({ "spam", "100000", "print", "THE END" });
		spec.MaxOutputBytes = 1000;
		const ProcessResult result = RunProcess(spec);
		REQUIRE(result.Succeeded());
		CHECK(result.Output.size() == 1000);
		CHECK(result.Output.find("THE END") != std::string::npos);
	}

	TEST_CASE("RunProcess kills a child that outlives its timeout")
	{
		ProcessSpec spec = ChildProcess({ "print", "started", "sleep", "20000", "exit", "0" });
		// Long enough for a Debug or sanitizer build of the child to start and print on a busy CI machine.
		spec.Timeout = std::chrono::milliseconds(5000);
		const auto start = std::chrono::steady_clock::now();
		const ProcessResult result = RunProcess(spec);
		CHECK(result.Outcome == ProcessOutcome::TimedOut);
		CHECK_FALSE(result.Succeeded());
		CHECK(result.Output.find("started") != std::string::npos);
		CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(20));
	}

	TEST_CASE("RunProcess reports a crash as a crash, never as an exit")
	{
		for (const char* action : { "crash", "trap" })
		{
			INFO(action);
			const ProcessResult result = RunProcess(ChildProcess({ "print", "before the crash", action }));
			CHECK(result.Outcome == ProcessOutcome::Crashed);
			CHECK_FALSE(result.Succeeded());
			CHECK_FALSE(result.Error.empty());
			CHECK(result.Output.find("before the crash") != std::string::npos);
#if !defined(_WIN32)
			CHECK(result.ExitCode > 128);
			CHECK(result.Error.find("signal") != std::string::npos);
#endif
		}
#if defined(_WIN32)
		const ProcessResult violation = RunProcess(ChildProcess({ "crash" }));
		CHECK(static_cast<uint32_t>(violation.ExitCode) == 0xC0000005u);
		CHECK(violation.Error.find("0xC0000005") != std::string::npos);
#else
		CHECK(RunProcess(ChildProcess({ "crash" })).ExitCode == 128 + SIGABRT);
#endif
		// A negative exit code is an exit, not a crash, on every platform.
		const ProcessResult negative = RunProcess(ChildProcess({ "exit", "-1" }));
		CHECK(negative.Outcome == ProcessOutcome::Exited);
		CHECK_FALSE(negative.Succeeded());
	}

	TEST_CASE("RunProcess output is valid UTF-8 even when the tail starts inside a character")
	{
		// 1001 two-byte characters, cut to 1001 bytes: the first byte kept is the second half of a character.
		ProcessSpec spec = ChildProcess({ "utf8", "1001", "invalid" });
		spec.MaxOutputBytes = 1001 + 4;
		const ProcessResult result = RunProcess(spec);
		REQUIRE(result.Succeeded());
		CHECK(result.Output.rfind("\xC3\xA9", 0) == 0);
		CHECK(result.Output.find("[\xEF\xBF\xBD\xEF\xBF\xBD]") != std::string::npos);
		CHECK(nlohmann::json(result.Output).dump().size() > 0);
	}

#if !defined(_WIN32)
	TEST_CASE("RunProcess does not pass the caller's other open files to the child")
	{
		// A descriptor without close-on-exec, as a library or a socket might leave it.
		int ends[2] = { -1, -1 };
		REQUIRE(pipe(ends) == 0);
		const ProcessResult result = RunProcess(ChildProcess({ "fd", std::to_string(ends[1]).c_str(), "fd", "1" }));
		close(ends[0]);
		close(ends[1]);
		REQUIRE(result.Succeeded());
		CHECK(result.Output.find("fd closed\nfd open") != std::string::npos);
	}
#endif

	TEST_CASE("RunProcess rejects environment names it cannot pass on")
	{
		ProcessSpec spec = ChildProcess({ "exit", "0" });
		for (const char* name : { "", "A=B" })
		{
			spec.Environment = { { name, "value" } };
			const ProcessResult result = RunProcess(spec);
			CHECK(result.Outcome == ProcessOutcome::Failed);
			CHECK(result.Error.find("invalid environment variable name") != std::string::npos);
		}
		// Which of two values for one name wins would differ between platforms (Windows ignores case).
		spec.Environment = { { "BASALT_TEST_VALUE", "a" }, { "basalt_test_value", "b" } };
		ProcessResult result = RunProcess(spec);
		CHECK(result.Outcome == ProcessOutcome::Failed);
		CHECK(result.Error.find("set twice") != std::string::npos);
		spec.Environment.clear();
		spec.Timeout = std::chrono::milliseconds(0);
		result = RunProcess(spec);
		CHECK(result.Outcome == ProcessOutcome::Failed);
		CHECK(result.Error.find("timeout must be positive") != std::string::npos);
	}

	TEST_CASE("RunProcess resolves a relative executable against the caller's directory, not the child's")
	{
		BasaltTest::TempDirectory directory("ProcessRelativeExecutable");
		ProcessSpec spec = ChildProcess({ "exit", "0" });
		std::error_code error;
		spec.Executable = std::filesystem::relative(spec.Executable, std::filesystem::current_path(), error);
		REQUIRE_FALSE(error);
		REQUIRE_FALSE(spec.Executable.empty());
		spec.WorkingDirectory = directory.GetPath();
		CHECK(RunProcess(spec).Succeeded());
	}

	TEST_CASE("RunProcess distinguishes a program that cannot start from one that fails")
	{
		ProcessSpec spec;
		spec.Executable = FileSystem::GetExecutableDirectory() / "NoSuchProgram-Basalt";
		const ProcessResult result = RunProcess(spec);
		CHECK(result.Outcome == ProcessOutcome::Failed);
		CHECK_FALSE(result.Succeeded());
		CHECK(result.Error.find("cannot start") != std::string::npos);
	}
}

TEST_SUITE("Core")
{
	TEST_CASE("Log history listeners see every message pushed until they are removed")
	{
		LogHistory history(4);
		std::vector<std::string> seen;
		const uint64_t first = history.AddListener([&](const LogMessage& message) { seen.push_back(message.Text); });
		const uint64_t second = history.AddListener([&](const LogMessage& message) { seen.push_back("2:" + message.Text); });
		CHECK(first != second);
		history.Push({ LogLevel::Info, "BASALT", "a" });
		history.RemoveListener(first);
		history.Push({ LogLevel::Error, "BASALT", "b" });
		history.RemoveListener(second);
		history.RemoveListener(second);
		history.Push({ LogLevel::Warn, "BASALT", "c" });
		CHECK(seen == std::vector<std::string>{ "a", "2:a", "2:b" });
		CHECK(history.GetTotalCount() == 3);
	}
}
