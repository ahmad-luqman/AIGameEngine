#include "Panels/JsonWidgets.h"

#include "UITestUtils.h"

#include <Basalt/Scene/ComponentRegistry.h>

#include <doctest/doctest.h>
#include <imgui.h>

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

	// The open state of the BlendSpaces list node (index -1) or of one of its element nodes.
	int& NodeState(int index)
	{
		ImGui::PushID("BlendSpaces");
		ImGuiID id = ImGui::GetID("###list");
		if (index >= 0)
		{
			ImGui::PushID(index);
			id = ImGui::GetID("##element");
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
	}

	TEST_CASE("The list widget draws nested lists without changing them, and its Add button appends")
	{
		BasaltTest::ImGuiTestContext context;
		ImGuiIO& io = ImGui::GetIO();
		const json spaces = { { { "Name", "Walk" }, { "Type", "1D" }, { "Samples", { { { "Clip", "Step" }, { "X", 0.0 }, { "Y", 0.0 }, { "Speed", 1.0 } } } } } };
		json value = spaces;
		// Open the list and its element, as a user would.
		InWindow([] {
			NodeState(-1) = 1;
			NodeState(0) = 1;
		});
		Frame frame;
		for (int i = 0; i < 3; i++)
		{
			frame = DrawFrame(value);
			CHECK_FALSE(frame.Result.Changed);
			CHECK_FALSE(frame.Result.Committed);
		}
		CHECK(value == spaces);
		// The open list ends with its Add button: press and release over it.
		REQUIRE(frame.LastItemMax.y > frame.LastItemMin.y);
		io.AddMousePosEvent((frame.LastItemMin.x + frame.LastItemMax.x) / 2, (frame.LastItemMin.y + frame.LastItemMax.y) / 2);
		DrawFrame(value);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		DrawFrame(value);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		frame = DrawFrame(value);
		CHECK(frame.Result.Changed);
		CHECK(frame.Result.Committed);
		REQUIRE(value.size() == 2);
		CHECK(value[1]["Name"] == "BlendSpace");
		// The new element's node starts closed; the first stays open.
		InWindow([] {
			CHECK(NodeState(0) == 1);
			CHECK(NodeState(1) == 0);
		});
	}
}
