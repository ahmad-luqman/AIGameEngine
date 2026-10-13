#include <doctest/doctest.h>

#include <Basalt/Core/FileSystem.h>
#include <Basalt/Core/GamepadCodes.h>
#include <Basalt/Core/Input.h>
#include <Basalt/Core/JsonUtils.h>
#include <Basalt/Core/KeyCodes.h>
#include <Basalt/Core/LayerStack.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Core/MouseCodes.h>
#include <Basalt/Core/UUID.h>
#include <Basalt/Events/ApplicationEvent.h>
#include <Basalt/Events/KeyEvent.h>
#include <Basalt/Events/MouseEvent.h>

#include <filesystem>
#include <limits>
#include <unordered_set>
#include <vector>

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

		Input::SetGamepadConnected(1, true, "Pad");
		CHECK(Input::IsGamepadConnected(1));
		CHECK(Input::WasGamepadConnected(1));
		CHECK(Input::GetGamepadName(1) == "Pad");
		CHECK(Input::SetGamepadButtonState(1, GamepadButton::A, true));
		CHECK(Input::IsGamepadButtonDown(1, GamepadButton::A));
		CHECK(Input::IsGamepadButtonPressed(1, GamepadButton::A));
		CHECK_FALSE(Input::IsGamepadButtonDown(0, GamepadButton::A));
		Input::EndFrame();
		CHECK_FALSE(Input::WasGamepadConnected(1));
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
		CHECK_FALSE(Input::WasGamepadConnected(1));
		CHECK_FALSE(Input::IsGamepadButtonDown(1, GamepadButton::A));
		CHECK_FALSE(Input::IsGamepadButtonReleased(1, GamepadButton::A));
		CHECK(Input::GetGamepadAxis(1, GamepadAxis::LeftX) == 0.0f);

		// Disconnecting while a button is held reports the release and the disconnect on that frame.
		CHECK(Input::SetGamepadButtonState(1, GamepadButton::B, true));
		Input::EndFrame();
		Input::SetGamepadConnected(1, false);
		CHECK_FALSE(Input::IsGamepadConnected(1));
		CHECK(Input::WasGamepadDisconnected(1));
		CHECK(Input::IsGamepadButtonReleased(1, GamepadButton::B));
		CHECK_FALSE(Input::IsGamepadButtonDown(1, GamepadButton::B));
		Input::EndFrame();
		CHECK_FALSE(Input::WasGamepadDisconnected(1));
		CHECK_FALSE(Input::IsGamepadButtonReleased(1, GamepadButton::B));
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
		CHECK(Input::WasGamepadConnected(2));
		CHECK(Input::GetGamepadName(2) == "Controller");
		CHECK(Input::IsGamepadButtonPressed(2, GamepadButton::Start));
		CHECK(Input::GetGamepadAxis(2, GamepadAxis::LeftY) == 1.0f);
		Input::EndFrame();
		Input::UpdateGamepadFromDevice(2, nullptr);
		CHECK(Input::WasGamepadDisconnected(2));
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

		// Forgetting injected gamepads reports no disconnect, and a reconnection on the same frame is reported
		// (a replay starts from no gamepads). Device gamepads are not forgotten.
		Input::SetGamepadConnected(4, true);
		Input::UpdateGamepadFromDevice(5, &device);
		Input::EndFrame();
		Input::ForgetInjectedGamepads();
		CHECK_FALSE(Input::IsGamepadConnected(4));
		CHECK_FALSE(Input::WasGamepadDisconnected(4));
		CHECK(Input::IsGamepadConnected(5));
		Input::SetGamepadConnected(4, true);
		CHECK(Input::WasGamepadConnected(4));
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
		device.Buttons[static_cast<size_t>(GamepadButton::A)] = true;
		device.Axes[static_cast<size_t>(GamepadAxis::LeftX)] = 0.9f;
		Input::UpdateGamepadFromDevice(0, &device);
		CHECK(Input::GetGamepadSource(0) == GamepadSource::Device);
		Input::EndFrame();
		CHECK(Input::IsGamepadButtonDown(0, GamepadButton::A));

		// Focus loss: released, and the next poll (A and the stick still held) does not press them again.
		Input::Reset();
		Input::UpdateGamepadFromDevice(0, &device);
		CHECK_FALSE(Input::IsGamepadButtonDown(0, GamepadButton::A));
		CHECK_FALSE(Input::IsGamepadButtonPressed(0, GamepadButton::A));
		CHECK(Input::GetGamepadAxis(0, GamepadAxis::LeftX) == 0.0f);
		Input::EndFrame();
		// Let go (the stick back near the center), then pressed again: a real press.
		device.Buttons[static_cast<size_t>(GamepadButton::A)] = false;
		device.Axes[static_cast<size_t>(GamepadAxis::LeftX)] = 0.1f;
		Input::UpdateGamepadFromDevice(0, &device);
		CHECK(Input::GetGamepadAxis(0, GamepadAxis::LeftX) == doctest::Approx(0.1f));
		Input::EndFrame();
		device.Buttons[static_cast<size_t>(GamepadButton::A)] = true;
		device.Axes[static_cast<size_t>(GamepadAxis::LeftX)] = 0.9f;
		Input::UpdateGamepadFromDevice(0, &device);
		CHECK(Input::IsGamepadButtonPressed(0, GamepadButton::A));
		CHECK(Input::GetGamepadAxis(0, GamepadAxis::LeftX) == doctest::Approx(0.9f));

		// Injecting over a physical gamepad takes it over, released; it cannot be driven while it is the device's.
		Input::EndFrame();
		Input::Reset();
		Input::UpdateGamepadFromDevice(1, &device);
		CHECK_FALSE(Input::SetGamepadButtonState(1, GamepadButton::B, true));
		Input::SetGamepadConnected(1, true, "Test");
		CHECK(Input::GetGamepadSource(1) == GamepadSource::Injected);
		CHECK_FALSE(Input::IsGamepadButtonDown(1, GamepadButton::A));
		CHECK(Input::SetGamepadButtonState(1, GamepadButton::B, true));

		// With device gamepads off (replays), physical ones are forgotten and polls ignored.
		Input::SetDeviceGamepadsEnabled(false);
		CHECK_FALSE(Input::IsGamepadConnected(0));
		CHECK_FALSE(Input::WasGamepadDisconnected(0));
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
}
