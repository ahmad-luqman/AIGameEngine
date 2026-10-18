#include "Panels/JsonWidgets.h"

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

	TEST_CASE("The list widget draws nested lists without changing an untouched value")
	{
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = { 800, 600 };
		io.DeltaTime = 1.0f / 60.0f;
		unsigned char* pixels = nullptr;
		int width = 0, height = 0;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		const json spaces = { { { "Name", "Walk" }, { "Type", "1D" }, { "Samples", { { { "Clip", "Step" }, { "X", 0.0 }, { "Y", 0.0 }, { "Speed", 1.0 } } } } } };
		json value = spaces;
		for (int frame = 0; frame < 3; frame++)
		{
			ImGui::NewFrame();
			ImGui::Begin("Inspector");
			// Opens the list, its element and the element's Samples list, as a user would.
			ImGuiStorage* storage = ImGui::GetStateStorage();
			ImGui::PushID("BlendSpaces");
			storage->SetInt(ImGui::GetID("###list"), 1);
			ImGui::PushID(0);
			storage->SetInt(ImGui::GetID("##element"), 1);
			ImGui::PushID("Samples");
			storage->SetInt(ImGui::GetID("###list"), 1);
			ImGui::PopID();
			ImGui::PopID();
			ImGui::PopID();
			const JsonFieldResult result = DrawJsonList("BlendSpaces", value, AnimatorList("BlendSpaces"));
			CHECK_FALSE(result.Changed);
			CHECK_FALSE(result.Committed);
			ImGui::End();
			ImGui::Render();
		}
		CHECK(value == spaces);
		ImGui::DestroyContext();
	}
}
