#include <doctest/doctest.h>

#include <Basalt/Core/FileSystem.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Project/Project.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>
#include <Basalt/UI/GameUI.h>
#include <Basalt/UI/UIFonts.h>
#include <Basalt/UI/UIGeometry.h>
#include <Basalt/UI/UILayout.h>
#include <Basalt/UI/UIRenderer.h>
#include <Basalt/UI/UIRules.h>

#include "TestUtils.h"
#include "UITestUtils.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace Basalt;
using namespace BasaltTest;

namespace {

	std::vector<UIDrawCommand> GetImmediateCommands()
	{
		std::vector<UIDrawCommand> commands;
		GameUI::AppendCommands(commands);
		return commands;
	}

}

TEST_SUITE("UI")
{
	TEST_CASE("GameUI maps the mouse into the game's coordinates and shares its routing with Input")
	{
		BasaltTest::ScopedGameInput restore;
		struct RestoreHost
		{
			~RestoreHost() { GameUI::SetHostState({}); }
		} restoreHost;
		GameUI::SetHostState({});
		// Without a resolution, game coordinates are the viewport's.
		CHECK(GameUI::GetResolution() == glm::vec2(1920.0f, 1080.0f));
		CHECK(GameUI::ToGamePosition({ 12.0f, 34.0f }) == glm::vec2(12.0f, 34.0f));
		GameUI::SetViewportOrigin({ 10.0f, 20.0f });
		GameUI::SetViewportSize({ 400.0f, 300.0f });
		CHECK(GameUI::GetResolution() == glm::vec2(400.0f, 300.0f));
		CHECK(GameUI::ToGamePosition({ 210.0f, 170.0f }) == glm::vec2(200.0f, 150.0f));

		GameUI::SetResolution(glm::vec2(1600.0f, 900.0f));
		CHECK(GameUI::GetResolution() == glm::vec2(1600.0f, 900.0f));
		CHECK(GameUI::ToGamePosition({ 210.0f, 170.0f }) == glm::vec2(800.0f, 450.0f));
		CHECK(GameUI::ToGamePosition({ 10.0f, 20.0f }) == glm::vec2(0.0f, 0.0f));
		CHECK(GameUI::ToGameDelta({ 1.0f, 2.0f }) == glm::vec2(4.0f, 6.0f));
		// Unusable resolutions are ignored; nullopt goes back to the viewport size.
		GameUI::SetResolution(glm::vec2(0.0f, 900.0f));
		GameUI::SetResolution(glm::vec2(std::numeric_limits<float>::infinity(), 900.0f));
		CHECK(GameUI::GetResolution() == glm::vec2(1600.0f, 900.0f));
		CHECK(GameUI::GetHostState().Resolution == glm::vec2(1600.0f, 900.0f));
		GameUI::SetResolution(std::nullopt);
		CHECK(GameUI::GetResolution() == glm::vec2(400.0f, 300.0f));

		// The widgets' pointer and navigation flags are Input's game routing.
		GameUI::SetPointerEnabled(false);
		CHECK_FALSE(Input::HasGamePointer());
		Input::SetGameFocus(false);
		CHECK_FALSE(GameUI::IsNavigationEnabled());
		GameUI::HostState state = GameUI::GetHostState();
		CHECK_FALSE(state.PointerEnabled);
		CHECK_FALSE(state.NavigationEnabled);
		state.PointerEnabled = true;
		state.NavigationEnabled = true;
		state.Resolution = glm::vec2(800.0f, 600.0f);
		GameUI::SetHostState(state);
		CHECK(Input::HasGamePointer());
		CHECK(Input::HasGameFocus());
		CHECK(GameUI::GetHostState() == state);
		// Without the pointer the widgets have no pointer position either.
		Input::SetMousePosition({ 100.0f, 100.0f });
		CHECK(GameUI::GetPointerPosition().has_value());
		Input::SetGamePointer(false);
		CHECK_FALSE(GameUI::GetPointerPosition().has_value());
	}

	TEST_CASE("GameUI collects immediate-mode commands per scene update and maps the 1080-unit canvas")
	{
		GameUI::BeginFrame();
		UIDrawCommand text;
		text.Kind = UIDrawKind::Text;
		text.Text = "Score";
		text.FontSize = 1000.0f;
		text.Color = { 2.0f, -1.0f, 0.5f, 1.0f };
		CHECK(GameUI::Submit(text) == GameUI::SubmitResult::Queued);
		UIDrawCommand broken;
		broken.Position = { std::numeric_limits<float>::quiet_NaN(), 0.0f };
		CHECK(GameUI::Submit(broken) == GameUI::SubmitResult::InvalidPlacement);
		UIDrawCommand rect;
		rect.Size = { -5.0f, 50.0f };
		CHECK(GameUI::Submit(rect) == GameUI::SubmitResult::Queued);

		const std::vector<UIDrawCommand> commands = GetImmediateCommands();
		REQUIRE(commands.size() == 2);
		CHECK(commands[0].FontSize == UITextComponent::MaxFontSize);
		CHECK(commands[0].Color == glm::vec4(1.0f, 0.0f, 0.5f, 1.0f));
		CHECK(commands[1].Size == glm::vec2(0.0f, 50.0f));

		CHECK(GameUI::GetCanvasSize({ 1920.0f, 1080.0f }) == glm::vec2(1920.0f, 1080.0f));
		CHECK(GameUI::GetCanvasSize({ 800.0f, 800.0f }) == glm::vec2(1080.0f, 1080.0f));

		// A scene update starts a new UI frame.
		Scene scene;
		scene.OnRuntimeStart();
		scene.OnUpdate(Step);
		CHECK(GameUI::GetCommandCount() == 0);
		scene.OnRuntimeStop();
	}

	TEST_CASE("GameUI drops commands past MaxCommands")
	{
		GameUI::BeginFrame();
		for (size_t i = 0; i < GameUI::MaxCommands; i++)
			REQUIRE(GameUI::Submit(UIDrawCommand{}) == GameUI::SubmitResult::Queued);
		CHECK(GameUI::Submit(UIDrawCommand{}) == GameUI::SubmitResult::QueueFull);
		CHECK(GameUI::GetCommandCount() == GameUI::MaxCommands);
		GameUI::BeginFrame();
	}

	TEST_CASE("ComputeUIRect places a rect by its anchors, pivot, position and size")
	{
		const UIBox parent{ { 100.0f, 50.0f }, { 800.0f, 600.0f } };

		// Equal anchors pin a point; Size is the size and the pivot sits on the anchor.
		UIRectComponent centered;
		CHECK(ComputeUIRect(parent, centered) == UIBox{ { 400.0f, 300.0f }, { 200.0f, 100.0f } });

		// Different anchors stretch with the parent; a negative Size insets.
		UIRectComponent stretched;
		stretched.AnchorMin = { 0.0f, 0.0f };
		stretched.AnchorMax = { 1.0f, 1.0f };
		stretched.Size = { -20.0f, -40.0f };
		CHECK(ComputeUIRect(parent, stretched) == UIBox{ { 110.0f, 70.0f }, { 780.0f, 560.0f } });

		// A top-right corner element, moved inward by Position.
		UIRectComponent corner;
		corner.AnchorMin = corner.AnchorMax = corner.Pivot = { 1.0f, 0.0f };
		corner.Position = { -10.0f, 10.0f };
		corner.Size = { 50.0f, 20.0f };
		CHECK(ComputeUIRect(parent, corner) == UIBox{ { 840.0f, 60.0f }, { 50.0f, 20.0f } });

		// A stretched axis with an off-center pivot: Position moves the pivot, not the corner.
		UIRectComponent bar;
		bar.AnchorMin = { 0.0f, 1.0f };
		bar.AnchorMax = { 1.0f, 1.0f };
		bar.Pivot = { 0.5f, 1.0f };
		bar.Position = { 0.0f, -8.0f };
		bar.Size = { 0.0f, 30.0f };
		CHECK(ComputeUIRect(parent, bar) == UIBox{ { 100.0f, 612.0f }, { 800.0f, 30.0f } });

		// Shrinking past zero collapses the size around the pivot.
		UIRectComponent collapsed;
		collapsed.Size = { -500.0f, 10.0f };
		CHECK(ComputeUIRect(parent, collapsed) == UIBox{ { 500.0f, 345.0f }, { 0.0f, 10.0f } });
	}

	TEST_CASE("AlignInBox and ResolveUIBox place content and anchored commands")
	{
		const UIBox box{ { 10.0f, 10.0f }, { 100.0f, 50.0f } };
		CHECK(AlignInBox(box, { 20.0f, 10.0f }, { 0.0f, 0.0f }) == glm::vec2(10.0f, 10.0f));
		CHECK(AlignInBox(box, { 20.0f, 10.0f }, { 0.5f, 1.0f }) == glm::vec2(50.0f, 50.0f));
		CHECK(AlignInBox(box, { 20.0f, 10.0f }, { 1.0f, 0.5f }) == glm::vec2(90.0f, 30.0f));
		// A zero-size box makes the alignment the content's pivot.
		CHECK(AlignInBox({ { 50.0f, 50.0f }, { 0.0f, 0.0f } }, { 20.0f, 10.0f }, { 1.0f, 1.0f }) == glm::vec2(30.0f, 40.0f));

		UIDrawCommand command;
		command.Anchor = command.Pivot = { 1.0f, 1.0f };
		command.Position = { -20.0f, -20.0f };
		command.Size = { 100.0f, 50.0f };
		CHECK(ResolveUIBox(command, { 1920.0f, 1080.0f }) == UIBox{ { 1800.0f, 1010.0f }, { 100.0f, 50.0f } });
		// The same anchored command follows a narrower canvas.
		CHECK(ResolveUIBox(command, { 1440.0f, 1080.0f }) == UIBox{ { 1320.0f, 1010.0f }, { 100.0f, 50.0f } });
	}

	TEST_CASE("Anchor presets name the canvas corners, edges and center")
	{
		CHECK(FindUIAnchorPreset("TopLeft") == glm::vec2(0.0f, 0.0f));
		CHECK(FindUIAnchorPreset("Top") == glm::vec2(0.5f, 0.0f));
		CHECK(FindUIAnchorPreset("Right") == glm::vec2(1.0f, 0.5f));
		CHECK(FindUIAnchorPreset("Center") == glm::vec2(0.5f, 0.5f));
		CHECK(FindUIAnchorPreset("BottomRight") == glm::vec2(1.0f, 1.0f));
		CHECK_FALSE(FindUIAnchorPreset("Middle"));
		CHECK_FALSE(FindUIAnchorPreset("topleft"));
		CHECK(GetUIAnchorPresetNames().size() == 9);
		for (std::string_view name : GetUIAnchorPresetNames())
			CHECK(FindUIAnchorPreset(name));
	}

	TEST_CASE("ComputeNineSlice keeps corners, stretches edges and shrinks overlapping borders")
	{
		const UIBox box{ { 0.0f, 0.0f }, { 100.0f, 60.0f } };
		const std::array<UIQuad, 9> quads = ComputeNineSlice(box, { 32.0f, 32.0f }, { 8.0f, 8.0f, 8.0f, 8.0f }, 1.0f);
		CHECK(quads[0].Box == UIBox{ { 0.0f, 0.0f }, { 8.0f, 8.0f } });
		CHECK(quads[2].Box == UIBox{ { 92.0f, 0.0f }, { 8.0f, 8.0f } });
		CHECK(quads[4].Box == UIBox{ { 8.0f, 8.0f }, { 84.0f, 44.0f } });
		CHECK(quads[8].Box == UIBox{ { 92.0f, 52.0f }, { 8.0f, 8.0f } });
		CHECK(quads[0].UVMin == glm::vec2(0.0f, 0.0f));
		CHECK(quads[0].UVMax == glm::vec2(0.25f, 0.25f));
		CHECK(quads[4].UVMin == glm::vec2(0.25f, 0.25f));
		CHECK(quads[4].UVMax == glm::vec2(0.75f, 0.75f));
		CHECK(quads[8].UVMax == glm::vec2(1.0f, 1.0f));

		// SliceScale scales the borders on the canvas, not in the texture.
		const std::array<UIQuad, 9> scaled = ComputeNineSlice(box, { 32.0f, 32.0f }, { 8.0f, 8.0f, 8.0f, 8.0f }, 2.0f);
		CHECK(scaled[0].Box.Size == glm::vec2(16.0f, 16.0f));
		CHECK(scaled[0].UVMax == glm::vec2(0.25f, 0.25f));

		// Borders wider than the box shrink together: the center collapses, corners never invert.
		const std::array<UIQuad, 9> narrow = ComputeNineSlice({ { 0.0f, 0.0f }, { 10.0f, 60.0f } }, { 32.0f, 32.0f }, { 8.0f, 8.0f, 8.0f, 8.0f }, 1.0f);
		CHECK(narrow[0].Box.Size.x == doctest::Approx(5.0f));
		CHECK(narrow[1].Box.Size.x == doctest::Approx(0.0f));
		CHECK(narrow[2].Box.Min.x == doctest::Approx(5.0f));

		// Borders wider than the texture are clamped to it.
		const std::array<UIQuad, 9> clamped = ComputeNineSlice(box, { 32.0f, 32.0f }, { 40.0f, 0.0f, 40.0f, 0.0f }, 1.0f);
		CHECK(clamped[0].UVMax.x == doctest::Approx(0.5f));
		CHECK(clamped[2].UVMin.x == doctest::Approx(0.5f));
		CHECK(clamped[0].Box.Size.x == doctest::Approx(16.0f));
		CHECK(clamped[1].Box.Size.y == doctest::Approx(0.0f)); // no top border
		CHECK(clamped[3].Box.Size.y == doctest::Approx(60.0f));
	}

	TEST_CASE("UILayout lays out the hierarchy in draw order")
	{
		Scene scene;
		CHECK_FALSE(UILayout::HasUI(scene));
		CHECK(UILayout::Layout(scene, { 1920.0f, 1080.0f }).empty());

		Entity hud = scene.CreateEntity("HUD");
		hud.AddComponent<UIRectComponent>(UIRectComponent{ .AnchorMin = { 0.0f, 0.0f }, .AnchorMax = { 1.0f, 1.0f }, .Size = { -40.0f, -40.0f } });
		hud.AddComponent<UIImageComponent>().Color = { 0.0f, 0.0f, 0.0f, 0.25f };
		Entity score = scene.CreateChildEntity(hud, "Score");
		score.AddComponent<UIRectComponent>(UIRectComponent{ .AnchorMin = { 0.0f, 0.0f }, .AnchorMax = { 0.0f, 0.0f }, .Pivot = { 0.0f, 0.0f }, .Position = { 10.0f, 10.0f }, .Size = { 300.0f, 50.0f } });
		score.AddComponent<UITextComponent>().Text = "Score";
		// No UIRect: occupies its parent's rect.
		Entity caption = scene.CreateChildEntity(hud, "Caption");
		caption.AddComponent<UITextComponent>().HorizontalAlign = UIHorizontalAlign::Center;
		// Hidden: neither it nor its subtree is laid out.
		Entity menu = scene.CreateChildEntity(hud, "Menu");
		menu.AddComponent<UIRectComponent>().Visible = false;
		Entity menuButton = scene.CreateChildEntity(menu, "Button");
		menuButton.AddComponent<UIImageComponent>();
		// A plain entity passes the canvas on to its children.
		Entity group = scene.CreateEntity("Group");
		Entity badge = scene.CreateChildEntity(group, "Badge");
		badge.AddComponent<UIRectComponent>(UIRectComponent{ .AnchorMin = { 1.0f, 1.0f }, .AnchorMax = { 1.0f, 1.0f }, .Pivot = { 1.0f, 1.0f }, .Size = { 64.0f, 64.0f } });

		const glm::vec2 canvas(1920.0f, 1080.0f);
		CHECK(UILayout::HasUI(scene));
		const std::vector<UILayoutItem> items = UILayout::Layout(scene, canvas);
		REQUIRE(items.size() == 4);
		CHECK(items[0].Owner == hud);
		CHECK(items[0].Rect == UIBox{ { 20.0f, 20.0f }, { 1880.0f, 1040.0f } });
		CHECK(items[1].Owner == score);
		CHECK(items[1].Rect == UIBox{ { 30.0f, 30.0f }, { 300.0f, 50.0f } });
		CHECK(items[2].Owner == caption);
		CHECK(items[2].Rect == items[0].Rect);
		CHECK(items[3].Owner == badge);
		CHECK(items[3].Rect == UIBox{ { 1856.0f, 1016.0f }, { 64.0f, 64.0f } });

		CHECK(UILayout::GetRect(scene, score, canvas) == items[1].Rect);
		CHECK(UILayout::GetRect(scene, badge, canvas) == items[3].Rect);
		CHECK_FALSE(UILayout::GetRect(scene, menu, canvas));
		CHECK_FALSE(UILayout::GetRect(scene, menuButton, canvas));
		CHECK_FALSE(UILayout::GetRect(scene, group, canvas));

		std::vector<UIDrawCommand> commands;
		UILayout::BuildCommands(scene, canvas, commands);
		REQUIRE(commands.size() == 3);
		CHECK(commands[0].Kind == UIDrawKind::Image);
		CHECK(commands[0].Color == glm::vec4(0.0f, 0.0f, 0.0f, 0.25f));
		CHECK(ResolveUIBox(commands[0], canvas) == items[0].Rect);
		CHECK(commands[1].Kind == UIDrawKind::Text);
		CHECK(commands[1].Text == "Score");
		CHECK(commands[1].Wrap);
		CHECK(ResolveUIBox(commands[1], canvas) == items[1].Rect);
		CHECK(commands[2].Align == glm::vec2(0.5f, 0.0f));

		// Showing the menu brings back its subtree.
		menu.GetComponent<UIRectComponent>().Visible = true;
		CHECK(UILayout::Layout(scene, canvas).size() == 6);
	}

	TEST_CASE("UILayout repairs out-of-range components written from C++")
	{
		Scene scene;
		Entity entity = scene.CreateEntity("Broken");
		UIRectComponent& rect = entity.AddComponent<UIRectComponent>();
		rect.AnchorMin = { 0.5f, std::numeric_limits<float>::quiet_NaN() };
		rect.AnchorMax = { 0.25f, 0.5f };
		UITextComponent& text = entity.AddComponent<UITextComponent>();
		text.FontSize = 9000.0f;
		text.HorizontalAlign = static_cast<UIHorizontalAlign>(7);
		UIImageComponent& image = entity.AddComponent<UIImageComponent>();
		image.Color = { std::numeric_limits<float>::quiet_NaN(), 2.0f, 0.5f, 1.0f };

		const uint64_t since = Log::GetHistory().GetTotalCount();
		const std::vector<UILayoutItem> items = UILayout::Layout(scene, { 1920.0f, 1080.0f });
		REQUIRE(items.size() == 1);
		// AnchorMin.y takes its default (0.5) and AnchorMax.x is raised to AnchorMin.x.
		CHECK(items[0].Rect == UIBox{ { 860.0f, 490.0f }, { 200.0f, 100.0f } });
		std::vector<UIDrawCommand> commands;
		UILayout::BuildCommands(scene, { 1920.0f, 1080.0f }, commands);
		REQUIRE(commands.size() == 2);
		CHECK(commands[0].Color == glm::vec4(1.0f, 1.0f, 0.5f, 1.0f)); // NaN takes the default
		CHECK(commands[1].FontSize == UITextComponent::MaxFontSize);
		CHECK(commands[1].Align.x == 0.0f);
		// Each component is reported once (UIRect, UIImage, UIText), however often it is laid out.
		UILayout::BuildCommands(scene, { 1920.0f, 1080.0f }, commands);
		CHECK(CountLogMessages(since, "on 'Broken' has invalid values") == 3);
		// A value that changes every frame (C++ animating it) is still reported once.
		rect.AnchorMin.y = 2.0f;
		image.Color.r = 3.0f;
		text.FontSize = 9500.0f;
		UILayout::BuildCommands(scene, { 1920.0f, 1080.0f }, commands);
		CHECK(CountLogMessages(since, "on 'Broken' has invalid values") == 3);
	}

	TEST_CASE("UI component rules reject values the layout cannot use")
	{
		Scene scene;
		Entity entity = scene.CreateEntity();
		const auto expectError = [&](const char* component, const nlohmann::json& data, const char* fragment) {
			std::string error;
			CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, component, data, error));
			CHECK_MESSAGE(Contains(error, fragment), component, " ", data.dump(), " -> ", error);
		};
		expectError("UIRect", { { "AnchorMin", { -0.1, 0.0 } } }, "AnchorMin.x must be between 0 and 1");
		expectError("UIRect", { { "AnchorMin", { 0.6, 0.5 } } }, "AnchorMax.x must not be less than AnchorMin.x");
		expectError("UIRect", { { "Pivot", { 0.0, 1.5 } } }, "Pivot.y must be between 0 and 1");
		expectError("UIRect", { { "Size", { 2e6, 0.0 } } }, "Size.x must be between");
		expectError("UIRect", { { "Position", { 0.0, -2e6 } } }, "Position.y must be between");
		expectError("UIRect", { { "Size", { 1.0, 2.0, 3.0 } } }, "array of 2 numbers");
		expectError("UIImage", { { "SliceScale", 0.0 } }, "SliceScale must be between");
		expectError("UIImage", { { "Slice", { -1.0, 0.0, 0.0, 0.0 } } }, "Slice.x must be between 0 and");
		expectError("UIImage", { { "Color", { 2.0, 0.0, 0.0, 1.0 } } }, "Color.r must be between 0 and 1");
		expectError("UIText", { { "FontSize", 2.0 } }, "FontSize must be between 4 and 512");
		expectError("UIText", { { "Color", { 0.0, 0.0, 0.0, 1.5 } } }, "Color.a must be between 0 and 1");
		expectError("UIText", { { "HorizontalAlign", "Middle" } }, "HorizontalAlign");
		expectError("UIText", { { "VerticalAlign", "Center" } }, "VerticalAlign");
		CHECK_FALSE(entity.HasComponent<UIRectComponent>());
		CHECK_FALSE(entity.HasComponent<UIImageComponent>());
		CHECK_FALSE(entity.HasComponent<UITextComponent>());

		std::string error;
		REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(entity, "UIRect", { { "AnchorMin", { 0.0, 0.0 } }, { "AnchorMax", { 1.0, 0.0 } }, { "Pivot", { 0.0, 0.0 } } }, error), error);
		CHECK(entity.GetComponent<UIRectComponent>().AnchorMax == glm::vec2(1.0f, 0.0f));
		REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(entity, "UIText", { { "HorizontalAlign", "Right" }, { "VerticalAlign", "Middle" } }, error), error);
		CHECK(entity.GetComponent<UITextComponent>().VerticalAlign == UIVerticalAlign::Middle);

		// Sanitizing gives what validation accepts.
		UIRectComponent rect;
		rect.AnchorMin = { 2.0f, std::numeric_limits<float>::infinity() };
		rect.AnchorMax = { 0.0f, 0.0f };
		rect.Size = { std::numeric_limits<float>::quiet_NaN(), -3e6f };
		const UIRectComponent repaired = SanitizeUIRect(rect);
		CHECK(ValidateUIRect(repaired).empty());
		CHECK(repaired.AnchorMin == glm::vec2(1.0f, 0.5f));
		CHECK(repaired.AnchorMax == glm::vec2(1.0f, 0.5f));
		CHECK(repaired.Size == glm::vec2(200.0f, -UIRectComponent::MaxMagnitude));
		CHECK(SanitizeUIRect(UIRectComponent{}) == UIRectComponent{});
		UIImageComponent image;
		image.SliceScale = 0.0f;
		CHECK(SanitizeUIImage(image).SliceScale == doctest::Approx(1e-3f));
		CHECK(ValidateUIImage(UIImageComponent{}).empty());
		CHECK(ValidateUIText(UITextComponent{}).empty());
	}

	TEST_CASE("BreakLines breaks at newlines and between words, without trailing blanks")
	{
		ImGuiTestContext imgui;
		std::string error;
		ImFont* font = UIFonts::LoadFromMemory(ReadKarla(), "Karla", error);
		REQUIRE_MESSAGE(font, error);
		const float size = 32.0f;
		const auto width = [&](const char* text) { return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x; };
		const auto texts = [](const std::vector<UITextLine>& lines) {
			std::vector<std::string> result;
			result.reserve(lines.size());
			for (const UITextLine& line : lines)
				result.emplace_back(line.Text);
			return result;
		};

		std::vector<UITextLine> lines = UIRenderer::BreakLines(font, size, "Score\nLives   \n", 0.0f);
		CHECK(texts(lines) == std::vector<std::string>{ "Score", "Lives", "" });
		CHECK(lines[0].Width == doctest::Approx(width("Score")));
		CHECK(lines[1].Width == doctest::Approx(width("Lives")));
		// Carriage returns count as trailing blanks.
		CHECK(texts(UIRenderer::BreakLines(font, size, "a\r\nb", 0.0f)) == std::vector<std::string>{ "a", "b" });

		CHECK(texts(UIRenderer::BreakLines(font, size, "Hello world again", width("Hello world") + 2.0f)) == std::vector<std::string>{ "Hello world", "again" });
		// Regression: a wrap at the blanks before a newline used to leave an empty line; a blank line the
		// text asks for stays.
		const float wrap = (width("ab") + width("ab ")) * 0.5f;
		CHECK(texts(UIRenderer::BreakLines(font, size, "ab \ncd", wrap)) == std::vector<std::string>{ "ab", "cd" });
		CHECK(texts(UIRenderer::BreakLines(font, size, "ab \n\ncd", wrap)) == std::vector<std::string>{ "ab", "", "cd" });

		// No wrapping without a width, however long the line.
		CHECK(UIRenderer::BreakLines(font, size, "Hello world again", 0.0f).size() == 1);
		// A width too small for anything still makes progress, one whole character (not byte) at a time.
		CHECK(texts(UIRenderer::BreakLines(font, size, "ab", 0.5f)) == std::vector<std::string>{ "a", "b" });
		CHECK(texts(UIRenderer::BreakLines(font, size, "\xC3\xA9\xC3\xA9", 0.5f)) == std::vector<std::string>{ "\xC3\xA9", "\xC3\xA9" });
		CHECK(UIRenderer::BreakLines(font, size, "", 100.0f).size() == 1);
	}

	TEST_CASE("UIRenderer draws rects, images, nine-slice panels and aligned text where the canvas maps them")
	{
		ImGuiTestContext imgui;
		ImGui::NewFrame();
		ImGuiTestContext::MarkTexturesUploaded();
		ImDrawList* drawList = ImGui::GetForegroundDrawList();
		UIResources resources;
		resources.Textures["Panel.png"] = UITexture{ 0x1234, { 32.0f, 32.0f } };

		// The 1920x1080 canvas drawn at half size, at an offset: canvas (x, y) lands on (300 + x/2, 200 + y/2).
		const glm::vec2 origin(300.0f, 200.0f);
		const glm::vec2 size(960.0f, 540.0f);
		const glm::vec2 canvas(1920.0f, 1080.0f);
		struct Drawn
		{
			std::vector<ImDrawVert> Vertices;
			std::vector<ImDrawCmd> Commands;
			glm::vec2 Min{ FLT_MAX };
			glm::vec2 Max{ -FLT_MAX };
		};
		const auto drawAt = [&](const UIDrawCommand& command, const glm::vec2& at, const glm::vec2& extent) {
			const int firstVertex = drawList->VtxBuffer.Size;
			const int firstCommand = drawList->CmdBuffer.Size;
			UIRenderer::Draw(drawList, at, extent, canvas, std::span(&command, 1), resources);
			// Drawing text can grow the atlas into new textures.
			ImGuiTestContext::MarkTexturesUploaded();
			Drawn drawn;
			for (int i = firstVertex; i < drawList->VtxBuffer.Size; i++)
			{
				const ImDrawVert& vertex = drawList->VtxBuffer[i];
				drawn.Vertices.push_back(vertex);
				drawn.Min = glm::min(drawn.Min, glm::vec2(vertex.pos.x, vertex.pos.y));
				drawn.Max = glm::max(drawn.Max, glm::vec2(vertex.pos.x, vertex.pos.y));
			}
			for (int i = std::max(firstCommand - 1, 0); i < drawList->CmdBuffer.Size; i++)
				drawn.Commands.push_back(drawList->CmdBuffer[i]);
			return drawn;
		};
		const auto draw = [&](const UIDrawCommand& command) { return drawAt(command, origin, size); };

		UIDrawCommand rect;
		rect.Kind = UIDrawKind::Rect;
		rect.Position = { 100.0f, 50.0f };
		rect.Size = { 200.0f, 100.0f };
		Drawn drawn = draw(rect);
		CHECK(drawn.Min == glm::vec2(350.0f, 225.0f));
		CHECK(drawn.Max == glm::vec2(450.0f, 275.0f));
		// Clipped to the viewport.
		CHECK(std::ranges::any_of(drawn.Commands, [&](const ImDrawCmd& command) { return command.ClipRect.x == origin.x && command.ClipRect.y == origin.y && command.ClipRect.z == origin.x + size.x && command.ClipRect.w == origin.y + size.y; }));

		// An anchored command is placed on the canvas width it is drawn with.
		UIDrawCommand anchored = rect;
		anchored.Anchor = anchored.Pivot = { 1.0f, 1.0f };
		anchored.Position = { 0.0f, 0.0f };
		drawn = draw(anchored);
		CHECK(drawn.Max == origin + size);

		UIDrawCommand panel;
		panel.Kind = UIDrawKind::Image;
		panel.Texture = "Panel.png";
		panel.Size = { 200.0f, 100.0f };
		panel.Slice = { 8.0f, 8.0f, 8.0f, 8.0f };
		drawn = draw(panel);
		int panelIndices = 0;
		for (const ImDrawCmd& command : drawn.Commands)
		{
			if (command.GetTexID() == static_cast<ImTextureID>(0x1234))
				panelIndices += static_cast<int>(command.ElemCount);
		}
		CHECK(panelIndices == 9 * 6); // nine patches, two triangles each
		CHECK(drawn.Min == origin);
		CHECK(drawn.Max == origin + glm::vec2(100.0f, 50.0f));

		// An image whose texture did not resolve is filled with its color: one quad, no texture of its own.
		UIDrawCommand missing = panel;
		missing.Texture = "Missing.png";
		drawn = draw(missing);
		CHECK(drawn.Vertices.size() == 4);
		CHECK(std::ranges::none_of(drawn.Commands, [](const ImDrawCmd& command) { return command.GetTexID() == static_cast<ImTextureID>(0x1234); }));

		// Text aligned to the bottom-right corner of its box (1000..1400, 500..600 on the canvas).
		UIDrawCommand text;
		text.Kind = UIDrawKind::Text;
		text.Text = "Right";
		text.Position = { 1000.0f, 500.0f };
		text.Size = { 400.0f, 100.0f };
		text.Align = { 1.0f, 1.0f };
		text.Shadow = false;
		drawn = draw(text);
		const glm::vec2 corner = origin + glm::vec2(700.0f, 300.0f);
		// (Glyph quads carry up to a pixel of padding.)
		CHECK(drawn.Max.x <= corner.x + 1.5f);
		CHECK(drawn.Max.x > corner.x - 10.0f);
		CHECK(drawn.Max.y <= corner.y + 1.5f);
		CHECK(drawn.Max.y > corner.y - 16.0f); // glyphs end at the descent, above the line's bottom
		CHECK(drawn.Min.x > origin.x + 600.0f);

		// Two centered lines: the block is centered in the box (and so is each line).
		text.Text = "One\nTwo";
		text.Align = { 0.5f, 0.5f };
		text.FontSize = 40.0f;
		drawn = draw(text);
		const glm::vec2 center = origin + glm::vec2(600.0f, 275.0f);
		CHECK((drawn.Min.x + drawn.Max.x) * 0.5f == doctest::Approx(center.x).epsilon(0.02));
		CHECK(drawn.Min.y >= center.y - 21.5f); // 2 lines of 20 pixels
		CHECK(drawn.Max.y <= center.y + 21.5f);

		// Text is capped at MaxTextPixelSize physical pixels: 512 display units where ImGui rasterizes at 2x
		// (a 512-unit font on a canvas drawn at 2x would be 1024; an "I" is about 0.6 of the size tall).
		ImGui::SetFontRasterizerDensity(2.0f);
		std::string error;
		ImFont* karla = UIFonts::LoadFromMemory(ReadKarla(), "Karla", error);
		REQUIRE_MESSAGE(karla, error);
		resources.Fonts["Karla.ttf"] = karla;
		UIDrawCommand huge;
		huge.Kind = UIDrawKind::Text;
		huge.Font = "Karla.ttf";
		huge.Text = "I";
		huge.FontSize = UITextComponent::MaxFontSize;
		huge.Shadow = false;
		const Drawn tall = drawAt(huge, { 0.0f, 0.0f }, { 3840.0f, 2160.0f });
		const float top = tall.Min.y;
		const float bottom = tall.Max.y;
		CHECK(bottom - top > 200.0f);
		CHECK(bottom - top <= UIRenderer::MaxTextPixelSize / 2.0f * 0.75f);
		// The draw rasterized at the display's density and left the (not pushed) font's own as it was.
		CHECK(karla->CurrentRasterizerDensity == 1.0f);
		ImGui::SetFontRasterizerDensity(1.0f);
		ImGuiTestContext::MarkTexturesUploaded();

		// Drawing reads no font files: a font missing from the resources draws with the built-in font, and
		// is neither loaded nor reported.
		UIDrawCommand unresolved = text;
		unresolved.Font = "Assets/Fonts/Unresolved.ttf";
		const int fontCount = ImGui::GetIO().Fonts->Fonts.Size;
		const float builtInWidth = draw(text).Max.x;
		CHECK(draw(unresolved).Max.x == builtInWidth);
		CHECK(ImGui::GetIO().Fonts->Fonts.Size == fontCount);
		CHECK(UIFonts::GetError(unresolved.Font).empty());

		// A shadow doubles the glyphs.
		text.Text = "Shadow";
		const size_t plain = draw(text).Vertices.size();
		text.Shadow = true;
		CHECK(draw(text).Vertices.size() == 2 * plain);
		ImGui::EndFrame();
	}

	TEST_CASE("UIFrame puts UI components under this frame's immediate commands and resolves each resource once")
	{
		ImGuiTestContext imgui;
		Scene scene;
		Entity panel = scene.CreateEntity("Panel");
		panel.AddComponent<UIRectComponent>();
		panel.AddComponent<UIImageComponent>().Texture = "Assets/Textures/Panel.png";
		Entity label = scene.CreateEntity("Label");
		label.AddComponent<UIRectComponent>();
		label.AddComponent<UITextComponent>().Font = "Assets/Fonts/Missing.ttf";
		Entity broken = scene.CreateEntity("Broken");
		broken.AddComponent<UIImageComponent>().Texture = "Assets/Textures/Broken.png";

		GameUI::BeginFrame();
		UIDrawCommand overlay;
		overlay.Kind = UIDrawKind::Image;
		overlay.Texture = "Assets/Textures/Panel.png";
		REQUIRE(GameUI::Submit(overlay) == GameUI::SubmitResult::Queued);

		UIFrame frame;
		frame.Build(&scene, { 800.0f, 600.0f }, true);
		CHECK(frame.GetCanvasSize() == glm::vec2(1440.0f, 1080.0f));
		REQUIRE(frame.GetCommands().size() == 4);
		CHECK(frame.GetCommands()[0].Kind == UIDrawKind::Image);
		CHECK(frame.GetCommands()[1].Kind == UIDrawKind::Text);
		CHECK(frame.GetCommands()[3].Texture == overlay.Texture);

		std::vector<std::string> lookups;
		const uint64_t since = Log::GetHistory().GetTotalCount();
		frame.Prepare([&](const std::string& path) -> std::optional<UITexture> {
			lookups.push_back(path);
			if (path == "Assets/Textures/Broken.png")
				return UITexture{ 0, { 16.0f, 16.0f } }; // an invalid handle counts as missing
			return UITexture{ 7, { 16.0f, 16.0f } };
		});
		CHECK(lookups == std::vector<std::string>{ "Assets/Textures/Panel.png", "Assets/Textures/Broken.png" });
		CHECK(frame.GetResources().Textures.at("Assets/Textures/Panel.png").Handle == 7);
		CHECK_FALSE(frame.GetResources().Textures.contains("Assets/Textures/Broken.png"));
		// Fonts load (and fail, falling back to the built-in one) while preparing, outside the ImGui frame.
		CHECK(CountLogMessages(since, "Missing.ttf") == 1);
		CHECK(frame.GetResources().Fonts.at("Assets/Fonts/Missing.ttf") == UIFonts::GetDefault());

		// Drawing places anchored commands on Build's canvas (1440 wide for 800x600), whatever the draw size:
		// a right-anchored rect ends at 1440 * 0.5 = 720 on a 960-wide draw.
		ImGui::NewFrame();
		ImGuiTestContext::MarkTexturesUploaded();
		GameUI::BeginFrame();
		UIDrawCommand corner;
		corner.Kind = UIDrawKind::Rect;
		corner.Anchor = corner.Pivot = { 1.0f, 0.0f };
		corner.Size = { 100.0f, 100.0f };
		REQUIRE(GameUI::Submit(corner) == GameUI::SubmitResult::Queued);
		UIFrame anchored;
		anchored.Build(nullptr, { 800.0f, 600.0f }, true);
		ImDrawList* drawList = ImGui::GetForegroundDrawList();
		const int firstVertex = drawList->VtxBuffer.Size;
		anchored.Draw(drawList, { 0.0f, 0.0f }, { 960.0f, 540.0f });
		float right = 0.0f;
		for (int i = firstVertex; i < drawList->VtxBuffer.Size; i++)
			right = std::max(right, drawList->VtxBuffer[i].pos.x);
		CHECK(right == doctest::Approx(720.0f));
		ImGui::EndFrame();

		// Building again forgets the last frame's resources.
		frame.Build(&scene, { 1920.0f, 1080.0f }, false);
		CHECK(frame.GetCommands().size() == 3);
		CHECK(frame.GetResources().Textures.empty());
		CHECK(frame.GetResources().Fonts.empty());
		frame.Build(nullptr, { 1920.0f, 1080.0f }, true);
		CHECK(frame.GetCommands().size() == 1);
		frame.Clear();
		CHECK(frame.GetCommands().empty());
		GameUI::BeginFrame();
	}

	TEST_CASE("UI.* Lua functions submit anchored commands and reject bad options")
	{
		Scene scene;
		Entity hud = scene.CreateEntity("HUD");
		hud.AddComponent<UIRectComponent>(UIRectComponent{ .AnchorMin = { 1.0f, 0.0f }, .AnchorMax = { 1.0f, 0.0f }, .Pivot = { 1.0f, 0.0f }, .Position = { -20.0f, 20.0f }, .Size = { 300.0f, 80.0f } });
		scene.CreateEntity("Plain");
		scene.OnRuntimeStart();
		REQUIRE(scene.GetScriptEngine());
		GameUI::SetViewportSize({ 1920.0f, 1080.0f });
		GameUI::BeginFrame();

		CHECK(BasaltTest::Eval(scene, "UI.Text('Legacy', 100, 50, 40, Vec4(1, 0, 0, 1), 'Center')") == "");
		CHECK(BasaltTest::Eval(scene, "UI.Text('Opts', -20, -20, { Size = 24, Anchor = 'BottomRight', Font = 'Assets/Fonts/X.ttf', Shadow = false, Width = 300 })") == "");
		CHECK(BasaltTest::Eval(scene, "UI.Text('Pivot', 0, 0, { Anchor = Vec2(0.5, 0.25), Pivot = Vec2(0, 1), Align = 'Right' })") == "");
		CHECK(BasaltTest::Eval(scene, "UI.Rect(10, 10, 100, 20)") == "");
		CHECK(BasaltTest::Eval(scene, "UI.Rect(0, 0, 100, 20, { Color = Vec4(1, 1, 1, 1), Anchor = 'Top' })") == "");
		CHECK(BasaltTest::Eval(scene, "UI.Image('Assets/Textures/Panel.png', 0, 0, 64, 64, Vec4(1, 1, 1, 0.5))") == "");
		CHECK(BasaltTest::Eval(scene, "UI.Image('Assets/Textures/Panel.png', 0, 0, 64, 64, { Slice = Vec4(4, 5, 6, 7), SliceScale = 2, Tint = Vec4(0, 1, 0, 1), Anchor = 'Center' })") == "");
		// A negative size draws nothing (a health bar below zero health), rather than raising an error.
		CHECK(BasaltTest::Eval(scene, "UI.Rect(0, 0, 200 * -5 / 100, 20)") == "");

		const std::vector<UIDrawCommand> commands = GetImmediateCommands();
		REQUIRE(commands.size() == 8);
		CHECK(commands[0].Kind == UIDrawKind::Text);
		CHECK(commands[0].FontSize == 40.0f);
		CHECK(commands[0].Anchor == glm::vec2(0.0f));
		CHECK(commands[0].Align == glm::vec2(0.5f, 0.0f));
		CHECK(commands[1].Anchor == glm::vec2(1.0f, 1.0f));
		CHECK(commands[1].Align == glm::vec2(1.0f, 1.0f));
		CHECK(commands[1].Font == "Assets/Fonts/X.ttf");
		CHECK_FALSE(commands[1].Shadow);
		CHECK(commands[1].Wrap);
		CHECK(commands[1].Size == glm::vec2(300.0f, 0.0f));
		// Width makes a box whose right edge sits on the anchored point.
		CHECK(ResolveUIBox(commands[1], { 1920.0f, 1080.0f }) == UIBox{ { 1600.0f, 1060.0f }, { 300.0f, 0.0f } });
		CHECK(commands[2].Anchor == glm::vec2(0.5f, 0.25f));
		CHECK(commands[2].Align == glm::vec2(1.0f, 1.0f));
		CHECK(commands[3].Kind == UIDrawKind::Rect);
		CHECK(commands[3].Color == glm::vec4(0.0f, 0.0f, 0.0f, 0.5f));
		CHECK(commands[4].Anchor == glm::vec2(0.5f, 0.0f));
		CHECK(commands[4].Pivot == glm::vec2(0.5f, 0.0f));
		CHECK(commands[5].Kind == UIDrawKind::Image);
		CHECK(commands[5].Color.a == 0.5f);
		CHECK(commands[6].Slice == glm::vec4(4.0f, 5.0f, 6.0f, 7.0f));
		CHECK(commands[6].SliceScale == 2.0f);
		CHECK(commands[6].Color == glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
		CHECK(commands[6].Pivot == glm::vec2(0.5f, 0.5f));
		CHECK(commands[7].Size == glm::vec2(0.0f, 20.0f));

		CHECK(BasaltTest::Eval(scene, "local r = UI.GetRect(Scene.FindEntityByName('HUD')) return r.x .. ',' .. r.y .. ',' .. r.z .. ',' .. r.w") == "1600.0,20.0,300.0,80.0");
		CHECK(BasaltTest::Eval(scene, "return UI.GetRect(Scene.FindEntityByName('Plain')) == nil") == "true");

		const std::pair<const char*, const char*> errors[] = {
			{ "UI.Text('x', 0, 0, { Colour = Vec4(1) })", "unknown option 'Colour'" },
			{ "UI.Text('x', 0, 0, { Anchor = 'Middle' })", "unknown Anchor 'Middle'" },
			{ "UI.Text('x', 0, 0, { Anchor = 3 })", "preset name or a Vec2" },
			{ "UI.Text('x', 0, 0, { Size = 'big' })", "option 'Size' must be a number" },
			{ "UI.Text('x', 0, 0, { Shadow = 1 })", "option 'Shadow' must be true or false" },
			{ "UI.Text('x', 0, 0, { Width = 0 })", "option 'Width' must be positive" },
			{ "UI.Text('x', 0, 0, { Align = 'Middle' })", "align must be 'Left', 'Center' or 'Right'" },
			{ "UI.Text('x', 0, 0, 20, Vec4(1), 'Middle')", "align must be" },
			{ "UI.Text('x', 0, 0, { Size = 20 }, Vec4(1))", "either an options table" },
			{ "UI.Text('x', 0, 0, 'big')", "a size or an options table" },
			{ "UI.Text('x', 0/0, 0)", "UI.Text: x must be between" },
			{ "UI.Rect(0, 0, 1, 1, 'red')", "Vec4 color or an options table" },
			{ "UI.Rect(0, 0, 1, 1, { Tint = Vec4(1) })", "unknown option 'Tint'" },
			{ "UI.Image('', 0, 0, 1, 1)", "texture path is empty" },
			{ "UI.Image('a.png', 0, 0, 1, 1, { Slice = 4 })", "option 'Slice' must be a Vec4" },
			{ "UI.Image('a.png', 0, 0, 1, 1, { [1] = 4 })", "option keys must be names" },
			{ "UI.GetRect(nil)", "stack index 1" },
			// Values outside what the UI components accept are errors, not silently clamped.
			{ "UI.Text('x', 0, 0, 1000)", "UI.Text: size must be between 4 and 512 (got 1000)" },
			{ "UI.Text('x', 0, 0, { Size = 2 })", "Size must be between 4 and 512" },
			{ "UI.Text('x', 0, 0, 20, Vec4(0/0, 1, 1, 1))", "color.x must be between 0 and 1" },
			{ "UI.Text('x', 0, 0, { Color = Vec4(2, 1, 1, 1) })", "Color.x must be between 0 and 1" },
			{ "UI.Text('x', 0, 0, { Width = 1e7 })", "option 'Width' must be positive and at most" },
			{ "UI.Text('x', 3e38, 0)", "x must be between -1000000 and 1000000" },
			{ "UI.Rect(0, 0, -2e6, 1)", "width must be between -1000000 and 1000000" },
			{ "UI.Rect(0, 0, 1, 3e38)", "height must be between -1000000 and 1000000" },
			{ "UI.Rect(0, 0, 1, 1, { Anchor = Vec2(1e38, 0) })", "Anchor.x must be between 0 and 1" },
			{ "UI.Rect(0, 0, 1, 1, { Pivot = Vec2(0, 2) })", "Pivot.y must be between 0 and 1" },
			{ "UI.Image('a.png', 0, 0, 1, 1, { Slice = Vec4(-1, 0, 0, 0) })", "Slice.x must be between 0 and" },
			{ "UI.Image('a.png', 0, 0, 1, 1, { SliceScale = 0 })", "SliceScale must be between" },
			{ "UI.Image('a.png', 0, 0, 1, 1, Vec4(1, 1, 1, 1.5))", "UI.Image: tint.w must be between 0 and 1" },
			{ "UI.Image('a.png', 0, 0, 1, 1, 'red')", "a Vec4 tint or an options table" },
			{ "UI.Text('x', 0, 0, { Font = 42 })", "option 'Font' must be a string" },
		};
		BasaltTest::CheckLuaErrors(*scene.GetScriptEngine(), errors);
		// The limits themselves are accepted.
		const uint64_t accepted = GameUI::GetCommandCount();
		CHECK(BasaltTest::Eval(scene, "UI.Rect(1e6, -1e6, 1e6, 0) UI.Text('x', 0, 0, 4) UI.Text('x', 0, 0, 512)") == "");
		CHECK(GameUI::GetCommandCount() == accepted + 3);
		GameUI::BeginFrame();
		for (int i = 0; i < 8; i++)
			REQUIRE(GameUI::Submit(UIDrawCommand{}) == GameUI::SubmitResult::Queued);
		// None of the failed calls queued anything.
		CHECK(GameUI::GetCommandCount() == 8);

		// A full queue is a Lua error too (a runaway loop), rather than UI silently missing.
		GameUI::BeginFrame();
		std::string result;
		CHECK_FALSE(scene.GetScriptEngine()->ExecuteString("for i = 1, 70000 do UI.Rect(0, 0, 1, 1) end", result));
		CHECK(Contains(result, "UI.Rect: more than 65536 UI commands this frame"));
		CHECK(GameUI::GetCommandCount() == GameUI::MaxCommands);
		scene.OnRuntimeStop();
		// Stopping (and starting) play clears the immediate-mode UI.
		CHECK(GameUI::GetCommandCount() == 0);
	}

	TEST_CASE("Starting play clears the previous session's immediate-mode UI")
	{
		GameUI::BeginFrame();
		REQUIRE(GameUI::Submit(UIDrawCommand{}) == GameUI::SubmitResult::Queued);
		Scene scene;
		scene.OnRuntimeStart();
		CHECK(GameUI::GetCommandCount() == 0);
		scene.OnRuntimeStop();
	}

	TEST_CASE("GameUI ignores viewport sizes that are not positive and finite")
	{
		GameUI::SetViewportSize({ 800.0f, 600.0f });
		GameUI::SetViewportSize({ 0.0f, 600.0f });
		GameUI::SetViewportSize({ std::numeric_limits<float>::infinity(), 600.0f });
		GameUI::SetViewportSize({ 800.0f, std::numeric_limits<float>::quiet_NaN() });
		CHECK(GameUI::GetViewportSize() == glm::vec2(800.0f, 600.0f));
		GameUI::SetViewportSize({ 1920.0f, 1080.0f });
	}

	TEST_CASE("SanitizeUIDrawCommand keeps every box finite")
	{
		UIDrawCommand command;
		command.Position = { 3e38f, -3e38f };
		command.Size = { 3e38f, -5.0f };
		command.Anchor = { 1e38f, -2.0f };
		command.Pivot = { 2.0f, 0.5f };
		command.Align = { -1.0f, 3.0f };
		command.Color = { std::numeric_limits<float>::quiet_NaN(), 2.0f, -1.0f, 0.5f };
		command.Slice = { -1.0f, 1e9f, 4.0f, 0.0f };
		command.SliceScale = 0.0f;
		command.FontSize = std::numeric_limits<float>::infinity();
		REQUIRE(SanitizeUIDrawCommand(command));
		constexpr float limit = UIRectComponent::MaxMagnitude;
		CHECK(command.Position == glm::vec2(limit, -limit));
		CHECK(command.Size == glm::vec2(limit, 0.0f));
		CHECK(command.Anchor == glm::vec2(1.0f, 0.0f));
		CHECK(command.Pivot == glm::vec2(1.0f, 0.5f));
		CHECK(command.Align == glm::vec2(0.0f, 1.0f));
		CHECK(command.Color == glm::vec4(1.0f, 1.0f, 0.0f, 0.5f));
		CHECK(command.Slice == glm::vec4(0.0f, UIImageComponent::MaxSlice, 4.0f, 0.0f));
		CHECK(command.SliceScale == UIImageComponent::MinSliceScale);
		CHECK(command.FontSize == UITextComponent::DefaultFontSize);
		const UIBox box = ResolveUIBox(command, { 1e6f, 1080.0f });
		CHECK(std::isfinite(box.Min.x));
		CHECK(std::isfinite(box.Min.y));

		UIDrawCommand broken;
		broken.Pivot = { std::numeric_limits<float>::quiet_NaN(), 0.0f };
		CHECK_FALSE(SanitizeUIDrawCommand(broken));
	}

	TEST_CASE("Text alignment maps to fractions of the free space")
	{
		CHECK(UIAlignFraction(UIHorizontalAlign::Left) == 0.0f);
		CHECK(UIAlignFraction(UIHorizontalAlign::Center) == 0.5f);
		CHECK(UIAlignFraction(UIHorizontalAlign::Right) == 1.0f);
		CHECK(UIAlignFraction(UIVerticalAlign::Top) == 0.0f);
		CHECK(UIAlignFraction(UIVerticalAlign::Middle) == 0.5f);
		CHECK(UIAlignFraction(UIVerticalAlign::Bottom) == 1.0f);

		Scene scene;
		Entity entity = scene.CreateEntity("Label");
		entity.AddComponent<UITextComponent>(UITextComponent{ .HorizontalAlign = UIHorizontalAlign::Right, .VerticalAlign = UIVerticalAlign::Middle });
		std::vector<UIDrawCommand> commands;
		UILayout::BuildCommands(scene, { 1920.0f, 1080.0f }, commands);
		REQUIRE(commands.size() == 1);
		CHECK(commands[0].Align == glm::vec2(1.0f, 0.5f));
	}

	TEST_CASE("Validating a UI component accepts exactly what sanitizing leaves unchanged")
	{
		std::mt19937 random(42);
		const float values[] = { 0.0f, 0.25f, 1.0f, -0.5f, 1.5f, 3e6f, -3e6f, 600.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() };
		const auto pick = [&] { return values[random() % std::size(values)]; };
		for (int i = 0; i < 500; i++)
		{
			INFO("iteration ", i);
			CheckRulesAgree(UIRectComponent{ .AnchorMin = { pick(), pick() }, .AnchorMax = { pick(), pick() }, .Pivot = { pick(), pick() }, .Position = { pick(), pick() }, .Size = { pick(), pick() } }, &SanitizeUIRect, &ValidateUIRect);
			CheckRulesAgree(UIImageComponent{ .Color = { pick(), pick(), pick(), pick() }, .Slice = { pick(), pick(), pick(), pick() }, .SliceScale = pick() }, &SanitizeUIImage, &ValidateUIImage);
			CheckRulesAgree(UITextComponent{ .FontSize = pick() * 100.0f, .Color = { pick(), pick(), pick(), pick() } }, &SanitizeUIText, &ValidateUIText);
		}
	}

	TEST_CASE("UI.GetRect and the layout agree on every rect of a random hierarchy")
	{
		std::mt19937 random(7);
		const float anchors[] = { 0.0f, 0.25f, 0.5f, 1.0f };
		for (int round = 0; round < 20; round++)
		{
			Scene scene;
			std::vector<Entity> entities;
			for (int i = 0; i < 30; i++)
			{
				Entity parent = entities.empty() || random() % 4 == 0 ? Entity{} : entities[random() % entities.size()];
				Entity entity = parent ? scene.CreateChildEntity(parent, "E") : scene.CreateEntity("E");
				switch (random() % 4)
				{
					case 0:
						break; // no UI component: passes its parent's rect on
					case 1:
						entity.AddComponent<UITextComponent>();
						break;
					default:
					{
						UIRectComponent rect;
						rect.Visible = random() % 5 != 0;
						rect.AnchorMin = { anchors[random() % 2], anchors[random() % 2] };
						rect.AnchorMax = { anchors[2 + random() % 2], anchors[2 + random() % 2] };
						rect.Pivot = { anchors[random() % 4], anchors[random() % 4] };
						rect.Position = { static_cast<float>(random() % 200) - 100.0f, static_cast<float>(random() % 200) - 100.0f };
						rect.Size = { static_cast<float>(random() % 400) - 100.0f, static_cast<float>(random() % 400) - 100.0f };
						entity.AddComponent<UIRectComponent>(rect);
						break;
					}
				}
				entities.push_back(entity);
			}
			const glm::vec2 canvas(1920.0f, 1080.0f);
			const std::vector<UILayoutItem> items = UILayout::Layout(scene, canvas);
			for (const UILayoutItem& item : items)
			{
				CHECK(UILayout::GetRect(scene, item.Owner, canvas) == item.Rect);
				// The parent rect the inspector's presets use is the one the layout used.
				if (const auto* rect = item.Owner.TryGetComponent<UIRectComponent>())
					CHECK(ComputeUIRect(UILayout::GetParentRect(scene, item.Owner, canvas), SanitizeUIRect(*rect)) == item.Rect);
			}
			for (Entity entity : entities)
			{
				const bool laidOut = std::ranges::any_of(items, [&](const UILayoutItem& item) { return item.Owner == entity; });
				CHECK(laidOut == UILayout::GetRect(scene, entity, canvas).has_value());
			}
		}
	}

	// (Here rather than in UIFontTests.cpp: it needs imgui_internal.h, which clashes with the stb copy there.)
	TEST_CASE("UIFonts empties its cache when its ImGui context shuts down, with one hook per context")
	{
		BasaltTest::TempProject project("UIFontsHook");
		ImGuiTestContext imgui;
		CHECK(UIFonts::Get("Assets/Fonts/Missing.ttf") == UIFonts::GetDefault());
		CHECK_FALSE(UIFonts::GetError("Assets/Fonts/Missing.ttf").empty());
		ImGuiContext* context = ImGui::GetCurrentContext();
		const int hooks = context->Hooks.Size;
		UIFonts::Clear();
		UIFonts::Get("Assets/Fonts/Missing.ttf");
		CHECK(context->Hooks.Size == hooks); // however often the cache is cleared and refilled
		// ... or another context takes over for a while.
		ImGuiContext* other = ImGui::CreateContext();
		ImGui::SetCurrentContext(other);
		ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
		UIFonts::Get("Assets/Fonts/Missing.ttf");
		ImGui::SetCurrentContext(context);
		UIFonts::Get("Assets/Fonts/Missing.ttf");
		CHECK(context->Hooks.Size == hooks);
		ImGui::DestroyContext(other);
		ImGui::SetCurrentContext(context);
		// The hook itself, without relying on a new context reusing the old one's address.
		ImGui::CallContextHooks(ImGui::GetCurrentContext(), ImGuiContextHookType_Shutdown);
		CHECK(UIFonts::GetError("Assets/Fonts/Missing.ttf").empty());
	}

	TEST_CASE("GetParentRect ignores visibility and falls back to the canvas")
	{
		Scene scene;
		Entity menu = scene.CreateEntity("Menu");
		menu.AddComponent<UIRectComponent>(UIRectComponent{ .Visible = false, .AnchorMin = { 0.0f, 0.0f }, .AnchorMax = { 0.5f, 1.0f }, .Size = { 0.0f, 0.0f } });
		Entity plain = scene.CreateChildEntity(menu, "Plain");
		Entity button = scene.CreateChildEntity(plain, "Button");
		button.AddComponent<UIRectComponent>();
		const glm::vec2 canvas(1920.0f, 1080.0f);
		CHECK_FALSE(UILayout::GetRect(scene, button, canvas));
		// The hidden menu's rect (through the plain entity, which passes it on).
		CHECK(UILayout::GetParentRect(scene, button, canvas) == UIBox{ { 0.0f, 0.0f }, { 960.0f, 1080.0f } });
		CHECK(UILayout::GetParentRect(scene, menu, canvas) == UIBox{ { 0.0f, 0.0f }, canvas });
		Scene other;
		CHECK(UILayout::GetParentRect(other, button, canvas) == UIBox{ { 0.0f, 0.0f }, canvas });
		CHECK(UILayout::GetParentRect(scene, Entity{}, canvas) == UIBox{ { 0.0f, 0.0f }, canvas });
	}

	TEST_CASE("Anchor presets change how a rect follows its parent, never where it is")
	{
		const UIBox parent{ { 100.0f, 50.0f }, { 800.0f, 600.0f } };
		const UIRectComponent rects[] = {
			{},
			{ .AnchorMin = { 0.0f, 0.0f }, .AnchorMax = { 1.0f, 1.0f }, .Size = { -48.0f, -48.0f } },
			{ .AnchorMin = { 0.0f, 1.0f }, .AnchorMax = { 1.0f, 1.0f }, .Pivot = { 0.5f, 1.0f }, .Position = { 10.0f, -8.0f }, .Size = { 0.0f, 64.0f } },
			{ .AnchorMin = { 1.0f, 0.0f }, .AnchorMax = { 1.0f, 0.0f }, .Pivot = { 1.0f, 0.0f }, .Position = { -20.0f, 20.0f }, .Size = { 360.0f, 120.0f } },
		};
		std::vector<std::string_view> presets(GetUIAnchorPresetNames().begin(), GetUIAnchorPresetNames().end());
		presets.push_back(UIStretchPreset);
		for (const UIRectComponent& rect : rects)
		{
			const UIBox current = ComputeUIRect(parent, rect);
			for (std::string_view preset : presets)
			{
				INFO("preset ", std::string(preset));
				const std::optional<UIRectComponent> changed = ApplyUIAnchorPreset(rect, preset, parent);
				REQUIRE(changed);
				const UIBox after = ComputeUIRect(parent, *changed);
				CHECK(after.Min.x == doctest::Approx(current.Min.x));
				CHECK(after.Min.y == doctest::Approx(current.Min.y));
				CHECK(after.Size.x == doctest::Approx(current.Size.x));
				CHECK(after.Size.y == doctest::Approx(current.Size.y));
				CHECK(ValidateUIRect(*changed).empty());
			}
		}
		// A rect whose Position or Size would leave their range keeps a valid one (and moves as little as
		// it must).
		const UIRectComponent wide{ .AnchorMin = { 0.0f, 0.0f }, .AnchorMax = { 1.0f, 1.0f }, .Size = { UIRectComponent::MaxMagnitude, 0.0f } };
		const std::optional<UIRectComponent> pinned = ApplyUIAnchorPreset(wide, "Center", parent);
		REQUIRE(pinned);
		CHECK(ValidateUIRect(*pinned).empty());
		CHECK(pinned->Size.x == UIRectComponent::MaxMagnitude);

		// A point preset pins anchors and pivot to the point; Stretch spans the parent.
		const std::optional<UIRectComponent> corner = ApplyUIAnchorPreset(rects[1], "BottomRight", parent);
		REQUIRE(corner);
		CHECK(corner->AnchorMin == glm::vec2(1.0f, 1.0f));
		CHECK(corner->AnchorMax == glm::vec2(1.0f, 1.0f));
		CHECK(corner->Pivot == glm::vec2(1.0f, 1.0f));
		CHECK(corner->Size == glm::vec2(752.0f, 552.0f)); // the stretched rect keeps its size
		const std::optional<UIRectComponent> stretched = ApplyUIAnchorPreset(rects[0], UIStretchPreset, parent);
		REQUIRE(stretched);
		CHECK(stretched->AnchorMax == glm::vec2(1.0f, 1.0f));
		CHECK_FALSE(ApplyUIAnchorPreset(rects[0], "Middle", parent));
	}
}
