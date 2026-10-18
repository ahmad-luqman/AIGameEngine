#include "Panels/JsonWidgets.h"

#include "EditorContext.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <optional>
#include <utility>

namespace Basalt {

	namespace {

		JsonFieldResult Finish(bool changed)
		{
			return { changed, ImGui::IsItemDeactivatedAfterEdit() };
		}

		// "#2 Footstep": the element's position and its first non-empty Name or Clip.
		std::string ElementLabel(const nlohmann::json& element, size_t index)
		{
			std::string label = "#" + std::to_string(index);
			for (const char* key : { "Name", "Clip" })
			{
				if (auto it = element.find(key); it != element.end() && it->is_string())
					if (const auto& text = it->get_ref<const std::string&>(); !text.empty())
						return label.append(" ").append(text);
			}
			return label;
		}

		// Gives the element at `index` a value of `field` no other element has: "Walk", then "Walk 2", "Walk 3"...
		void MakeUnique(nlohmann::json& array, size_t index, const std::string& field)
		{
			const auto taken = [&](const std::string& candidate) {
				for (size_t i = 0; i < array.size(); i++)
					if (i != index && array[i].value(field, "") == candidate)
						return true;
				return false;
			};
			const std::string base = array[index].value(field, "");
			std::string candidate = base;
			for (int suffix = 2; taken(candidate); suffix++)
				candidate = base + " " + std::to_string(suffix);
			array[index][field] = candidate;
		}

		// The structural part of a list edit, shared by element values and their tree-node open states.
		bool ApplyListEdit(nlohmann::json& array, JsonListEdit edit, size_t index, const nlohmann::json& fresh)
		{
			switch (edit)
			{
				case JsonListEdit::Add:
					array.push_back(fresh);
					return true;
				case JsonListEdit::Duplicate:
					if (index >= array.size())
						return false;
					array.insert(array.begin() + static_cast<std::ptrdiff_t>(index) + 1, array[index]);
					return true;
				case JsonListEdit::Remove:
					if (index >= array.size())
						return false;
					array.erase(array.begin() + static_cast<std::ptrdiff_t>(index));
					return true;
				case JsonListEdit::MoveUp:
					if (index == 0 || index >= array.size())
						return false;
					std::swap(array[index - 1], array[index]);
					return true;
				case JsonListEdit::MoveDown:
					if (index + 1 >= array.size())
						return false;
					std::swap(array[index], array[index + 1]);
					return true;
			}
			return false;
		}

		// The new element's position after an edit that adds one.
		size_t AddedIndex(const nlohmann::json& array, JsonListEdit edit, size_t index)
		{
			return edit == JsonListEdit::Add ? array.size() - 1 : index + 1;
		}

		ImGuiID ElementNodeID(size_t index)
		{
			ImGui::PushID(static_cast<int>(index));
			const ImGuiID id = ImGui::GetID("##element");
			ImGui::PopID();
			return id;
		}

		float DragSpeed(const std::string& name)
		{
			if (name.find("Rotation") != std::string::npos || name.find("Angle") != std::string::npos || name == "FOV")
				return 0.5f;
			return 0.02f;
		}

	}

	JsonFieldResult DrawJsonField(const std::string& name, nlohmann::json& value, const std::vector<std::string>* enumOptions)
	{
		ImGui::PushID(name.c_str());
		JsonFieldResult result;

		if (value.is_boolean())
		{
			bool flag = value.get<bool>();
			const bool changed = ImGui::Checkbox(name.c_str(), &flag);
			if (changed)
				value = flag;
			// Checkboxes commit immediately.
			result = { changed, changed };
		}
		else if (value.is_number_integer() || value.is_number_unsigned())
		{
			int64_t number = value.get<int64_t>();
			const int64_t minimum = 0;
			const bool changed = ImGui::DragScalar(name.c_str(), ImGuiDataType_S64, &number, 0.1f, &minimum, nullptr);
			if (changed)
				value = number;
			result = Finish(changed);
		}
		else if (value.is_number())
		{
			float number = value.get<float>();
			const bool changed = ImGui::DragFloat(name.c_str(), &number, DragSpeed(name));
			if (changed)
				value = number;
			result = Finish(changed);
		}
		else if (value.is_string() && enumOptions)
		{
			const std::string current = value.get<std::string>();
			bool changed = false;
			if (ImGui::BeginCombo(name.c_str(), current.c_str()))
			{
				for (const std::string& option : *enumOptions)
				{
					if (ImGui::Selectable(option.c_str(), option == current) && option != current)
					{
						value = option;
						changed = true;
					}
				}
				ImGui::EndCombo();
			}
			result = { changed, changed };
		}
		else if (value.is_string())
		{
			std::string text = value.get<std::string>();
			const bool changed = ImGui::InputText(name.c_str(), &text, ImGuiInputTextFlags_EnterReturnsTrue);
			const bool committed = changed || ImGui::IsItemDeactivatedAfterEdit();
			if (committed)
				value = text;
			// Asset fields accept drops from the content browser.
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(AssetPayloadType))
				{
					value = std::string(static_cast<const char*>(payload->Data));
					result = { true, true };
				}
				ImGui::EndDragDropTarget();
			}
			if (committed)
				result = { true, true };
		}
		else if (value.is_array() && value.size() >= 2 && value.size() <= 4 && std::all_of(value.begin(), value.end(), [](const nlohmann::json& v) { return v.is_number(); }))
		{
			float components[4] = {};
			for (size_t i = 0; i < value.size(); i++)
				components[i] = value[i].get<float>();
			bool changed = false;
			const bool isColor = name.find("Color") != std::string::npos;
			if (isColor && value.size() == 3)
				changed = ImGui::ColorEdit3(name.c_str(), components, ImGuiColorEditFlags_Float);
			else if (isColor)
				changed = ImGui::ColorEdit4(name.c_str(), components, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar);
			else if (value.size() == 2)
				changed = ImGui::DragFloat2(name.c_str(), components, DragSpeed(name));
			else if (value.size() == 3)
				changed = ImGui::DragFloat3(name.c_str(), components, DragSpeed(name));
			else
				changed = ImGui::DragFloat4(name.c_str(), components, DragSpeed(name));
			if (changed)
			{
				for (size_t i = 0; i < value.size(); i++)
					value[i] = components[i];
			}
			result = Finish(changed);
		}
		else if (value.is_array() && value.size() == 3 && std::all_of(value.begin(), value.end(), [](const nlohmann::json& v) { return v.is_boolean(); }))
		{
			// Per-axis flags: one checkbox per axis on a single row.
			constexpr const char* AxisLabels[] = { "X", "Y", "Z" };
			bool changed = false;
			for (size_t i = 0; i < 3; i++)
			{
				bool flag = value[i].get<bool>();
				if (i > 0)
					ImGui::SameLine();
				if (ImGui::Checkbox(AxisLabels[i], &flag))
				{
					value[i] = flag;
					changed = true;
				}
			}
			ImGui::SameLine();
			ImGui::TextUnformatted(name.c_str());
			result = { changed, changed };
		}
		else if (value.is_array() && value.size() == 3 && enumOptions && std::all_of(value.begin(), value.end(), [](const nlohmann::json& v) { return v.is_string(); }))
		{
			// Per-axis enum values: one combo per axis.
			constexpr const char* AxisLabels[] = { "X", "Y", "Z" };
			bool changed = false;
			for (size_t i = 0; i < 3; i++)
			{
				const std::string current = value[i].get<std::string>();
				const std::string label = name + " " + AxisLabels[i];
				if (ImGui::BeginCombo(label.c_str(), current.c_str()))
				{
					for (const std::string& option : *enumOptions)
					{
						if (ImGui::Selectable(option.c_str(), option == current) && option != current)
						{
							value[i] = option;
							changed = true;
						}
					}
					ImGui::EndCombo();
				}
			}
			result = { changed, changed };
		}
		else
		{
			ImGui::TextDisabled("%s: %s", name.c_str(), value.dump().c_str());
		}

		ImGui::PopID();
		return result;
	}

	bool EditJsonList(nlohmann::json& array, JsonListEdit edit, size_t index, const ComponentFieldType& type)
	{
		if (!array.is_array() || !type.Element)
			return false;
		const bool grows = edit == JsonListEdit::Add || edit == JsonListEdit::Duplicate;
		if (grows && array.size() >= type.MaxCount)
			return false;
		if (!ApplyListEdit(array, edit, index, type.Element->Default))
			return false;
		if (grows && !type.Element->UniqueField.empty())
			MakeUnique(array, AddedIndex(array, edit, index), type.Element->UniqueField);
		return true;
	}

	JsonFieldResult DrawJsonValue(const std::string& name, nlohmann::json& value, const ComponentFieldType& type, const std::map<std::string, std::vector<std::string>>& enumOptions)
	{
		if (type.Kind == ComponentFieldKind::List)
			return DrawJsonList(name, value, type);
		const auto options = enumOptions.find(name);
		return DrawJsonField(name, value, options == enumOptions.end() ? nullptr : &options->second);
	}

	JsonFieldResult DrawJsonList(const std::string& name, nlohmann::json& array, const ComponentFieldType& type)
	{
		JsonFieldResult result;
		if (!array.is_array() || !type.Element)
		{
			ImGui::TextDisabled("%s: %s", name.c_str(), array.dump().c_str());
			return result;
		}
		const ComponentObjectSchema& schema = *type.Element;
		ImGui::PushID(name.c_str());
		// Structural edits wait until the loop is done so it never walks an array it changed.
		std::optional<std::pair<JsonListEdit, size_t>> pending;
		const std::string header = name + " (" + std::to_string(array.size()) + ")###list";
		if (ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth))
		{
			const bool full = array.size() >= type.MaxCount;
			for (size_t i = 0; i < array.size(); i++)
			{
				ImGui::PushID(static_cast<int>(i));
				const std::string label = ElementLabel(array[i], i);
				const bool open = ImGui::TreeNodeEx("##element", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap, "%s", label.c_str());
				if (ImGui::BeginPopupContextItem())
				{
					if (ImGui::MenuItem("Duplicate", nullptr, false, !full))
						pending = { JsonListEdit::Duplicate, i };
					if (ImGui::MenuItem("Move Up", nullptr, false, i > 0))
						pending = { JsonListEdit::MoveUp, i };
					if (ImGui::MenuItem("Move Down", nullptr, false, i + 1 < array.size()))
						pending = { JsonListEdit::MoveDown, i };
					if (ImGui::MenuItem("Remove"))
						pending = { JsonListEdit::Remove, i };
					ImGui::EndPopup();
				}
				const float button = ImGui::GetFrameHeight();
				ImGui::SameLine(ImGui::GetContentRegionMax().x - 3.0f * button - 2.0f * ImGui::GetStyle().ItemSpacing.x);
				ImGui::BeginDisabled(i == 0);
				if (ImGui::ArrowButton("up", ImGuiDir_Up))
					pending = { JsonListEdit::MoveUp, i };
				ImGui::EndDisabled();
				ImGui::SameLine();
				ImGui::BeginDisabled(i + 1 >= array.size());
				if (ImGui::ArrowButton("down", ImGuiDir_Down))
					pending = { JsonListEdit::MoveDown, i };
				ImGui::EndDisabled();
				ImGui::SameLine();
				if (ImGui::Button("x", ImVec2(button, button)))
					pending = { JsonListEdit::Remove, i };
				if (open)
				{
					for (const std::string& field : schema.Fields)
					{
						auto value = array[i].find(field);
						if (value == array[i].end())
							continue;
						const auto fieldType = schema.FieldTypes.find(field);
						if (fieldType == schema.FieldTypes.end())
							continue;
						const JsonFieldResult fieldResult = DrawJsonValue(field, *value, fieldType->second, schema.EnumOptions);
						result.Changed |= fieldResult.Changed;
						result.Committed |= fieldResult.Committed;
					}
					ImGui::TreePop();
				}
				ImGui::PopID();
			}
			ImGui::BeginDisabled(full);
			if (ImGui::Button(("Add " + schema.Name).c_str()))
				pending = { JsonListEdit::Add, 0 };
			ImGui::EndDisabled();
			if (full)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("(at most %u)", type.MaxCount);
			}
			ImGui::TreePop();
		}
		if (pending)
		{
			// Element nodes are keyed by position, so their open states move with the elements.
			ImGuiStorage* storage = ImGui::GetStateStorage();
			nlohmann::json open = nlohmann::json::array();
			for (size_t i = 0; i < array.size(); i++)
				open.push_back(storage->GetInt(ElementNodeID(i), 0));
			if (EditJsonList(array, pending->first, pending->second, type))
			{
				ApplyListEdit(open, pending->first, pending->second, 0);
				for (size_t i = 0; i < open.size(); i++)
					storage->SetInt(ElementNodeID(i), open[i].get<int>());
				result = { true, true };
			}
		}
		ImGui::PopID();
		return result;
	}

}
