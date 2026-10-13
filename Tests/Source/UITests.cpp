#include <doctest/doctest.h>

#include <Basalt/Core/FileSystem.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Project/Project.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>
#include <Basalt/UI/GameUI.h>
#include <Basalt/UI/TrueTypeValidator.h>
#include <Basalt/UI/UIFonts.h>
#include <Basalt/UI/UIGeometry.h>
#include <Basalt/UI/UILayout.h>
#include <Basalt/UI/UIRenderer.h>
#include <Basalt/UI/UIRules.h>

#include "TestUtils.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace Basalt;
using BasaltTest::Step;

namespace {

	const std::filesystem::path& GetFeatureTestDirectory()
	{
		static const std::filesystem::path directory = std::filesystem::path(BASALT_TEST_DATA_DIR) / "FeatureTest";
		return directory;
	}

	// The FuzzFont seed with composite glyphs (Karla has none).
	std::vector<uint8_t> ReadCousine()
	{
		std::optional<std::vector<uint8_t>> data = FileSystem::ReadBinaryFile(std::filesystem::path(BASALT_TEST_DATA_DIR) / "../Fuzz/Corpus/FuzzFont/Cousine-Regular.ttf");
		REQUIRE(data);
		return *data;
	}

	std::vector<uint8_t> ReadKarla()
	{
		std::optional<std::vector<uint8_t>> data = FileSystem::ReadBinaryFile(GetFeatureTestDirectory() / "Assets/Fonts/Karla-Regular.ttf");
		REQUIRE(data);
		return *data;
	}

	// An ImGui context for font and draw tests: no window or GPU, but the dynamic font atlas works on the CPU
	// once a renderer claims to handle textures.
	class ImGuiTestContext
	{
	public:
		ImGuiTestContext()
		{
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
			io.IniFilename = nullptr;
			io.DisplaySize = ImVec2(1920.0f, 1080.0f);
			io.DeltaTime = 1.0f / 60.0f;
		}
		~ImGuiTestContext()
		{
			UIFonts::Clear();
			ImGui::DestroyContext();
		}

		ImGuiTestContext(const ImGuiTestContext&) = delete;
		ImGuiTestContext& operator=(const ImGuiTestContext&) = delete;
	};

	// Big-endian access into a font file, for tests that corrupt one field.
	uint16_t ReadU16(const std::vector<uint8_t>& data, size_t at)
	{
		return static_cast<uint16_t>((data[at] << 8) | data[at + 1]);
	}

	uint32_t ReadU32(const std::vector<uint8_t>& data, size_t at)
	{
		return (static_cast<uint32_t>(ReadU16(data, at)) << 16) | ReadU16(data, at + 2);
	}

	void WriteU16(std::vector<uint8_t>& data, size_t at, uint16_t value)
	{
		data[at] = static_cast<uint8_t>(value >> 8);
		data[at + 1] = static_cast<uint8_t>(value & 0xFF);
	}

	void WriteU32(std::vector<uint8_t>& data, size_t at, uint32_t value)
	{
		WriteU16(data, at, static_cast<uint16_t>(value >> 16));
		WriteU16(data, at + 2, static_cast<uint16_t>(value & 0xFFFF));
	}

	// Offset of a table in a font file (0 when it has none).
	size_t FindTable(const std::vector<uint8_t>& data, const char* tag)
	{
		const uint16_t tables = ReadU16(data, 4);
		for (size_t i = 0; i < tables; i++)
		{
			const size_t record = 12 + 16 * i;
			if (std::memcmp(&data[record], tag, 4) == 0)
				return ReadU32(data, record + 8);
		}
		return 0;
	}

	// Offset of a glyph's data (relative to the file) and its length.
	std::pair<size_t, size_t> FindGlyph(const std::vector<uint8_t>& data, uint32_t glyph)
	{
		const size_t head = FindTable(data, "head");
		const size_t loca = FindTable(data, "loca");
		const size_t glyf = FindTable(data, "glyf");
		const bool longOffsets = ReadU16(data, head + 50) == 1;
		const auto offset = [&](size_t index) -> size_t { return longOffsets ? ReadU32(data, loca + 4 * index) : size_t{ ReadU16(data, loca + 2 * index) } * 2; };
		return { glyf + offset(glyph), offset(glyph + 1) - offset(glyph) };
	}

	uint32_t GlyphCount(const std::vector<uint8_t>& data)
	{
		return ReadU16(data, FindTable(data, "maxp") + 4);
	}

	// First glyph whose contour count passes `predicate` (simple: > 0, composite: < 0).
	template<typename Predicate>
	uint32_t FindGlyphWhere(const std::vector<uint8_t>& data, Predicate predicate)
	{
		for (uint32_t glyph = 0; glyph < GlyphCount(data); glyph++)
		{
			const auto [offset, length] = FindGlyph(data, glyph);
			if (length >= 10 && predicate(static_cast<int16_t>(ReadU16(data, offset))))
				return glyph;
		}
		FAIL("no such glyph in the font");
		return 0;
	}

	bool Contains(const std::string& text, const char* fragment)
	{
		return text.find(fragment) != std::string::npos;
	}

	// Number of log messages containing `fragment`.
	size_t CountLogMessages(const char* fragment)
	{
		size_t count = 0;
		for (const LogMessage& message : Log::GetHistory().GetAll())
		{
			if (Contains(message.Text, fragment))
				count++;
		}
		return count;
	}

}

TEST_SUITE("UI")
{
	TEST_CASE("GameUI collects immediate-mode commands per scene update and maps the 1080-unit canvas")
	{
		GameUI::BeginFrame();
		UIDrawCommand text;
		text.Kind = UIDrawKind::Text;
		text.Text = "Score";
		text.FontSize = 1000.0f;
		text.Color = { 2.0f, -1.0f, 0.5f, 1.0f };
		CHECK(GameUI::Submit(text));
		UIDrawCommand broken;
		broken.Position = { std::numeric_limits<float>::quiet_NaN(), 0.0f };
		CHECK_FALSE(GameUI::Submit(broken));
		UIDrawCommand rect;
		rect.Size = { -5.0f, 50.0f };
		CHECK(GameUI::Submit(rect));

		const std::vector<UIDrawCommand> commands = GameUI::GetCommands();
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
			REQUIRE(GameUI::Submit(UIDrawCommand{}));
		CHECK_FALSE(GameUI::Submit(UIDrawCommand{}));
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

		const std::vector<UILayoutItem> items = UILayout::Layout(scene, { 1920.0f, 1080.0f });
		REQUIRE(items.size() == 1);
		// AnchorMin.y takes its default (0.5) and AnchorMax.x is raised to AnchorMin.x.
		CHECK(items[0].Rect == UIBox{ { 860.0f, 490.0f }, { 200.0f, 100.0f } });
		std::vector<UIDrawCommand> commands;
		UILayout::BuildCommands(scene, { 1920.0f, 1080.0f }, commands);
		REQUIRE(commands.size() == 1);
		CHECK(commands[0].FontSize == UITextComponent::MaxFontSize);
		CHECK(commands[0].Align.x == 0.0f);
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

	TEST_CASE("The TrueType validator accepts real fonts and names what is wrong with broken ones")
	{
		const std::vector<uint8_t> karla = ReadKarla();
		CHECK(ValidateTrueTypeFont(karla).empty());

		CHECK(Contains(ValidateTrueTypeFont(std::vector<uint8_t>(50, 0)), "too small"));
		std::vector<uint8_t> junk(4096, 0x41);
		CHECK(Contains(ValidateTrueTypeFont(junk), "not a TrueType font"));
		std::vector<uint8_t> cff = karla;
		std::memcpy(cff.data(), "OTTO", 4);
		CHECK(Contains(ValidateTrueTypeFont(cff), "CFF"));
		std::vector<uint8_t> collection = karla;
		std::memcpy(collection.data(), "ttcf", 4);
		CHECK(Contains(ValidateTrueTypeFont(collection), "collections"));

		// Truncation cuts tables short.
		const std::vector<uint8_t> truncated(karla.begin(), karla.begin() + static_cast<std::ptrdiff_t>(karla.size() / 2));
		const std::string truncatedError = ValidateTrueTypeFont(truncated);
		CHECK_MESSAGE((Contains(truncatedError, "outside the file") || Contains(truncatedError, "truncated")), truncatedError);

		// A table directory pointing past the end of the file.
		std::vector<uint8_t> farTable = karla;
		for (size_t record = 12; record < 12 + 16 * size_t{ ReadU16(farTable, 4) }; record += 16)
		{
			if (std::memcmp(&farTable[record], "head", 4) == 0)
				WriteU32(farTable, record + 8, 0x7FFFFFF0u);
		}
		CHECK(Contains(ValidateTrueTypeFont(farTable), "outside the file"));

		// No horizontal metrics.
		std::vector<uint8_t> noMetrics = karla;
		WriteU16(noMetrics, FindTable(noMetrics, "hhea") + 34, 0);
		CHECK(Contains(ValidateTrueTypeFont(noMetrics), "horizontal metrics"));

		// Ascent below descent (ImGui would divide by zero or flip the font).
		std::vector<uint8_t> flat = karla;
		WriteU16(flat, FindTable(flat, "hhea") + 4, 0);
		WriteU16(flat, FindTable(flat, "hhea") + 6, 0);
		CHECK(Contains(ValidateTrueTypeFont(flat), "ascent"));

		// An unknown loca format.
		std::vector<uint8_t> badLoca = karla;
		WriteU16(badLoca, FindTable(badLoca, "head") + 50, 2);
		CHECK(Contains(ValidateTrueTypeFont(badLoca), "'loca' format"));

		// Glyph problems break only that glyph. A box that ends at the baseline while the outline rises above it
		// makes stb assert while rasterizing (the outline starts above the bitmap's first row).
		std::vector<uint8_t> overBaseline = karla;
		const uint32_t tall = FindGlyphWhere(overBaseline, [](int16_t contours) { return contours > 0; });
		const size_t tallOffset = FindGlyph(overBaseline, tall).first;
		REQUIRE(static_cast<int16_t>(ReadU16(overBaseline, tallOffset + 8)) > 0);
		WriteU16(overBaseline, tallOffset + 4, static_cast<uint16_t>(-300)); // yMin
		WriteU16(overBaseline, tallOffset + 8, 0);                           // yMax
		TrueTypeFontCheck check = CheckTrueTypeFont(overBaseline);
		CHECK(check.Error.empty());
		REQUIRE(check.BrokenGlyphs.size() == 1);
		CHECK(check.BrokenGlyphs[0].Index == tall);
		CHECK(Contains(check.BrokenGlyphs[0].Problem, "rises above the baseline"));
		CHECK(ValidateTrueTypeFont(overBaseline) == "glyph " + std::to_string(tall) + ": " + check.BrokenGlyphs[0].Problem);
		// Geometry outside the box elsewhere is clipped by the rasterizer: fine.
		std::vector<uint8_t> belowBaseline = karla;
		WriteU16(belowBaseline, tallOffset + 8, 5);
		CHECK(ValidateTrueTypeFont(belowBaseline).empty());

		// An inverted box would give ImGui a negative bitmap size; an oversized one would not fit the atlas.
		std::vector<uint8_t> inverted = karla;
		WriteU16(inverted, tallOffset + 6, static_cast<uint16_t>(static_cast<int16_t>(ReadU16(inverted, tallOffset + 2)) - 1)); // xMax < xMin
		CHECK(Contains(ValidateTrueTypeFont(inverted), "inverted"));
		std::vector<uint8_t> huge = karla;
		WriteU16(huge, tallOffset + 2, static_cast<uint16_t>(-30000));
		WriteU16(huge, tallOffset + 6, 30000);
		CHECK(Contains(ValidateTrueTypeFont(huge), "times the line height"));

		// Contour end points out of order.
		std::vector<uint8_t> unordered = karla;
		const uint32_t multi = FindGlyphWhere(unordered, [](int16_t contours) { return contours > 1; });
		WriteU16(unordered, FindGlyph(unordered, multi).first + 12, 0);
		CHECK(Contains(ValidateTrueTypeFont(unordered), "out of order"));

		// A composite glyph that contains itself would recurse until the stack overflows. (Karla has no
		// composite glyphs: turn a simple one into a composite of itself.)
		std::vector<uint8_t> cycle = karla;
		const uint32_t victim = FindGlyphWhere(cycle, [](int16_t contours) { return contours > 0; });
		const size_t victimOffset = FindGlyph(cycle, victim).first;
		WriteU16(cycle, victimOffset, 0xFFFF);                             // numberOfContours = -1
		WriteU16(cycle, victimOffset + 10, 0x0002);                        // ARGS_ARE_XY_VALUES, byte offsets, last component
		WriteU16(cycle, victimOffset + 12, static_cast<uint16_t>(victim)); // the component is the glyph itself
		WriteU16(cycle, victimOffset + 14, 0);
		CHECK(Contains(ValidateTrueTypeFont(cycle), "form a cycle"));

		// Point matching would make stb assert.
		std::vector<uint8_t> matching = cycle;
		WriteU16(matching, victimOffset + 10, 0x0000);
		CHECK(Contains(ValidateTrueTypeFont(matching), "point matching"));

		// A character mapped past the last glyph.
		std::vector<uint8_t> badMap = karla;
		const size_t cmap = FindTable(badMap, "cmap");
		size_t format4 = 0;
		for (size_t i = 0; i < ReadU16(badMap, cmap + 2) && format4 == 0; i++)
		{
			const size_t subtable = cmap + ReadU32(badMap, cmap + 4 + 8 * i + 4);
			if (ReadU16(badMap, subtable) == 4)
				format4 = subtable;
		}
		REQUIRE(format4 != 0);
		const size_t segmentsX2 = ReadU16(badMap, format4 + 6);
		const size_t startCodes = format4 + 16 + segmentsX2;
		const size_t deltas = startCodes + segmentsX2;
		const size_t rangeOffsets = deltas + segmentsX2;
		REQUIRE(ReadU16(badMap, rangeOffsets) == 0); // the first segment maps through its delta
		WriteU16(badMap, deltas, static_cast<uint16_t>(GlyphCount(badMap) - ReadU16(badMap, startCodes)));
		CHECK(Contains(ValidateTrueTypeFont(badMap), "but the font has"));
	}

	TEST_CASE("Composite glyphs: real ones pass, cycles and runaway transforms break only their glyph")
	{
		const std::vector<uint8_t> cousine = ReadCousine();
		CHECK(ValidateTrueTypeFont(cousine).empty());

		const uint32_t composite = FindGlyphWhere(cousine, [](int16_t contours) { return contours < 0; });
		const size_t offset = FindGlyph(cousine, composite).first;
		const uint16_t flags = ReadU16(cousine, offset + 10);
		REQUIRE((flags & 2) != 0); // positioned by offsets

		std::vector<uint8_t> cycle = cousine;
		WriteU16(cycle, offset + 12, static_cast<uint16_t>(composite));
		TrueTypeFontCheck check = CheckTrueTypeFont(cycle);
		CHECK(check.Error.empty());
		REQUIRE_FALSE(check.BrokenGlyphs.empty());
		CHECK(std::ranges::any_of(check.BrokenGlyphs, [&](const auto& glyph) { return glyph.Index == composite && Contains(glyph.Problem, "form a cycle"); }));
		std::vector<uint8_t> repaired = cycle;
		CHECK(RepairTrueTypeFont(repaired).empty());
		CHECK(ValidateTrueTypeFont(repaired).empty());

		// A component moved far away and scaled up would overflow stb's 16-bit vertex coordinates. Rewrite the
		// first component as: 16-bit offsets (bit 0), positioned by offsets (bit 1), one scale (bit 3), last.
		REQUIRE(FindGlyph(cousine, composite).second >= 20);
		std::vector<uint8_t> far = cousine;
		WriteU16(far, offset + 10, (1 << 0) | (1 << 1) | (1 << 3));
		WriteU16(far, offset + 14, 32000);  // x offset
		WriteU16(far, offset + 16, 0);      // y offset
		WriteU16(far, offset + 18, 0x7FFF); // scale ~2 (F2Dot14)
		const std::string farError = ValidateTrueTypeFont(far);
		CHECK_MESSAGE(Contains(farError, "beyond 16-bit coordinates"), farError);
	}

	TEST_CASE("stb only reads inside the character map of fonts the check accepts")
	{
		// Random changes to Karla's format 4 character map (its search parameters, segment ends and starts,
		// deltas and range offsets). For every changed font the check accepts, ImGui (stb) looks up every 16-bit
		// codepoint and loads the glyphs of some: an out-of-bounds read there fails under ASan, and a glyph
		// index past the end makes stb read past the metrics.
		ImGuiTestContext imgui;
		const std::vector<uint8_t> karla = ReadKarla();
		const size_t cmap = FindTable(karla, "cmap");
		size_t format4 = 0;
		for (size_t i = 0; i < ReadU16(karla, cmap + 2) && format4 == 0; i++)
		{
			const size_t subtable = cmap + ReadU32(karla, cmap + 4 + 8 * i + 4);
			if (ReadU16(karla, subtable) == 4)
				format4 = subtable;
		}
		REQUIRE(format4 != 0);
		const size_t length = 16 + 4 * size_t{ ReadU16(karla, format4 + 6) };

		std::mt19937 random(1234);
		int accepted = 0;
		int rejected = 0;
		for (int mutant = 0; mutant < 60; mutant++)
		{
			std::vector<uint8_t> font = karla;
			const int changes = 1 + static_cast<int>(random() % 3);
			for (int change = 0; change < changes; change++)
			{
				const size_t at = format4 + 6 + random() % (length - 6);
				font[at] = static_cast<uint8_t>(random() % 4 == 0 ? random() : font[at] ^ (1u << (random() % 8)));
			}
			std::string error;
			ImFont* loaded = UIFonts::LoadFromMemory(font, "Mutant", error);
			if (!loaded)
			{
				rejected++;
				continue;
			}
			accepted++;
			for (uint32_t c = 0; c <= 0xFFFF; c++)
				loaded->IsGlyphInFont(static_cast<ImWchar>(c));
			ImFontBaked* baked = loaded->GetFontBaked(20.0f);
			for (ImWchar c = 32; c < 127; c++)
				baked->FindGlyph(c);
		}
		INFO("accepted ", accepted, ", rejected ", rejected);
		CHECK(accepted > 5);
		CHECK(rejected > 5);
	}

	TEST_CASE("RepairTrueTypeFont blanks broken glyphs so ImGui can load and draw the rest of the font")
	{
		ImGuiTestContext imgui;
		const std::vector<uint8_t> karla = ReadKarla();
		std::vector<TrueTypeFontCheck::BrokenGlyph> repaired;
		std::vector<uint8_t> clean = karla;
		CHECK(RepairTrueTypeFont(clean, &repaired).empty());
		CHECK(repaired.empty());
		CHECK(clean == karla);

		// Break every glyph that rises above the baseline (so every letter is broken): one becomes a composite of
		// itself, the others get a box that ends at the baseline.
		std::vector<uint8_t> broken = karla;
		std::vector<uint32_t> victims;
		for (uint32_t glyph = 0; glyph < GlyphCount(broken); glyph++)
		{
			const auto [offset, length] = FindGlyph(broken, glyph);
			if (length < 16 || static_cast<int16_t>(ReadU16(broken, offset)) <= 0 || static_cast<int16_t>(ReadU16(broken, offset + 8)) <= 0)
				continue;
			victims.push_back(glyph);
			if (victims.size() == 1)
			{
				WriteU16(broken, offset, 0xFFFF);
				WriteU16(broken, offset + 10, 0x0002);
				WriteU16(broken, offset + 12, static_cast<uint16_t>(glyph));
				WriteU16(broken, offset + 14, 0);
				continue;
			}
			WriteU16(broken, offset + 4, static_cast<uint16_t>(-300));
			WriteU16(broken, offset + 8, 0);
		}
		REQUIRE(victims.size() > 50);

		CHECK(RepairTrueTypeFont(broken, &repaired).empty());
		REQUIRE(repaired.size() == victims.size());
		CHECK(Contains(repaired[0].Problem, "form a cycle"));
		CHECK(Contains(repaired[1].Problem, "rises above the baseline"));
		CHECK(ValidateTrueTypeFont(broken).empty());
		// A blanked glyph has no contours and an empty box.
		const size_t blankedOffset = FindGlyph(broken, victims[1]).first;
		for (size_t i = 0; i < 10; i++)
			CHECK(broken[blankedOffset + i] == 0);

		// The repaired font rasterizes every printable ASCII character at several sizes (stb would assert on
		// the unrepaired glyph).
		std::string error;
		ImFont* font = UIFonts::LoadFromMemory(broken, "Repaired", error);
		REQUIRE_MESSAGE(font, error);
		for (float size : { 8.0f, 32.0f, 200.0f })
		{
			ImFontBaked* baked = font->GetFontBaked(size);
			for (ImWchar c = 32; c < 127; c++)
				baked->FindGlyph(c);
		}
		CHECK(font->CalcTextSizeA(32.0f, FLT_MAX, 0.0f, "ABC xyz").x > 0.0f);

		// Font-wide problems cannot be repaired.
		std::vector<uint8_t> noMetrics = karla;
		WriteU16(noMetrics, FindTable(noMetrics, "hhea") + 34, 0);
		CHECK(Contains(RepairTrueTypeFont(noMetrics, &repaired), "horizontal metrics"));
		CHECK(repaired.empty());
	}

	TEST_CASE("UIFonts loads project fonts once, falls back to the built-in font and logs each failure once")
	{
		ImGuiTestContext imgui;
		// Loading a game font first still leaves the built-in font as ImGui's default (the atlas's first).
		std::string firstError;
		REQUIRE(UIFonts::LoadFromMemory(ReadKarla(), "Karla", firstError));
		CHECK(ImGui::GetIO().Fonts->Fonts[0] != ImGui::GetIO().Fonts->Fonts.back());
		ImFont* builtIn = UIFonts::GetDefault();
		CHECK(builtIn == ImGui::GetIO().Fonts->Fonts[0]);
		REQUIRE(builtIn);
		CHECK(UIFonts::Get("") == builtIn);

		std::string error;
		ImFont* karla = UIFonts::LoadFromMemory(ReadKarla(), "Karla", error);
		REQUIRE_MESSAGE(karla, error);
		const float karlaWidth = karla->CalcTextSizeA(40.0f, FLT_MAX, 0.0f, "Hello").x;
		CHECK(karlaWidth > 0.0f);
		CHECK(karlaWidth != builtIn->CalcTextSizeA(40.0f, FLT_MAX, 0.0f, "Hello").x);
		CHECK_FALSE(UIFonts::LoadFromMemory(std::vector<uint8_t>(200, 0), "Junk", error));
		CHECK(Contains(error, "not a TrueType font"));

		BasaltTest::TempProject project("UIFonts");
		const std::vector<uint8_t> data = ReadKarla();
		REQUIRE(FileSystem::WriteBinaryFile(project.GetDirectory() / "Assets/Fonts/Karla.ttf", data.data(), data.size()));
		ImFont* loaded = UIFonts::Get("Assets/Fonts/Karla.ttf");
		CHECK(loaded != builtIn);
		CHECK(UIFonts::Get("Assets/Fonts/../Fonts/Karla.ttf") == loaded); // one entry per file
		CHECK(UIFonts::GetError("Assets/Fonts/Karla.ttf").empty());

		const size_t before = CountLogMessages("Missing.ttf");
		CHECK(UIFonts::Get("Assets/Fonts/Missing.ttf") == builtIn);
		CHECK(UIFonts::Get("Assets/Fonts/Missing.ttf") == builtIn);
		CHECK(CountLogMessages("Missing.ttf") == before + 1);
		CHECK(Contains(UIFonts::GetError("Assets/Fonts/Missing.ttf"), "could not be read"));

		REQUIRE(FileSystem::WriteTextFile(project.GetDirectory() / "Assets/Fonts/Fake.ttf", std::string(500, 'x')));
		CHECK(UIFonts::Get("Assets/Fonts/Fake.ttf") == builtIn);
		CHECK(Contains(UIFonts::GetError("Assets/Fonts/Fake.ttf"), "not a TrueType font"));

		// Clearing forgets failures, so a fixed file loads.
		UIFonts::Clear();
		CHECK(UIFonts::GetError("Assets/Fonts/Missing.ttf").empty());
	}

	TEST_CASE("BreakLines breaks at newlines and between words, without trailing blanks")
	{
		ImGuiTestContext imgui;
		std::string error;
		ImFont* font = UIFonts::LoadFromMemory(ReadKarla(), "Karla", error);
		REQUIRE_MESSAGE(font, error);
		const float size = 32.0f;
		const auto width = [&](const char* text) { return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x; };

		std::vector<UITextLine> lines = UIRenderer::BreakLines(font, size, "Score\nLives   \n", 0.0f);
		REQUIRE(lines.size() == 3);
		CHECK(lines[0].Text == "Score");
		CHECK(lines[0].Width == doctest::Approx(width("Score")));
		CHECK(lines[1].Text == "Lives");
		CHECK(lines[1].Width == doctest::Approx(width("Lives")));
		CHECK(lines[2].Text.empty());

		lines = UIRenderer::BreakLines(font, size, "Hello world again", width("Hello world") + 2.0f);
		REQUIRE(lines.size() == 2);
		CHECK(lines[0].Text == "Hello world");
		CHECK(lines[1].Text == "again");

		// No wrapping without a width, however long the line.
		CHECK(UIRenderer::BreakLines(font, size, "Hello world again", 0.0f).size() == 1);
		// A width too small for anything still makes progress.
		lines = UIRenderer::BreakLines(font, size, "ab", 0.5f);
		CHECK(lines.size() == 2);
		CHECK(UIRenderer::BreakLines(font, size, "", 100.0f).size() == 1);
	}

	TEST_CASE("UIRenderer draws rects, images, nine-slice panels and aligned text into a draw list")
	{
		ImGuiTestContext imgui;
		ImGui::NewFrame();
		ImDrawList* drawList = ImGui::GetForegroundDrawList();

		UITextureMap textures;
		textures["Panel.png"] = UITexture{ 0x1234, { 32.0f, 32.0f } };

		std::vector<UIDrawCommand> commands(4);
		commands[0].Kind = UIDrawKind::Rect;
		commands[0].Position = { 0.0f, 0.0f };
		commands[0].Size = { 100.0f, 100.0f };
		commands[1].Kind = UIDrawKind::Image;
		commands[1].Texture = "Panel.png";
		commands[1].Size = { 200.0f, 100.0f };
		commands[1].Slice = { 8.0f, 8.0f, 8.0f, 8.0f };
		commands[2].Kind = UIDrawKind::Image;
		commands[2].Texture = "Missing.png"; // not resolved: filled with the color
		commands[2].Size = { 50.0f, 50.0f };
		commands[3].Kind = UIDrawKind::Text;
		commands[3].Text = "Right";
		commands[3].Position = { 1000.0f, 500.0f };
		commands[3].Size = { 400.0f, 100.0f };
		commands[3].Align = { 1.0f, 1.0f };
		commands[3].Shadow = false;

		// The 1920x1080 canvas drawn at half size: canvas units map to half a pixel.
		const int vertexCountBefore = drawList->VtxBuffer.Size;
		UIRenderer::Draw(drawList, { 0.0f, 0.0f }, { 960.0f, 540.0f }, commands, textures);

		int panelVertices = 0;
		for (const ImDrawCmd& command : drawList->CmdBuffer)
		{
			// TexRef, not GetTexID(): the font atlas was never uploaded (there is no GPU), which GetTexID asserts.
			if (command.TexRef._TexData == nullptr && command.TexRef._TexID == static_cast<ImTextureID>(0x1234))
				panelVertices += static_cast<int>(command.ElemCount);
		}
		CHECK(panelVertices == 9 * 6); // nine patches, two triangles each

		// The text's glyphs end at the box's bottom-right corner (1400, 600 canvas = 700, 300 pixels).
		float maxX = 0.0f;
		float minX = FLT_MAX;
		for (int i = vertexCountBefore + 4 + 9 * 4 + 4; i < drawList->VtxBuffer.Size; i++)
		{
			maxX = std::max(maxX, drawList->VtxBuffer[i].pos.x);
			minX = std::min(minX, drawList->VtxBuffer[i].pos.x);
		}
		CHECK(maxX <= 700.5f);
		CHECK(maxX > 680.0f);
		CHECK(minX > 600.0f);
		ImGui::EndFrame();
	}

	TEST_CASE("UIFrame puts UI components under this frame's immediate commands and resolves each texture once")
	{
		ImGuiTestContext imgui;
		Scene scene;
		Entity panel = scene.CreateEntity("Panel");
		panel.AddComponent<UIRectComponent>();
		panel.AddComponent<UIImageComponent>().Texture = "Assets/Textures/Panel.png";
		Entity label = scene.CreateEntity("Label");
		label.AddComponent<UIRectComponent>();
		label.AddComponent<UITextComponent>().Font = "Assets/Fonts/Missing.ttf";

		GameUI::BeginFrame();
		UIDrawCommand overlay;
		overlay.Kind = UIDrawKind::Image;
		overlay.Texture = "Assets/Textures/Panel.png";
		REQUIRE(GameUI::Submit(overlay));

		UIFrame frame;
		frame.Build(&scene, { 1920.0f, 1080.0f }, true);
		REQUIRE(frame.GetCommands().size() == 3);
		CHECK(frame.GetCommands()[0].Kind == UIDrawKind::Image);
		CHECK(frame.GetCommands()[1].Kind == UIDrawKind::Text);
		CHECK(frame.GetCommands()[2].Texture == overlay.Texture);

		int lookups = 0;
		const size_t missingFontErrors = CountLogMessages("Missing.ttf");
		frame.Prepare([&](const std::string& path) -> std::optional<UITexture> {
			lookups++;
			CHECK(path == "Assets/Textures/Panel.png");
			return UITexture{ 7, { 16.0f, 16.0f } };
		});
		CHECK(lookups == 1);
		CHECK(frame.GetTextures().at("Assets/Textures/Panel.png").Handle == 7);
		// Fonts load (and fail) while preparing, outside the ImGui frame.
		CHECK(CountLogMessages("Missing.ttf") == missingFontErrors + 1);

		frame.Build(&scene, { 1920.0f, 1080.0f }, false);
		CHECK(frame.GetCommands().size() == 2);
		frame.Build(nullptr, { 1920.0f, 1080.0f }, true);
		CHECK(frame.GetCommands().size() == 1);
		frame.Clear();
		CHECK(frame.GetCommands().empty());
		CHECK(frame.GetTextures().empty());
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

		const std::vector<UIDrawCommand> commands = GameUI::GetCommands();
		REQUIRE(commands.size() == 7);
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
			{ "UI.Text('x', 0/0, 0)", "must be finite" },
			{ "UI.Rect(0, 0, 1, 1, 'red')", "Vec4 color or an options table" },
			{ "UI.Rect(0, 0, 1, 1, { Tint = Vec4(1) })", "unknown option 'Tint'" },
			{ "UI.Image('', 0, 0, 1, 1)", "texture path is empty" },
			{ "UI.Image('a.png', 0, 0, 1, 1, { Slice = 4 })", "option 'Slice' must be a Vec4" },
			{ "UI.Image('a.png', 0, 0, 1, 1, { [1] = 4 })", "option keys must be names" },
			{ "UI.GetRect(nil)", "" },
		};
		BasaltTest::CheckLuaErrors(*scene.GetScriptEngine(), errors);
		scene.OnRuntimeStop();
		GameUI::BeginFrame();
	}
}
