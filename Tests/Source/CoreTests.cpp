#include <doctest/doctest.h>

#include <Basalt/Core/FileSystem.h>
#include <Basalt/Core/Input.h>
#include <Basalt/Core/KeyCodes.h>
#include <Basalt/Core/LayerStack.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Core/MouseCodes.h>
#include <Basalt/Core/UUID.h>
#include <Basalt/Events/ApplicationEvent.h>
#include <Basalt/Events/KeyEvent.h>
#include <Basalt/Events/MouseEvent.h>

#include <filesystem>
#include <unordered_set>
#include <vector>

using namespace Basalt;

TEST_SUITE("Core")
{
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
}
