#include "Panels/JsonWidgets.h"

#include "EditorContext.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>

namespace Basalt {

	namespace {

		JsonFieldResult Finish(bool changed)
		{
			return { changed, ImGui::IsItemDeactivatedAfterEdit() };
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
		else if (value.is_array() && (value.size() == 3 || value.size() == 4) && std::all_of(value.begin(), value.end(), [](const nlohmann::json& v) { return v.is_number(); }))
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

}
