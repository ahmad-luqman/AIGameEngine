#include <doctest/doctest.h>

#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>
#include <Basalt/UI/GameUI.h>
#include <Basalt/UI/UILayout.h>
#include <Basalt/UI/UIRenderer.h>
#include <Basalt/UI/UIRules.h>

#include "UITestUtils.h"
#include "TestUtils.h"

#include <imgui_internal.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Basalt;

TEST_SUITE("UI foundation")
{
	TEST_CASE("nested clips intersect and reset at the frame boundary")
	{
		GameUI::BeginFrame();
		REQUIRE(GameUI::PushClip({ { 0, 0 }, { 100, 100 } }));
		REQUIRE(GameUI::PushClip({ { 50, 20 }, { 100, 100 } }));
		UIDrawCommand command;
		command.Size = { 200, 200 };
		REQUIRE(GameUI::Submit(command) == GameUI::SubmitResult::Queued);
		std::vector<UIDrawCommand> commands;
		GameUI::AppendCommands(commands);
		REQUIRE(commands.size() == 1);
		REQUIRE(commands[0].Clip);
		CHECK(commands[0].Clip->Min == glm::vec2(50, 20));
		CHECK(commands[0].Clip->Size == glm::vec2(50, 80));
		REQUIRE(GameUI::PopClip());
		REQUIRE(GameUI::PopClip());
		CHECK_FALSE(GameUI::PopClip());
		GameUI::BeginFrame();
		CHECK_FALSE(GameUI::GetClip());
	}

	TEST_CASE("rect transform rotates and scales about its pivot and hit tests in local space")
	{
		UIRectComponent rect;
		rect.Rotation = glm::half_pi<float>();
		rect.Scale = { 2, 1 };
		const UIBox box{ { 10, 20 }, { 100, 40 } };
		const glm::mat3 transform = ComputeUITransform(box, rect);
		const glm::vec2 pivot = glm::vec2(transform * glm::vec3(box.Center(), 1));
		CHECK(pivot.x == doctest::Approx(60));
		CHECK(pivot.y == doctest::Approx(40));
		const UIBox bounds = TransformUIBox(box, transform);
		CHECK(bounds.Size.x == doctest::Approx(40));
		CHECK(bounds.Size.y == doctest::Approx(200));
		CHECK(ContainsUIBox(box, transform, { 60, 120 }));
		CHECK_FALSE(ContainsUIBox(box, transform, { 90, 40 }));
	}

	TEST_CASE("scroll descendants share clipped offset layout with GetRect")
	{
		Scene scene;
		Entity parent = scene.CreateEntity("Viewport");
		auto& rect = parent.AddComponent<UIRectComponent>();
		rect.AnchorMin = rect.AnchorMax = rect.Pivot = { 0, 0 };
		rect.Size = { 100, 80 };
		auto& scroll = parent.AddComponent<UIScrollViewComponent>();
		scroll.ContentSize = { 100, 300 };
		scroll.Offset = { 0, 50 };
		Entity child = scene.CreateEntity("Content");
		REQUIRE(scene.SetParent(child, parent));
		child.AddComponent<UIImageComponent>();
		const auto items = UILayout::Layout(scene, { 1920, 1080 });
		REQUIRE(items.size() == 2);
		CHECK(items[1].Rect.Min == glm::vec2(0, -50));
		CHECK(items[1].Rect.Size == glm::vec2(100, 300));
		REQUIRE(items[1].Clip);
		CHECK(items[1].Contains({ 20, 20 }));
		CHECK_FALSE(items[1].Contains({ 20, 100 }));
		CHECK(UILayout::GetRect(scene, child, { 1920, 1080 }) == items[1].Rect);
	}

	TEST_CASE("sprite sheet rules reject zero grid and out of range frames")
	{
		UIImageComponent image;
		image.SpriteColumns = 4;
		image.SpriteRows = 2;
		image.SpriteFrame = 7;
		CHECK(ValidateUIImage(image).empty());
		image.SpriteFrame = 8;
		CHECK_FALSE(ValidateUIImage(image).empty());
		CHECK(SanitizeUIImage(image).SpriteFrame == 0);
		image.SpriteColumns = 0;
		CHECK_FALSE(ValidateUIImage(image).empty());
		CHECK(SanitizeUIImage(image).SpriteColumns == 1);
	}

	TEST_CASE("text measurement uses drawing line breaks and works without a GPU")
	{
		BasaltTest::ImGuiTestContext context;
		const auto single = UIRenderer::MeasureText("Hello", 24);
		const auto doubleLine = UIRenderer::MeasureText("Hello\nHello", 24);
		CHECK(single.x > 0);
		CHECK(single.y == 24);
		CHECK(doubleLine.x == single.x);
		CHECK(doubleLine.y == 48);
		const auto wrapped = UIRenderer::MeasureText("Hello Hello", 24, {}, single.x + 1);
		CHECK(wrapped.y == 48);
		CHECK(UIRenderer::MeasureText("", 24) == glm::vec2(0));
	}
}

TEST_CASE("transformed UI text retains glyphs before final GPU clipping")
{
	BasaltTest::ImGuiTestContext context;
	ImGui::NewFrame();
	BasaltTest::ImGuiTestContext::MarkTexturesUploaded();
	ImDrawList* list = ImGui::GetForegroundDrawList();
	UIDrawCommand command;
	command.Kind = UIDrawKind::Text;
	command.Text = "Visible";
	command.Position = { -500, -500 };
	command.Transform[2] = { 600, 600, 1 };
	command.Clip = UIBox{ { 90, 90 }, { 300, 100 } };
	UIRenderer::Draw(list, { 0, 0 }, { 1920, 1080 }, { 1920, 1080 }, std::span(&command, 1), {});
	BasaltTest::ImGuiTestContext::MarkTexturesUploaded();
	CHECK(list->VtxBuffer.Size > 0);
	for (const auto& draw : list->CmdBuffer)
	{
		if (!draw.ElemCount)
			continue;
		CHECK(draw.ClipRect.x == 90);
		CHECK(draw.ClipRect.y == 90);
		CHECK(draw.ClipRect.z == 390);
		CHECK(draw.ClipRect.w == 190);
	}
	ImGui::EndFrame();
}

TEST_CASE("sprite sheet rendering maps UV rectangles to the selected row-major cell")
{
	BasaltTest::ImGuiTestContext context;
	ImGui::NewFrame();
	BasaltTest::ImGuiTestContext::MarkTexturesUploaded();
	ImDrawList* list = ImGui::GetForegroundDrawList();
	UIDrawCommand command;
	command.Kind = UIDrawKind::Image;
	command.Texture = "sheet";
	command.Size = { 100, 100 };
	command.SpriteColumns = 4;
	command.SpriteRows = 2;
	command.SpriteFrame = 5;
	command.UVMin = { 0.2f, 0.2f };
	command.UVMax = { 1, 1 };
	UIResources resources;
	resources.Textures["sheet"] = { 123, { 400, 200 } };
	UIRenderer::Draw(list, { 0, 0 }, { 1920, 1080 }, { 1920, 1080 }, std::span(&command, 1), resources);
	REQUIRE(list->VtxBuffer.Size == 4);
	CHECK(list->VtxBuffer[0].uv.x == doctest::Approx(0.4f));
	CHECK(list->VtxBuffer[0].uv.y == doctest::Approx(0.6f));
	CHECK(list->VtxBuffer[2].uv.x == doctest::Approx(0.6f));
	CHECK(list->VtxBuffer[2].uv.y == doctest::Approx(1.0f));
	ImGui::EndFrame();
}

TEST_CASE("MeasureText creates and releases a context in headless sessions")
{
	REQUIRE(ImGui::GetCurrentContext() == nullptr);
	const auto extent = UIRenderer::MeasureText("Headless", 20);
	CHECK(extent.x > 0);
	CHECK(extent.y == 20);
	CHECK(ImGui::GetCurrentContext() == nullptr);
}

TEST_CASE("UI foundation Lua options validate transforms sprites and clip stack")
{
	Scene scene;
	scene.OnRuntimeStart();
	CHECK(BasaltTest::Eval(scene, "local s=UI.MeasureText('Hello', {Size=20}); assert(s.x>0 and s.y==20)") == "");
	CHECK(BasaltTest::Eval(scene, "UI.PushClip(0,0,100,100); UI.Rect(0,0,200,200,{Rotation=45,Scale=Vec2(2,1)}); UI.PopClip()") == "");
	CHECK(BasaltTest::Eval(scene, "UI.Image('sheet',0,0,20,20,{UVMin=Vec2(0.2,0),UVMax=Vec2(1,1),SpriteColumns=4,SpriteRows=2,SpriteFrame=7})") == "");
	CHECK_FALSE(BasaltTest::Eval(scene, "UI.PopClip()").empty());
	CHECK_FALSE(BasaltTest::Eval(scene, "UI.Image('sheet',0,0,20,20,{SpriteColumns=2.5})").empty());
	CHECK_FALSE(BasaltTest::Eval(scene, "UI.MeasureText('Hello',{Size=0})").empty());
	scene.OnRuntimeStop();
}

TEST_CASE("composed UI scales stop unsafe subtrees and retain finite renderer output")
{
	for (const float scale : { 0.001f, 1000.0f })
	{
		Scene scene;
		Entity parent;
		for (int i = 0; i < 24; ++i)
		{
			Entity entity = scene.CreateEntity("Scaled");
			if (parent)
				REQUIRE(scene.SetParent(entity, parent));
			auto& rect = entity.AddComponent<UIRectComponent>();
			rect.AnchorMin = rect.AnchorMax = rect.Pivot = { 0, 0 };
			rect.Size = { 100, 100 };
			rect.Scale = { scale, scale };
			rect.ClipChildren = true;
			entity.AddComponent<UIImageComponent>();
			parent = entity;
		}
		const auto items = UILayout::Layout(scene, { 1920, 1080 });
		CHECK_FALSE(items.empty());
		CHECK(items.size() < 24);
		for (const auto& item : items)
			CHECK(IsUsableUITransform(item.Rect, item.Transform));
		std::vector<UIDrawCommand> commands;
		UILayout::BuildCommands(scene, { 1920, 1080 }, commands);
		BasaltTest::ImGuiTestContext context;
		ImGui::NewFrame();
		BasaltTest::ImGuiTestContext::MarkTexturesUploaded();
		ImDrawList* list = ImGui::GetForegroundDrawList();
		UIRenderer::Draw(list, { 0, 0 }, { 1920, 1080 }, { 1920, 1080 }, commands, {});
		for (const auto& vertex : list->VtxBuffer)
		{
			CHECK(std::isfinite(vertex.pos.x));
			CHECK(std::isfinite(vertex.pos.y));
		}
		ImGui::EndFrame();
	}
}

TEST_CASE("small usable UI transforms retain inverse hit testing")
{
	const UIBox box{ { 0, 0 }, { 100, 100 } };
	glm::mat3 transform(1);
	transform[0][0] = transform[1][1] = 1e-8f;
	CHECK(IsUsableUITransform(box, transform));
	CHECK(ContainsUIBox(box, transform, { 5e-7f, 5e-7f }));
	CHECK_FALSE(ContainsUIBox(box, transform, { 2e-6f, 5e-7f }));
	transform[0][0] = std::numeric_limits<float>::infinity();
	CHECK_FALSE(IsUsableUITransform(box, transform));
	CHECK_FALSE(ContainsUIBox(box, transform, { 0, 0 }));
}

TEST_CASE("UI renderer suppresses transformed glyphs outside its finite numeric envelope")
{
	BasaltTest::ImGuiTestContext context;
	ImGui::NewFrame();
	BasaltTest::ImGuiTestContext::MarkTexturesUploaded();
	ImDrawList* list = ImGui::GetForegroundDrawList();
	UIDrawCommand command;
	command.Kind = UIDrawKind::Text;
	command.Text = "Huge glyph";
	command.Transform[0][0] = command.Transform[1][1] = 1e9f;
	UIRenderer::Draw(list, { 0, 0 }, { 1920, 1080 }, { 1920, 1080 }, std::span(&command, 1), {});
	BasaltTest::ImGuiTestContext::MarkTexturesUploaded();
	for (const auto& vertex : list->VtxBuffer)
	{
		CHECK(std::isfinite(vertex.pos.x));
		CHECK(std::isfinite(vertex.pos.y));
		CHECK(vertex.pos.x == 0);
		CHECK(vertex.pos.y == 0);
	}
	for (const auto& draw : list->CmdBuffer)
		if (draw.ElemCount)
			CHECK(draw.ClipRect.x == draw.ClipRect.z);
	ImGui::EndFrame();
}

TEST_CASE("text fields retain their own clip and intersect inherited clipping after transforms")
{
	for (int variant = 0; variant < 3; ++variant)
	{
		Scene scene;
		Entity parent = scene.CreateEntity("Parent clip");
		auto& parentRect = parent.AddComponent<UIRectComponent>();
		parentRect.AnchorMin = parentRect.AnchorMax = parentRect.Pivot = { 0, 0 };
		parentRect.Position = { 50, 50 };
		parentRect.Size = { 120, 60 };
		parentRect.ClipChildren = variant != 0;
		Entity field = scene.CreateEntity("Long text field");
		REQUIRE(scene.SetParent(field, parent));
		auto& rect = field.AddComponent<UIRectComponent>();
		rect.AnchorMin = rect.AnchorMax = rect.Pivot = { 0, 0 };
		rect.Position = { 20, 20 };
		rect.Size = { 200, 40 };
		if (variant == 2)
		{
			rect.Rotation = glm::radians(30.0f);
			rect.Scale = { 1.25f, 0.8f };
		}
		field.AddComponent<UITextInputComponent>().Text = "A very long text field value that extends well beyond its rectangle";
		scene.OnRuntimeStart();
		REQUIRE(BasaltTest::Eval(scene, "UI.SetFocus(Scene.FindEntityByName('Long text field'))") == "");
		const auto items = UILayout::Layout(scene, { 1920, 1080 });
		const auto item = std::find_if(items.begin(), items.end(), [&](const auto& value) { return value.Owner == field; });
		REQUIRE(item != items.end());
		const UIBox fieldClip = TransformUIBox(item->Rect, item->Transform);
		const UIBox expected = item->Clip ? IntersectUIBoxes(fieldClip, *item->Clip) : fieldClip;
		std::vector<UIDrawCommand> commands;
		UILayout::BuildCommands(scene, { 1920, 1080 }, commands);
		const auto text = std::find_if(commands.begin(), commands.end(), [](const auto& value) { return value.Kind == UIDrawKind::Text; });
		REQUIRE(text != commands.end());
		REQUIRE(text->Clip);
		CHECK(*text->Clip == expected);
		CHECK(text->Transform == item->Transform);
		CHECK(text->Text.find('|') != std::string::npos);
		BasaltTest::ImGuiTestContext context;
		ImGui::NewFrame();
		BasaltTest::ImGuiTestContext::MarkTexturesUploaded();
		ImDrawList* list = ImGui::GetForegroundDrawList();
		UIRenderer::Draw(list, { 0, 0 }, { 1920, 1080 }, { 1920, 1080 }, std::span(&*text, 1), {});
		BasaltTest::ImGuiTestContext::MarkTexturesUploaded();
		for (const auto& draw : list->CmdBuffer)
		{
			if (!draw.ElemCount)
				continue;
			CHECK(draw.ClipRect.x == doctest::Approx(expected.Min.x));
			CHECK(draw.ClipRect.y == doctest::Approx(expected.Min.y));
			CHECK(draw.ClipRect.z == doctest::Approx(expected.Max().x));
			CHECK(draw.ClipRect.w == doctest::Approx(expected.Max().y));
		}
		ImGui::EndFrame();
		scene.OnRuntimeStop();
	}
}
