#include "Panels/JsonWidgets.h"

#include "UITestUtils.h"

#include <Basalt/Scene/ComponentRegistry.h>

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>

using namespace Basalt;
using json = nlohmann::json;

namespace {
	const ComponentFieldType& AnimatorList(const char* field)
	{
		return ComponentRegistry::Find("Animator")->FieldTypes.at(field);
	}

	json Names(const json& array)
	{
		json names = json::array();
		for (const json& element : array)
			names.push_back(element["Name"]);
		return names;
	}

	// One frame of the BlendSpaces list in an inspector-like window: the widget's result and the rectangle of the
	// last item drawn (the list's Add button while the list is open and not full).
	struct Frame
	{
		JsonFieldResult Result;
		ImVec2 LastItemMin, LastItemMax;
	};

	Frame DrawFrame(json& value)
	{
		ImGui::NewFrame();
		BasaltTest::ImGuiTestContext::MarkTexturesUploaded();
		ImGui::SetNextWindowPos({ 0, 0 });
		ImGui::SetNextWindowSize({ 600, 900 });
		ImGui::Begin("Inspector");
		Frame frame;
		frame.Result = DrawJsonList("BlendSpaces", value, AnimatorList("BlendSpaces"));
		frame.LastItemMin = ImGui::GetItemRectMin();
		frame.LastItemMax = ImGui::GetItemRectMax();
		ImGui::End();
		ImGui::Render();
		return frame;
	}

	// The open state of the BlendSpaces list node (index -1) or of one of its element nodes, which live under the
	// open list node's ID.
	int& NodeState(int index)
	{
		ImGui::PushID("BlendSpaces");
		ImGuiID id = ImGui::GetID("###list");
		if (index >= 0)
		{
			ImGui::PushOverrideID(id);
			ImGui::PushID(index);
			id = ImGui::GetID("##element");
			ImGui::PopID();
			ImGui::PopID();
		}
		ImGui::PopID();
		return *ImGui::GetStateStorage()->GetIntRef(id, 0);
	}

	// Runs `use` inside the inspector window, where the widget's IDs live.
	template<typename F>
	void InWindow(F&& use)
	{
		ImGui::NewFrame();
		ImGui::Begin("Inspector");
		use();
		ImGui::End();
		ImGui::EndFrame();
	}
}

TEST_SUITE("Editor")
{
	TEST_CASE("List edits add defaults, duplicate, move and remove elements within the list's bounds")
	{
		const ComponentFieldType& events = AnimatorList("Events");
		json list = json::array();
		REQUIRE(EditJsonList(list, JsonListEdit::Add, 0, events));
		CHECK(list == json::array({ events.Element->Default }));
		list[0]["Name"] = "A";
		REQUIRE(EditJsonList(list, JsonListEdit::Duplicate, 0, events));
		// Event names may repeat (footsteps): only a schema's UniqueField is made unique.
		CHECK(Names(list) == json({ "A", "A" }));
		list[1]["Name"] = "B";
		REQUIRE(EditJsonList(list, JsonListEdit::Add, 99, events));
		list[2]["Name"] = "C";
		CHECK(Names(list) == json({ "A", "B", "C" }));
		CHECK_FALSE(EditJsonList(list, JsonListEdit::MoveUp, 0, events));
		CHECK_FALSE(EditJsonList(list, JsonListEdit::MoveDown, 2, events));
		CHECK_FALSE(EditJsonList(list, JsonListEdit::Remove, 3, events));
		CHECK_FALSE(EditJsonList(list, JsonListEdit::Duplicate, 3, events));
		REQUIRE(EditJsonList(list, JsonListEdit::MoveDown, 0, events));
		CHECK(Names(list) == json({ "B", "A", "C" }));
		REQUIRE(EditJsonList(list, JsonListEdit::MoveUp, 2, events));
		CHECK(Names(list) == json({ "B", "C", "A" }));
		REQUIRE(EditJsonList(list, JsonListEdit::Duplicate, 1, events));
		CHECK(Names(list) == json({ "B", "C", "C", "A" }));
		REQUIRE(EditJsonList(list, JsonListEdit::Remove, 0, events));
		CHECK(Names(list) == json({ "C", "C", "A" }));

		// Growing stops at MaxCount; a value that is not an array is left alone.
		json full = json::array_t(events.MaxCount, events.Element->Default);
		CHECK_FALSE(EditJsonList(full, JsonListEdit::Add, 0, events));
		CHECK_FALSE(EditJsonList(full, JsonListEdit::Duplicate, 0, events));
		CHECK(full.size() == events.MaxCount);
		REQUIRE(EditJsonList(full, JsonListEdit::Remove, 0, events));
		json notList = json::object();
		CHECK_FALSE(EditJsonList(notList, JsonListEdit::Add, 0, events));
		CHECK(notList == json::object());
	}

	TEST_CASE("Added and duplicated elements get a fresh value of the schema's unique field")
	{
		const ComponentFieldType& spaces = AnimatorList("BlendSpaces");
		REQUIRE(spaces.Element->UniqueField == "Name");
		json list = json::array();
		REQUIRE(EditJsonList(list, JsonListEdit::Add, 0, spaces));
		REQUIRE(EditJsonList(list, JsonListEdit::Add, 0, spaces));
		list[0]["Name"] = "Walk";
		REQUIRE(EditJsonList(list, JsonListEdit::Duplicate, 0, spaces));
		REQUIRE(EditJsonList(list, JsonListEdit::Duplicate, 0, spaces));
		CHECK(Names(list) == json({ "Walk", "Walk 3", "Walk 2", "BlendSpace 2" }));
		// Duplicating a numbered name numbers from its base.
		REQUIRE(EditJsonList(list, JsonListEdit::Duplicate, 2, spaces));
		CHECK(list[3]["Name"] == "Walk 4");
	}

	TEST_CASE("The list widget draws nested lists without changing them, and its buttons edit the list")
	{
		BasaltTest::ImGuiTestContext context;
		ImGuiIO& io = ImGui::GetIO();
		const json spaces = { { { "Name", "Walk" }, { "Type", "1D" }, { "Samples", { { { "Clip", "Step" }, { "X", 0.0 }, { "Y", 0.0 }, { "Speed", 1.0 } } } } } };
		json value = spaces;
		// Presses and releases the left button at `position` over three frames.
		auto click = [&](ImVec2 position) {
			io.AddMousePosEvent(position.x, position.y);
			DrawFrame(value);
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			DrawFrame(value);
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			return DrawFrame(value);
		};
		// The list's header row (the last item while the list is closed), then the Add button with the element closed.
		const Frame closed = DrawFrame(value);
		const float row = closed.LastItemMax.y - closed.LastItemMin.y;
		InWindow([] { NodeState(-1) = 1; });
		const Frame collapsed = DrawFrame(value);
		// Opening the element draws its fields, nested Samples list included, pushing the Add button down.
		InWindow([] { NodeState(0) = 1; });
		Frame frame;
		for (int i = 0; i < 3; i++)
		{
			frame = DrawFrame(value);
			CHECK_FALSE(frame.Result.Changed);
			CHECK_FALSE(frame.Result.Committed);
		}
		CHECK(value == spaces);
		CHECK(frame.LastItemMin.y > collapsed.LastItemMin.y + 3 * row);
		// Add appends a default element; its node starts closed.
		frame = click({ (frame.LastItemMin.x + frame.LastItemMax.x) / 2, (frame.LastItemMin.y + frame.LastItemMax.y) / 2 });
		CHECK(frame.Result.Changed);
		CHECK(frame.Result.Committed);
		REQUIRE(value.size() == 2);
		CHECK(value[1]["Name"] == "BlendSpace");
		InWindow([] {
			CHECK(NodeState(0) == 1);
			CHECK(NodeState(1) == 0);
		});
		// The first element's down arrow (second button from the right of its row, the row below the header) moves it
		// down, and its open node moves with it.
		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		const ImVec2 down = { 600.0f - ImGui::GetStyle().WindowPadding.x - row - spacing - row / 2, closed.LastItemMax.y + ImGui::GetStyle().ItemSpacing.y + row / 2 };
		frame = click(down);
		CHECK(frame.Result.Committed);
		CHECK(Names(value) == json({ "BlendSpace", "Walk" }));
		InWindow([] {
			CHECK(NodeState(0) == 0);
			CHECK(NodeState(1) == 1);
		});
	}
}

TEST_SUITE("Editor")
{
	TEST_CASE("Curve list edits preserve existing times and keep inserted keys strictly ordered")
	{
		const auto& type = ComponentRegistry::Find("ParticleEmitter")->FieldTypes.at("SizeCurve");
		json list = json::array();
		REQUIRE(EditJsonList(list, JsonListEdit::Add, 0, type));
		REQUIRE(EditJsonList(list, JsonListEdit::Add, 0, type));
		CHECK(list[0]["Time"] == 0);
		CHECK(list[1]["Time"] == 1);
		list[0]["Value"] = 2;
		list[1]["Value"] = 4;
		REQUIRE(EditJsonList(list, JsonListEdit::Duplicate, 0, type));
		CHECK(list[1]["Time"] == 0.5);
		CHECK(list[1]["Value"] == 2);
		REQUIRE(EditJsonList(list, JsonListEdit::MoveDown, 1, type));
		CHECK(list[1]["Time"] == 0.5);
		CHECK(list[1]["Value"] == 4);
		CHECK(list[2]["Value"] == 2);
		while (list.size() < type.MaxCount)
			REQUIRE(EditJsonList(list, JsonListEdit::Add, 0, type));
		CHECK_FALSE(EditJsonList(list, JsonListEdit::Add, 0, type));
		for (size_t i = 1; i < list.size(); ++i)
			CHECK(list[i]["Time"].get<float>() > list[i - 1]["Time"].get<float>());
	}

	TEST_CASE("Duplicating a middle curve key prefers its following interval and reports its actual slot")
	{
		const auto& type = ComponentRegistry::Find("ParticleEmitter")->FieldTypes.at("SizeCurve");
		json list = { { { "Time", 0 }, { "Value", 1 } }, { { "Time", 0.5 }, { "Value", 2 } }, { { "Time", 0.6 }, { "Value", 3 } }, { { "Time", 1 }, { "Value", 4 } } };
		size_t inserted = 99;
		REQUIRE(EditJsonList(list, JsonListEdit::Duplicate, 2, type, &inserted));
		CHECK(inserted == 3);
		CHECK(list[3]["Time"] == doctest::Approx(0.8));
		CHECK(list[3]["Value"] == 3);
		REQUIRE(EditJsonList(list, JsonListEdit::Add, 0, type, &inserted));
		CHECK(inserted == 1);
		CHECK(list[1]["Time"] == doctest::Approx(0.25));
	}
}

TEST_SUITE("Editor")
{
	TEST_CASE("A curve duplicate with no following float midpoint searches later free intervals")
	{
		const auto& type = ComponentRegistry::Find("ParticleEmitter")->FieldTypes.at("SizeCurve");
		const float narrow = std::nextafter(0.1f, 1.0f);
		json list = { { { "Time", 0.0f }, { "Value", 1 } }, { { "Time", 0.1f }, { "Value", 2 } }, { { "Time", narrow }, { "Value", 3 } }, { { "Time", 1.0f }, { "Value", 4 } } };
		size_t inserted = 0;
		REQUIRE(EditJsonList(list, JsonListEdit::Duplicate, 1, type, &inserted));
		CHECK(inserted == 3);
		CHECK(list[3]["Value"] == 2);
		CHECK(list[3]["Time"].get<float>() > 0.5f);
	}
}

TEST_SUITE("Editor")
{
	TEST_CASE("Timed list edits reject malformed external JSON without mutation")
	{
		const auto& type = ComponentRegistry::Find("ParticleEmitter")->FieldTypes.at("SizeCurve");
		for (const json& time : { json("oops"), json(1e300), json(-1) })
		{
			json list = { { { "Time", time }, { "Value", 1 } } };
			const json before = list;
			CHECK_FALSE(EditJsonList(list, JsonListEdit::Add, 0, type));
			CHECK(list == before);
		}
		json missing = { { { "Value", 1 } } };
		const json before = missing;
		CHECK_FALSE(EditJsonList(missing, JsonListEdit::Duplicate, 0, type));
		CHECK(missing == before);
	}
}
