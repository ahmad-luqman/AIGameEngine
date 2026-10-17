#include <doctest/doctest.h>

#include <Basalt/Core/FileSystem.h>
#include <Basalt/Core/FileWatcher.h>
#include <Basalt/Core/GamepadCodes.h>
#include <Basalt/Core/Input.h>
#include <Basalt/Core/InputActions.h>
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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
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

	TEST_CASE("Input reads the previous frame's keys, buttons and gamepads")
	{
		BasaltTest::ScopedGameInput restore;
		Input::SetKeyState(KeyCode::Q, true);
		Input::SetMouseButtonState(MouseCode::Right, true);
		Input::SetGamepadConnected(2, true);
		REQUIRE(Input::SetGamepadButtonState(2, GamepadButton::Y, true));
		REQUIRE(Input::SetGamepadAxis(2, GamepadAxis::RightX, 0.75f));
		CHECK_FALSE(Input::IsKeyDown(KeyCode::Q, InputFrame::Previous));
		CHECK_FALSE(Input::IsMouseButtonDown(MouseCode::Right, InputFrame::Previous));
		CHECK_FALSE(Input::IsGamepadButtonDown(2, GamepadButton::Y, InputFrame::Previous));
		CHECK(Input::GetGamepadAxis(2, GamepadAxis::RightX, InputFrame::Previous) == 0.0f);
		Input::EndFrame();
		Input::SetKeyState(KeyCode::Q, false);
		CHECK(Input::IsKeyDown(KeyCode::Q, InputFrame::Previous));
		CHECK(Input::IsMouseButtonDown(MouseCode::Right, InputFrame::Previous));
		CHECK(Input::IsGamepadButtonDown(2, GamepadButton::Y, InputFrame::Previous));
		CHECK(Input::GetGamepadAxis(2, GamepadAxis::RightX, InputFrame::Previous) == 0.75f);
		// A gamepad disconnected since still reads what it held then.
		Input::SetGamepadConnected(2, false);
		CHECK(Input::IsGamepadButtonDown(2, GamepadButton::Y, InputFrame::Previous));
		CHECK(Input::GetGamepadAxis(2, GamepadAxis::RightX, InputFrame::Previous) == 0.75f);
		CHECK_FALSE(Input::IsGamepadButtonDown(2, GamepadButton::Y));
		// A reset forgets the previous frame too (no release is reported).
		Input::Reset();
		CHECK_FALSE(Input::IsMouseButtonDown(MouseCode::Right, InputFrame::Previous));
		CHECK_FALSE(Input::IsKeyDown(KeyCode::Q, InputFrame::Previous));
		CHECK_FALSE(Input::IsKeyDown(static_cast<KeyCode>(9999), InputFrame::Previous));
		CHECK(Input::GetGamepadAxis(Input::MaxGamepads, GamepadAxis::LeftX, InputFrame::Previous) == 0.0f);
	}

	TEST_CASE("GameInput equals Input with full routing")
	{
		BasaltTest::ScopedGameInput restore;
		Input::SetKeyState(KeyCode::W, true);
		Input::SetMouseButtonState(MouseCode::Left, true);
		Input::SetMousePosition({ 10.0f, 20.0f });
		Input::EndFrame();
		Input::SetMousePosition({ 13.0f, 24.0f });
		Input::AddMouseScroll({ 0.0f, 1.0f });
		Input::SetKeyState(KeyCode::E, true);
		CHECK(GameInput::IsKeyDown(KeyCode::W));
		CHECK(GameInput::IsKeyPressed(KeyCode::E));
		CHECK_FALSE(GameInput::IsKeyPressed(KeyCode::W));
		CHECK(GameInput::IsKeyDown(KeyCode::W, InputFrame::Previous));
		CHECK_FALSE(GameInput::IsKeyDown(KeyCode::E, InputFrame::Previous));
		CHECK(GameInput::IsMouseButtonDown(MouseCode::Left));
		CHECK(GameInput::GetMouseDelta() == glm::vec2(3.0f, 4.0f));
		CHECK(GameInput::GetMouseScroll() == glm::vec2(0.0f, 1.0f));
		Input::SetKeyState(KeyCode::W, false);
		CHECK(GameInput::IsKeyReleased(KeyCode::W));
		Input::SetMouseButtonState(MouseCode::Left, false);
		CHECK(GameInput::IsMouseButtonReleased(MouseCode::Left));
		CHECK_FALSE(GameInput::IsKeyDown(static_cast<KeyCode>(9999)));
		CHECK_FALSE(GameInput::IsMouseButtonPressed(static_cast<MouseCode>(200)));
	}

	TEST_CASE("Losing game focus releases the keyboard for the game; what is held when it comes back stays released")
	{
		BasaltTest::ScopedGameInput restore;
		Input::SetKeyState(KeyCode::W, true);
		CHECK(GameInput::IsKeyPressed(KeyCode::W));
		Input::EndFrame();

		// Focus moves to another panel: the host still sees W, the game sees it let go once.
		Input::SetGameFocus(false);
		CHECK_FALSE(Input::HasGameFocus());
		CHECK(Input::IsKeyDown(KeyCode::W));
		CHECK_FALSE(GameInput::IsKeyDown(KeyCode::W));
		CHECK(GameInput::IsKeyReleased(KeyCode::W));
		Input::EndFrame();
		CHECK_FALSE(GameInput::IsKeyReleased(KeyCode::W));
		// Keys pressed elsewhere never reach the game.
		Input::SetKeyState(KeyCode::S, true);
		CHECK_FALSE(GameInput::IsKeyPressed(KeyCode::S));
		Input::EndFrame();

		// Focus comes back while both keys are held: neither presses by itself.
		Input::SetGameFocus(true);
		CHECK_FALSE(GameInput::IsKeyDown(KeyCode::W));
		CHECK_FALSE(GameInput::IsKeyPressed(KeyCode::S));
		Input::EndFrame();
		CHECK_FALSE(GameInput::IsKeyDown(KeyCode::W));
		// Let go: no release either (the game never saw it held); a new press is the game's.
		Input::SetKeyState(KeyCode::W, false);
		CHECK_FALSE(GameInput::IsKeyReleased(KeyCode::W));
		Input::EndFrame();
		Input::SetKeyState(KeyCode::W, true);
		CHECK(GameInput::IsKeyPressed(KeyCode::W));
		// S was let go between two frames (the block ends at EndFrame) and is pressed again.
		Input::SetKeyState(KeyCode::S, false);
		Input::EndFrame();
		Input::SetKeyState(KeyCode::S, true);
		CHECK(GameInput::IsKeyPressed(KeyCode::S));
	}

	TEST_CASE("The game's pointer gates mouse buttons, movement and scroll but not the position")
	{
		BasaltTest::ScopedGameInput restore;
		Input::SetMousePosition({ 0.0f, 0.0f });
		Input::SetMouseButtonState(MouseCode::Left, true);
		Input::EndFrame();
		Input::SetGamePointer(false);
		CHECK_FALSE(Input::HasGamePointer());
		Input::SetMousePosition({ 5.0f, 5.0f });
		Input::AddMouseScroll({ 1.0f, 0.0f });
		CHECK(GameInput::GetMouseDelta() == glm::vec2(0.0f));
		CHECK(GameInput::GetMouseScroll() == glm::vec2(0.0f));
		CHECK(Input::GetMouseDelta() == glm::vec2(5.0f));
		CHECK(Input::GetMousePosition() == glm::vec2(5.0f));
		CHECK(GameInput::IsMouseButtonReleased(MouseCode::Left));
		Input::EndFrame();
		// The pointer comes back with the button held: it is not the game's until let go.
		Input::SetGamePointer(true);
		CHECK_FALSE(GameInput::IsMouseButtonDown(MouseCode::Left));
		CHECK_FALSE(GameInput::IsMouseButtonPressed(MouseCode::Left));
		Input::SetMouseButtonState(MouseCode::Left, false);
		Input::EndFrame();
		Input::SetMouseButtonState(MouseCode::Left, true);
		CHECK(GameInput::IsMouseButtonPressed(MouseCode::Left));
		// Keyboard focus does not gate the mouse.
		Input::SetGameFocus(false);
		CHECK(GameInput::IsMouseButtonDown(MouseCode::Left));
	}

	TEST_CASE("Game focus gates gamepads like the keyboard, sticks until they return to rest")
	{
		BasaltTest::ScopedGameInput restore;
		Input::SetGamepadConnected(0, true);
		REQUIRE(Input::SetGamepadButtonState(0, GamepadButton::A, true));
		REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::LeftX, 1.0f));
		CHECK(GameInput::IsGamepadButtonPressed(0, GamepadButton::A));
		CHECK(GameInput::GetGamepadAxis(0, GamepadAxis::LeftX) == 1.0f);
		Input::EndFrame();
		CHECK(GameInput::GetGamepadAxis(0, GamepadAxis::LeftX, InputFrame::Previous) == 1.0f);

		Input::SetGameFocus(false);
		CHECK_FALSE(GameInput::IsGamepadButtonDown(0, GamepadButton::A));
		CHECK(GameInput::IsGamepadButtonReleased(0, GamepadButton::A));
		CHECK(GameInput::GetGamepadAxis(0, GamepadAxis::LeftX) == 0.0f);
		CHECK(Input::IsGamepadButtonDown(0, GamepadButton::A));
		Input::EndFrame();
		CHECK(GameInput::GetGamepadAxis(0, GamepadAxis::LeftX, InputFrame::Previous) == 0.0f);

		Input::SetGameFocus(true);
		CHECK_FALSE(GameInput::IsGamepadButtonPressed(0, GamepadButton::A));
		CHECK(GameInput::GetGamepadAxis(0, GamepadAxis::LeftX) == 0.0f);
		// A stick still pushed past rest stays blocked; back near the center it is the game's again.
		REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::LeftX, 0.5f));
		Input::EndFrame();
		CHECK(GameInput::GetGamepadAxis(0, GamepadAxis::LeftX) == 0.0f);
		REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::LeftX, 0.1f));
		REQUIRE(Input::SetGamepadButtonState(0, GamepadButton::A, false));
		Input::EndFrame();
		CHECK(GameInput::GetGamepadAxis(0, GamepadAxis::LeftX) == doctest::Approx(0.1f));
		REQUIRE(Input::SetGamepadButtonState(0, GamepadButton::A, true));
		CHECK(GameInput::IsGamepadButtonPressed(0, GamepadButton::A));
		Input::EndFrame();

		// A disconnect reports the release to the game too.
		Input::SetGamepadConnected(0, false);
		CHECK(GameInput::IsGamepadButtonReleased(0, GamepadButton::A));
		CHECK_FALSE(GameInput::IsGamepadButtonDown(0, GamepadButton::A));
		CHECK(GameInput::IsGamepadButtonDown(0, GamepadButton::A, InputFrame::Previous));
		CHECK_FALSE(GameInput::IsGamepadButtonReleased(Input::MaxGamepads, GamepadButton::A));
	}

	TEST_CASE("A reset clears what the game saw without reporting releases")
	{
		BasaltTest::ScopedGameInput restore;
		Input::SetKeyState(KeyCode::D, true);
		Input::EndFrame();
		Input::SetGameFocus(false);
		Input::SetGameFocus(true);
		Input::Reset();
		CHECK_FALSE(GameInput::IsKeyReleased(KeyCode::D));
		CHECK_FALSE(GameInput::IsKeyDown(KeyCode::D, InputFrame::Previous));
		// The block went with the reset: pressing D again is the game's at once.
		Input::SetKeyState(KeyCode::D, true);
		CHECK(GameInput::IsKeyPressed(KeyCode::D));
		// Routing is a setting, kept by a reset.
		Input::SetGameFocus(false);
		Input::Reset();
		CHECK_FALSE(Input::HasGameFocus());
	}

	TEST_CASE("Cursor modes have names and are stored, not applied, by Input")
	{
		BasaltTest::ScopedGameInput restore;
		CHECK(CursorModeToString(CursorMode::Locked) == "Locked");
		CHECK(CursorModeToString(static_cast<CursorMode>(7)) == "Unknown");
		CHECK(CursorModeFromString("hidden") == CursorMode::Hidden);
		CHECK(CursorModeFromString("NORMAL") == CursorMode::Normal);
		CHECK_FALSE(CursorModeFromString("Captured").has_value());
		CHECK_FALSE(CursorModeFromString("").has_value());
		CHECK(Input::GetCursorMode() == CursorMode::Normal);
		Input::SetCursorMode(CursorMode::Locked);
		Input::Reset();
		CHECK(Input::GetCursorMode() == CursorMode::Locked);
	}

	TEST_CASE("Input action maps round-trip through JSON with canonical names and only non-default fields")
	{
		const nlohmann::json data = {
			{ "Actions",
			  {
				  { { "Name", "Jump" }, { "Bindings", { { { "Key", "space" } }, { { "GamepadButton", "a" } }, { { "MouseButton", "LEFT" } } } } },
				  { { "Name", "MoveX" }, { "Type", "Axis" }, { "Bindings", { { { "Key", "D" } }, { { "Key", "A" }, { "Scale", -1 } }, { { "GamepadAxis", "LeftX" }, { "DeadZone", 0.1 }, { "Gamepad", 1 } } } } },
				  { { "Name", "Idle" } },
			  } },
		};
		std::string error;
		const auto map = InputActionMap::FromJson(data, error);
		REQUIRE_MESSAGE(map, error);
		REQUIRE(map->GetActions().size() == 3);
		const InputAction* jump = map->Find("Jump");
		REQUIRE(jump);
		CHECK(jump->Type == InputActionType::Button);
		REQUIRE(jump->Bindings.size() == 3);
		CHECK(std::get<KeyCode>(jump->Bindings[0].Control) == KeyCode::Space);
		CHECK(std::get<GamepadButton>(jump->Bindings[1].Control) == GamepadButton::A);
		CHECK(std::get<MouseCode>(jump->Bindings[2].Control) == MouseCode::Left);
		const InputAction* move = map->Find("MoveX");
		REQUIRE(move);
		CHECK(move->Type == InputActionType::Axis);
		CHECK(move->Bindings[1].Scale == -1.0f);
		CHECK(move->Bindings[2].DeadZone == 0.1f);
		CHECK(move->Bindings[2].Gamepad == 1u);
		CHECK(map->Find("Idle")->Bindings.empty());
		CHECK_FALSE(map->Find("jump"));

		const nlohmann::json saved = map->ToJson();
		CHECK(saved["Format"] == "BasaltInput");
		CHECK(saved["Version"] == 1);
		CHECK(saved["Actions"][0]["Type"] == "Button");
		CHECK(saved["Actions"][0]["Bindings"][0] == nlohmann::json{ { "Key", "Space" } });
		CHECK(saved["Actions"][0]["Bindings"][2] == nlohmann::json{ { "MouseButton", "Left" } });
		CHECK(saved["Actions"][1]["Bindings"][2] == nlohmann::json{ { "GamepadAxis", "LeftX" }, { "DeadZone", 0.1f }, { "Gamepad", 1 } });
		const auto reloaded = InputActionMap::FromJson(saved, error);
		REQUIRE(reloaded);
		CHECK(*reloaded == *map);
		CHECK(InputActionMap::FromJson(nlohmann::json::object(), error)->IsEmpty());
		CHECK(InputActionTypeToString(static_cast<InputActionType>(9)) == "Unknown");
		CHECK_FALSE(InputActionTypeFromString("axis").has_value());
	}

	TEST_CASE("Input action maps reject malformed data with a reason")
	{
		const auto action = [](nlohmann::json fields) {
			if (!fields.contains("Name"))
				fields["Name"] = "A";
			return nlohmann::json{ { "Actions", nlohmann::json::array({ fields }) } };
		};
		const auto binding = [&action](const nlohmann::json& fields) { return action({ { "Bindings", nlohmann::json::array({ fields }) } }); };
		nlohmann::json tooManyActions = { { "Actions", nlohmann::json::array() } };
		for (uint32_t i = 0; i <= InputActionMap::MaxActions; i++)
			tooManyActions["Actions"].push_back({ { "Name", "A" + std::to_string(i) } });
		nlohmann::json tooManyBindings = nlohmann::json::array();
		for (uint32_t i = 0; i <= InputActionMap::MaxBindings; i++)
			tooManyBindings.push_back({ { "Key", "A" } });
		const std::vector<std::pair<nlohmann::json, const char*>> cases = {
			{ nlohmann::json::array(), "must be an object" },
			{ { { "Actions", {} }, { "Extra", 1 } }, "unknown key 'Extra'" },
			{ { { "Format", "BasaltScene" } }, "Format must be \"BasaltInput\"" },
			{ { { "Version", 2 } }, "unsupported Version" },
			{ { { "Actions", nlohmann::json::array({ "Jump" }) } }, "input action #1 must be an object" },
			{ { { "Actions", "Jump" } }, "Actions must be an array" },
			{ tooManyActions, "at most 256 actions" },
			{ action({ { "Name", "" } }), "1 to 64 characters" },
			{ action({ { "Name", std::string(65, 'x') } }), "1 to 64 characters" },
			{ action({ { "Name", "Tab\there" } }), "control characters" },
			{ action({ { "Name", 5 } }), "Name must be a string" },
			{ { { "Actions", { { { "Name", "A" } }, { { "Name", "A" } } } } }, "duplicate action 'A'" },
			{ action({ { "Type", "Trigger" } }), "Type must be \"Button\" or \"Axis\"" },
			{ action({ { "Speed", 2 } }), "unknown key 'Speed'" },
			{ action({ { "Bindings", nlohmann::json::array({ "Space" }) } }), "a binding must be an object" },
			{ action({ { "Bindings", tooManyBindings } }), "at most 16 bindings" },
			{ action({ { "Bindings", "Space" } }), "Bindings must be an array" },
			{ binding(nlohmann::json::object()), "needs one of Key" },
			{ binding({ { "Key", "A" }, { "GamepadButton", "A" } }), "one control, not both" },
			{ binding({ { "Key", "Hyperspace" } }), "unknown Key 'Hyperspace'" },
			{ binding({ { "GamepadAxis", 3 } }), "GamepadAxis must be a control name" },
			{ binding({ { "Key", "A" }, { "Scale", 2 } }), "Scale must be a number in [-1, 1]" },
			{ binding({ { "Key", "A" }, { "Scale", "big" } }), "Scale must be a number" },
			{ binding({ { "Key", "A" }, { "DeadZone", 0.1 } }), "GamepadAxis bindings only" },
			{ binding({ { "GamepadAxis", "LeftX" }, { "DeadZone", 1 } }), "DeadZone must be a number in [0, 0.95]" },
			{ binding({ { "MouseButton", "Left" }, { "Gamepad", 0 } }), "GamepadButton and GamepadAxis bindings only" },
			{ binding({ { "GamepadButton", "A" }, { "Gamepad", 16 } }), "integer from 0 to 15" },
			{ binding({ { "GamepadButton", "A" }, { "Gamepad", -1 } }), "integer from 0 to 15" },
			{ binding({ { "Key", "A" }, { "Repeat", true } }), "unknown key 'Repeat'" },
		};
		for (const auto& [data, expected] : cases)
		{
			std::string error;
			CHECK_FALSE_MESSAGE(InputActionMap::FromJson(data, error).has_value(), data.dump());
			CHECK_MESSAGE(error.find(expected) != std::string::npos, data.dump(), " -> ", error);
		}
	}

	TEST_CASE("Input actions read buttons, axes, dead zones and gamepad indices through GameInput")
	{
		BasaltTest::ScopedGameInput restore;
		std::string error;
		const auto map = InputActionMap::FromJson(
			{ { "Actions",
				{
					{ { "Name", "Jump" }, { "Bindings", { { { "Key", "Space" } }, { { "GamepadButton", "A" } } } } },
					{ { "Name", "Fire" }, { "Bindings", { { { "GamepadAxis", "RightTrigger" }, { "DeadZone", 0 } }, { { "MouseButton", "Left" }, { "Scale", 0.25 } } } } },
					{ { "Name", "Up" }, { "Bindings", { { { "GamepadAxis", "LeftY" }, { "Scale", -1 } } } } },
					{ { "Name", "MoveX" }, { "Type", "Axis" }, { "Bindings", { { { "Key", "D" } }, { { "Key", "A" }, { "Scale", -1 } }, { { "Key", "Right" } }, { { "GamepadAxis", "LeftX" }, { "DeadZone", 0.5 } } } } },
					{ { "Name", "P2Jump" }, { "Bindings", { { { "GamepadButton", "A" }, { "Gamepad", 1 } } } } },
				} } },
			error);
		REQUIRE_MESSAGE(map, error);
		const InputAction& jump = *map->Find("Jump");
		const InputAction& fire = *map->Find("Fire");
		const InputAction& up = *map->Find("Up");
		const InputAction& move = *map->Find("MoveX");
		const InputAction& p2Jump = *map->Find("P2Jump");

		CHECK(InputActionMap::GetValue(jump) == 0.0f);
		Input::SetKeyState(KeyCode::Space, true);
		CHECK(InputActionMap::GetValue(jump) == 1.0f);
		CHECK(InputActionMap::IsPressed(jump));
		CHECK_FALSE(InputActionMap::IsDown(jump, InputFrame::Previous));
		Input::EndFrame();
		CHECK(InputActionMap::IsDown(jump));
		CHECK_FALSE(InputActionMap::IsPressed(jump));
		Input::SetKeyState(KeyCode::Space, false);
		CHECK(InputActionMap::IsReleased(jump));

		// Any gamepad presses an unindexed binding; an indexed one reads its gamepad only.
		Input::SetGamepadConnected(0, true);
		Input::SetGamepadConnected(1, true);
		REQUIRE(Input::SetGamepadButtonState(0, GamepadButton::A, true));
		CHECK(InputActionMap::IsDown(jump));
		CHECK_FALSE(InputActionMap::IsDown(p2Jump));
		REQUIRE(Input::SetGamepadButtonState(1, GamepadButton::A, true));
		CHECK(InputActionMap::IsDown(p2Jump));

		// A trigger gives a strength; a button's strength is its strongest binding, clamped (Scale 0.25 never presses).
		REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::RightTrigger, 0.4f));
		CHECK(InputActionMap::GetValue(fire) == doctest::Approx(0.4f));
		CHECK_FALSE(InputActionMap::IsDown(fire));
		Input::SetMouseButtonState(MouseCode::Left, true);
		CHECK(InputActionMap::GetValue(fire) == doctest::Approx(0.4f));
		REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::RightTrigger, 0.0f));
		CHECK(InputActionMap::GetValue(fire) == doctest::Approx(0.25f));
		// A negative scale turns a stick pushed up (-Y) into a button; pushed down it reads 0, not negative.
		REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::LeftY, -0.9f));
		CHECK(InputActionMap::GetValue(up) > 0.85f);
		REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::LeftY, 0.9f));
		CHECK(InputActionMap::GetValue(up) == 0.0f);

		// Axes sum their bindings and clamp: opposite keys cancel, two keys one way do not add past full.
		Input::SetKeyState(KeyCode::D, true);
		CHECK(InputActionMap::GetValue(move) == 1.0f);
		Input::SetKeyState(KeyCode::A, true);
		CHECK(InputActionMap::GetValue(move) == 0.0f);
		CHECK_FALSE(InputActionMap::IsDown(move));
		Input::SetKeyState(KeyCode::A, false);
		Input::SetKeyState(KeyCode::Right, true);
		CHECK(InputActionMap::GetValue(move) == 1.0f);
		Input::SetKeyState(KeyCode::D, false);
		Input::SetKeyState(KeyCode::Right, false);
		// The dead zone reads 0 inside it and rescales the rest to reach 1; the stick pushed furthest wins.
		REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::LeftX, -0.4f));
		CHECK(InputActionMap::GetValue(move) == 0.0f);
		REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::LeftX, -0.75f));
		CHECK(InputActionMap::GetValue(move) == doctest::Approx(-0.5f));
		REQUIRE(Input::SetGamepadAxis(1, GamepadAxis::LeftX, 1.0f));
		CHECK(InputActionMap::GetValue(move) == 1.0f);
		CHECK(InputActionMap::IsDown(move));

		// Actions see only what the host routes to the game.
		Input::EndFrame();
		Input::SetGameFocus(false);
		CHECK(InputActionMap::GetValue(move) == 0.0f);
		CHECK(InputActionMap::IsReleased(move));
		CHECK(InputActionMap::GetValue(fire) == doctest::Approx(0.25f));
		Input::SetGamePointer(false);
		CHECK(InputActionMap::GetValue(fire) == 0.0f);
	}

	TEST_CASE("Input actions can be rebound, but not created or overfilled")
	{
		std::string error;
		auto map = InputActionMap::FromJson({ { "Actions", { { { "Name", "Jump" }, { "Bindings", { { { "Key", "Space" } } } } } } } }, error);
		REQUIRE(map);
		const auto bindings = InputActionMap::BindingsFromJson({ { { "Key", "W" } }, { { "GamepadButton", "B" } } }, error);
		REQUIRE(bindings);
		CHECK(map->SetBindings("Jump", *bindings, error));
		CHECK(map->Find("Jump")->Bindings == *bindings);
		CHECK_FALSE(map->SetBindings("Dash", *bindings, error));
		CHECK(error.find("unknown input action 'Dash'") != std::string::npos);
		CHECK_FALSE(map->SetBindings("Jump", std::vector<InputBinding>(InputActionMap::MaxBindings + 1), error));
		CHECK(error.find("at most 16 bindings") != std::string::npos);
		CHECK(map->Find("Jump")->Bindings == *bindings);
		CHECK_FALSE(InputActionMap::BindingsFromJson(nlohmann::json::object(), error));
		CHECK(error.find("must be an array") != std::string::npos);
	}

	TEST_CASE("Rebinding validates every binding like the file, so a saved map always loads again")
	{
		std::string error;
		auto map = InputActionMap::FromJson({ { "Actions", { { { "Name", "Jump" }, { "Bindings", { { { "Key", "Space" } } } } } } } }, error);
		REQUIRE(map);
		const auto rejects = [&](const InputBinding& binding, const char* expected) {
			CHECK_FALSE(map->SetBindings("Jump", { binding }, error));
			CHECK_MESSAGE(error.find(expected) != std::string::npos, error);
			CHECK(std::get<KeyCode>(map->Find("Jump")->Bindings.at(0).Control) == KeyCode::Space);
		};
		rejects({ .Scale = 2.0f }, "Scale must be a number in [-1, 1]");
		rejects({ .Scale = std::numeric_limits<float>::quiet_NaN() }, "Scale must be a number in [-1, 1]");
		rejects({ .DeadZone = 0.5f }, "DeadZone applies to GamepadAxis bindings only");
		rejects({ .Control = GamepadAxis::LeftX, .DeadZone = -1.0f }, "DeadZone must be a number in [0, 0.95]");
		rejects({ .Control = GamepadAxis::LeftX, .DeadZone = std::numeric_limits<float>::infinity() }, "DeadZone must be a number");
		rejects({ .Gamepad = 0u }, "Gamepad applies to GamepadButton and GamepadAxis bindings only");
		rejects({ .Control = GamepadButton::A, .Gamepad = Input::MaxGamepads }, "integer from 0 to 15");
		rejects({ .Control = static_cast<KeyCode>(9999) }, "binding Key has no name");
		rejects({ .Control = static_cast<GamepadAxis>(40) }, "binding GamepadAxis has no name");
		// Whatever SetBindings accepts saves and loads again unchanged.
		const InputBinding stick{ .Control = GamepadAxis::RightY, .Scale = -0.5f, .DeadZone = 0.3f, .Gamepad = 3u };
		REQUIRE(map->SetBindings("Jump", { stick }, error));
		const auto reloaded = InputActionMap::FromJson(map->ToJson(), error);
		REQUIRE_MESSAGE(reloaded, error);
		CHECK(*reloaded == *map);
	}

	TEST_CASE("Injected gamepads end the game's blocks as soon as they are let go, like keys")
	{
		BasaltTest::ScopedGameInput restore;
		Input::SetKeyState(KeyCode::Q, true);
		Input::SetGamepadConnected(0, true);
		REQUIRE(Input::SetGamepadButtonState(0, GamepadButton::A, true));
		REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::LeftX, 1.0f));
		Input::SetGameFocus(false);
		Input::EndFrame();
		Input::SetGameFocus(true);
		REQUIRE_FALSE(GameInput::IsGamepadButtonDown(0, GamepadButton::A));
		REQUIRE_FALSE(GameInput::IsKeyDown(KeyCode::Q));
		// Released and pressed again within one frame: a new press for the game, for a key and a gamepad alike.
		Input::SetKeyState(KeyCode::Q, false);
		Input::SetKeyState(KeyCode::Q, true);
		REQUIRE(Input::SetGamepadButtonState(0, GamepadButton::A, false));
		REQUIRE(Input::SetGamepadButtonState(0, GamepadButton::A, true));
		CHECK(GameInput::IsKeyPressed(KeyCode::Q));
		CHECK(GameInput::IsGamepadButtonPressed(0, GamepadButton::A));
		// A stick is the game's again once it rests, without waiting for the frame to end.
		CHECK(GameInput::GetGamepadAxis(0, GamepadAxis::LeftX) == 0.0f);
		REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::LeftX, 0.0f));
		REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::LeftX, 0.9f));
		CHECK(GameInput::GetGamepadAxis(0, GamepadAxis::LeftX) == doctest::Approx(0.9f));
	}

	TEST_CASE("Physical gamepads held through a focus change stay the game's only after they are let go")
	{
		BasaltTest::ScopedGameInput restore;
		GamepadDeviceState held;
		held.Name = "Pad";
		held.Buttons[static_cast<size_t>(GamepadButton::B)] = true;
		Input::UpdateGamepadFromDevice(4, &held);
		CHECK(GameInput::IsGamepadButtonPressed(4, GamepadButton::B));
		Input::EndFrame();
		Input::SetGameFocus(false);
		Input::UpdateGamepadFromDevice(4, &held);
		CHECK(GameInput::IsGamepadButtonReleased(4, GamepadButton::B));
		Input::EndFrame();
		Input::SetGameFocus(true);
		Input::UpdateGamepadFromDevice(4, &held);
		CHECK(Input::IsGamepadButtonDown(4, GamepadButton::B));
		CHECK_FALSE(GameInput::IsGamepadButtonDown(4, GamepadButton::B));
		Input::EndFrame();
		GamepadDeviceState letGo;
		letGo.Name = "Pad";
		Input::UpdateGamepadFromDevice(4, &letGo);
		Input::EndFrame();
		Input::UpdateGamepadFromDevice(4, &held);
		CHECK(GameInput::IsGamepadButtonPressed(4, GamepadButton::B));
		Input::UpdateGamepadFromDevice(4, nullptr);
	}

	TEST_CASE("A key the host withholds never reaches the game, until it is pressed again")
	{
		BasaltTest::ScopedGameInput restore;
		Input::SetKeyState(KeyCode::Escape, true);
		Input::WithholdFromGame(KeyCode::Escape);
		CHECK(Input::IsKeyPressed(KeyCode::Escape));
		CHECK_FALSE(GameInput::IsKeyDown(KeyCode::Escape));
		CHECK_FALSE(GameInput::IsKeyPressed(KeyCode::Escape));
		Input::EndFrame();
		Input::SetKeyState(KeyCode::Escape, false);
		CHECK_FALSE(GameInput::IsKeyReleased(KeyCode::Escape));
		Input::EndFrame();
		Input::SetKeyState(KeyCode::Escape, true);
		CHECK(GameInput::IsKeyPressed(KeyCode::Escape));
		// Withholding a key that is not held does nothing.
		Input::SetKeyState(KeyCode::Escape, false);
		Input::WithholdFromGame(KeyCode::Escape);
		Input::SetKeyState(KeyCode::Escape, true);
		CHECK(GameInput::IsKeyDown(KeyCode::Escape));
	}

	TEST_CASE("A scaled gamepad axis presses a button on whichever gamepad pushes it the right way")
	{
		BasaltTest::ScopedGameInput restore;
		std::string error;
		const auto map = InputActionMap::FromJson({ { "Actions", { { { "Name", "Up" }, { "Bindings", { { { "GamepadAxis", "LeftY" }, { "Scale", -1 } } } } }, { { "Name", "Steer" }, { "Type", "Axis" }, { "Bindings", { { { "GamepadAxis", "LeftX" }, { "DeadZone", 0 } } } } } } } }, error);
		REQUIRE(map);
		Input::SetGamepadConnected(0, true);
		Input::SetGamepadConnected(1, true);
		// Gamepad 0 pushes up; gamepad 1 pushes down further, which must not hide gamepad 0's press.
		REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::LeftY, -0.9f));
		REQUIRE(Input::SetGamepadAxis(1, GamepadAxis::LeftY, 0.95f));
		CHECK(InputActionMap::IsDown(*map->Find("Up")));
		CHECK(InputActionMap::GetValue(*map->Find("Up")) > 0.85f);
		// An axis action takes the stick pushed furthest, either way.
		REQUIRE(Input::SetGamepadAxis(0, GamepadAxis::LeftX, 0.4f));
		REQUIRE(Input::SetGamepadAxis(1, GamepadAxis::LeftX, -0.6f));
		CHECK(InputActionMap::GetValue(*map->Find("Steer")) == doctest::Approx(-0.6f));
	}

	TEST_CASE("ParseJson can say why a document did not parse")
	{
		std::string error;
		CHECK(ParseJson(R"({"a": 1})", error)["a"] == 1);
		CHECK(error.empty());
		CHECK(ParseJson("{\n  \"a\": }", error).is_discarded());
		CHECK_MESSAGE(error.find("parse error at line 2") != std::string::npos, error);
		CHECK(ParseJson(std::string(MaxJsonDepth + 1, '['), error).is_discarded());
		CHECK_MESSAGE(error.find("nests deeper than") != std::string::npos, error);
		// A number out of range is reported, never thrown.
		CHECK(ParseJson("[1e999]", error).is_discarded());
		CHECK_MESSAGE(error.find("number overflow") != std::string::npos, error);
		// A success clears an earlier message.
		CHECK_FALSE(ParseJson("[]", error).is_discarded());
		CHECK(error.empty());
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
		CHECK_FALSE(BasaltTest::HasTempFiles(path.parent_path()));
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

	TEST_CASE("FileSystem reads only regular files")
	{
		// A directory opens as a stream on Linux with a meaningless size; reading one must fail, not allocate it.
		BasaltTest::TempDirectory directory("FileSystemReadDirectory");
		CHECK_FALSE(FileSystem::ReadTextFile(directory.GetPath()).has_value());
		CHECK_FALSE(FileSystem::ReadBinaryFile(directory.GetPath()).has_value());
		CHECK_FALSE(FileSystem::ReadTextFile(directory.GetPath() / "missing.txt").has_value());
		REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / "file.txt", "text"));
		CHECK(FileSystem::ReadTextFile(directory.GetPath() / "file.txt") == "text");
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
		CHECK_FALSE(BasaltTest::HasTempFiles(directory));

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

	TEST_CASE("Log history keeps messages whose listeners log or throw")
	{
		LogHistory history(8);
		int calls = 0;
		// A listener that logs re-enters Push: the nested message is stored but not passed to listeners again.
		const uint64_t echo = history.AddListener([&](const LogMessage& message) {
			calls++;
			history.Push({ LogLevel::Info, "BASALT", "echo of " + message.Text });
		});
		history.Push({ LogLevel::Info, "BASALT", "first" });
		CHECK(calls == 1);
		uint64_t next = 0;
		std::vector<std::string> texts;
		for (const LogMessage& message : history.GetMessagesSince(0, next))
			texts.push_back(message.Text);
		CHECK(texts == std::vector<std::string>{ "first", "echo of first" });
		history.RemoveListener(echo);

		// A throwing listener neither loses the message nor stops the others.
		history.AddListener([](const LogMessage&) { throw std::runtime_error("listener failure"); });
		int after = 0;
		history.AddListener([&](const LogMessage&) { after++; });
		history.Push({ LogLevel::Error, "BASALT", "kept" });
		CHECK(after == 1);
		// The message, then the failure as an error of its own.
		CHECK(history.GetErrorCount() == 2);
		const std::vector<LogMessage> all = history.GetAll();
		REQUIRE(all.size() >= 2);
		CHECK(all[all.size() - 2].Text == "kept");
		CHECK(all.back().Text == "LogHistory: a log listener failed: listener failure");
	}

	TEST_CASE("A history listener may log through the engine's loggers")
	{
		LogHistory& history = Log::GetHistory();
		std::atomic<int> calls = 0;
		const uint64_t id = history.AddListener([&](const LogMessage& message) {
			if (message.Text != "listener test: outer")
				return;
			calls++;
			BS_CORE_INFO("listener test: inner");
		});
		BS_CORE_INFO("listener test: outer");
		history.RemoveListener(id);
		CHECK(calls == 1);
		const std::vector<LogMessage> all = history.GetAll();
		const auto outer = std::ranges::find_if(all, [](const LogMessage& m) { return m.Text == "listener test: outer"; });
		const auto inner = std::ranges::find_if(all, [](const LogMessage& m) { return m.Text == "listener test: inner"; });
		CHECK(outer != all.end());
		CHECK(inner != all.end());
	}

	TEST_CASE("Concurrent atomic writes of one file all succeed and leave no temporary files")
	{
		BasaltTest::TempDirectory directory("ConcurrentWrites");
		const std::filesystem::path path = directory.GetPath() / "shared.txt";
		std::atomic<int> failures = 0;
		std::vector<std::thread> writers;
		writers.reserve(4);
		for (int t = 0; t < 4; t++)
		{
			writers.emplace_back([&, t]() {
				for (int i = 0; i < 100; i++)
				{
					std::string error;
					if (!FileSystem::WriteTextFile(path, "writer " + std::to_string(t), error))
						failures++;
				}
			});
		}
		for (std::thread& writer : writers)
			writer.join();
		CHECK(failures == 0);
		const std::string text = FileSystem::ReadTextFile(path).value_or("");
		CHECK(text.starts_with("writer "));
		CHECK(text.size() == 8);
		CHECK_FALSE(BasaltTest::HasTempFiles(directory.GetPath()));
	}

	TEST_CASE("Log history listeners can be removed while other threads log")
	{
		LogHistory history(64);
		std::atomic<bool> running = true;
		std::thread logger([&]() {
			while (running)
				history.Push({ LogLevel::Trace, "BASALT", "background" });
		});
		int total = 0;
		for (int i = 0; i < 200; i++)
		{
			auto counter = std::make_shared<std::atomic<int>>(0);
			const uint64_t id = history.AddListener([counter](const LogMessage&) { counter->fetch_add(1); });
			std::this_thread::sleep_for(std::chrono::microseconds(200));
			history.RemoveListener(id);
			// Removed means no call is running or will start.
			const int seen = counter->load();
			std::this_thread::sleep_for(std::chrono::microseconds(200));
			CHECK(counter->load() == seen);
			total += seen;
		}
		running = false;
		logger.join();
		// The listeners did run while the other thread logged.
		CHECK(total > 0);
	}
}

TEST_SUITE("Core")
{
	namespace {

		void WriteFile(const std::filesystem::path& path, const std::string& text)
		{
			REQUIRE(FileSystem::WriteTextFile(path, text));
		}

	}

	TEST_CASE("FileWatcher reports added, modified and removed files under its folder")
	{
		BasaltTest::TempDirectory root("FileWatcherRefresh");
		WriteFile(root.GetPath() / "Assets/Scripts/Player.lua", "return {}");
		WriteFile(root.GetPath() / "Assets/Textures/Wall.png", "png");
		WriteFile(root.GetPath() / "Project.bproject", "{}");

		FileWatcher watcher;
		CHECK(watcher.Refresh().empty());
		watcher.Watch(root.GetPath());
		REQUIRE(watcher.IsWatching());
		// What existed when watching started is the baseline.
		CHECK(watcher.Refresh().empty());

		WriteFile(root.GetPath() / "Assets/Scripts/Player.lua", "return { Speed = 1 }");
		WriteFile(root.GetPath() / "Assets/Models/Ship.gltf", "{}");
		std::filesystem::remove(root.GetPath() / "Assets/Textures/Wall.png");
		// Outside the watched folder.
		WriteFile(root.GetPath() / "Project.bproject", "{ }");
		const std::vector<FileChange> expected = { { "Assets/Models/Ship.gltf", FileChangeType::Added }, { "Assets/Scripts/Player.lua", FileChangeType::Modified }, { "Assets/Textures/Wall.png", FileChangeType::Removed } };
		CHECK(watcher.Refresh() == expected);
		CHECK(watcher.Refresh().empty());

		// The same size, only a newer modification time.
		const std::filesystem::path script = root.GetPath() / "Assets/Scripts/Player.lua";
		std::filesystem::last_write_time(script, std::filesystem::last_write_time(script) + std::chrono::seconds(5));
		CHECK(watcher.Refresh() == std::vector<FileChange>{ { "Assets/Scripts/Player.lua", FileChangeType::Modified } });

		// Another root starts afresh.
		BasaltTest::TempDirectory other("FileWatcherOther");
		WriteFile(other.GetPath() / "Assets/a.txt", "a");
		watcher.Watch(other.GetPath());
		CHECK(watcher.Refresh().empty());
		watcher.Watch({});
		CHECK_FALSE(watcher.IsWatching());
		CHECK(watcher.Refresh().empty());
		CHECK(std::string(FileChangeTypeName(FileChangeType::Removed)) == "Removed");
	}

	TEST_CASE("FileWatcher holds back a change until a scan finds the file unchanged")
	{
		BasaltTest::TempDirectory root("FileWatcherSettle");
		FileWatcher watcher;
		watcher.Watch(root.GetPath());
		const auto scanWith = [](uintmax_t size, int64_t time) {
			FileWatcher::Scan scan;
			scan.Files["Assets/Level.json"] = { size, time };
			return scan;
		};
		// Still being written: nothing yet, then reported once two scans agree.
		CHECK(watcher.Apply(scanWith(10, 1), watcher.BeginScan(), true).empty());
		CHECK(watcher.Apply(scanWith(20, 2), watcher.BeginScan(), true).empty());
		CHECK(watcher.Apply(scanWith(20, 2), watcher.BeginScan(), true) == std::vector<FileChange>{ { "Assets/Level.json", FileChangeType::Added } });
		CHECK(watcher.Apply(scanWith(20, 2), watcher.BeginScan(), true).empty());
		CHECK(watcher.Apply(scanWith(30, 3), watcher.BeginScan(), true).empty());
		CHECK(watcher.Apply(scanWith(30, 3), watcher.BeginScan(), true) == std::vector<FileChange>{ { "Assets/Level.json", FileChangeType::Modified } });
		// Changed back to what was reported before it settled: nothing to report.
		CHECK(watcher.Apply(scanWith(31, 4), watcher.BeginScan(), true).empty());
		CHECK(watcher.Apply(scanWith(30, 3), watcher.BeginScan(), true).empty());
		CHECK(watcher.Apply(scanWith(30, 3), watcher.BeginScan(), true).empty());
		// Removal settles too.
		CHECK(watcher.Apply({}, watcher.BeginScan(), true).empty());
		CHECK(watcher.Apply({}, watcher.BeginScan(), true) == std::vector<FileChange>{ { "Assets/Level.json", FileChangeType::Removed } });

		// A scan that could not read the folder reports nothing, and a scan older than the last one is ignored.
		FileWatcher::Scan failed = scanWith(1, 1);
		failed.Error = "cannot read";
		CHECK(watcher.Apply(failed, watcher.BeginScan(), false).empty());
		const uint64_t old = watcher.BeginScan();
		CHECK(watcher.Apply(scanWith(5, 5), watcher.BeginScan(), false).size() == 1);
		CHECK(watcher.Apply({}, old, false).empty());
	}

	TEST_CASE("FileWatcher does not report the host's own writes")
	{
		BasaltTest::TempDirectory root("FileWatcherAcknowledge");
		const std::filesystem::path file = root.GetPath() / "Assets/Scripts/Enemy.lua";
		WriteFile(file, "return {}");
		FileWatcher watcher;
		watcher.Watch(root.GetPath());

		WriteFile(file, "return { Health = 3 }");
		watcher.Acknowledge("Assets/Scripts/Enemy.lua");
		WriteFile(root.GetPath() / "Assets/Scripts/New.lua", "return {}");
		watcher.Acknowledge("Assets/Scripts/./New.lua");
		CHECK(watcher.Refresh().empty());

		// A scan that began before the write shows the old file; it must not undo the acknowledgement.
		const uint64_t sequence = watcher.BeginScan();
		const FileWatcher::Scan before = FileWatcher::ScanDirectory(root.GetPath(), "Assets");
		WriteFile(file, "return { Health = 30 }");
		watcher.Acknowledge("Assets/Scripts/Enemy.lua");
		std::filesystem::remove(root.GetPath() / "Assets/Scripts/New.lua");
		watcher.Acknowledge("Assets/Scripts/New.lua");
		// Without settling, so only the sequence check can hold the stale file back.
		CHECK(watcher.Apply(before, sequence, false).empty());
		CHECK(watcher.Refresh().empty());

		// Files outside the folder are not recorded (they would be reported as removed).
		WriteFile(root.GetPath() / "Notes.txt", "x");
		watcher.Acknowledge("Notes.txt");
		CHECK(watcher.Refresh().empty());
	}

	TEST_CASE("FileWatcher polling starts and stops cleanly")
	{
		BasaltTest::TempDirectory root("FileWatcherPolling");
		WriteFile(root.GetPath() / "Assets/a.txt", "a");
		{
			FileWatcher watcher;
			// Asked for before a folder is watched: starts with the folder.
			watcher.StartPolling(std::chrono::milliseconds(1));
			CHECK(watcher.IsPolling());
			watcher.Watch(root.GetPath());
			CHECK(watcher.IsPolling());
			// Whatever the worker scanned meanwhile (if anything), nothing changed.
			CHECK(watcher.Update().empty());
			watcher.StopPolling();
			CHECK_FALSE(watcher.IsPolling());
			CHECK(watcher.Update().empty());
			watcher.StartPolling(std::chrono::milliseconds(1));
			// Destroyed while polling: the destructor stops the worker.
		}
		CHECK(true);
	}
}
