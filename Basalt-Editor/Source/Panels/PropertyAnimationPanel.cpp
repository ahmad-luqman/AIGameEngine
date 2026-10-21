#include "Panels/PropertyAnimationPanel.h"

#include "Panels/JsonWidgets.h"

#include "Basalt/Animation/PropertyAnimation.h"
#include "Basalt/Automation/EditorCommandSchemas.h"
#include "Basalt/Scene/Entity.h"

#include <imgui.h>

#include <algorithm>

namespace Basalt {
	void PropertyAnimationPanel::OnImGuiRender()
	{
		if (ImGui::Begin(EditorCommandSchemas::PropertyAnimationTitle))
		{
			auto scene = m_Context.Session.GetScene();
			Entity entity = scene ? scene->GetEntityByUUID(m_Context.Session.GetSelection()) : Entity{};
			if (entity)
			{
				ImGui::TextUnformatted(entity.GetName().c_str());
				if (ImGui::BeginCombo("Component", m_Component.c_str()))
				{
					for (const auto& info : ComponentRegistry::GetAll())
						if (info.Has(entity) && std::ranges::any_of(info.Fields, [&](const auto& field) { return PropertyAnimation::FindField(info.Name, field) != nullptr; }) && ImGui::Selectable(info.Name.c_str(), info.Name == m_Component))
						{
							m_Component = info.Name;
							m_Field.clear();
						}
					ImGui::EndCombo();
				}
				if (ImGui::BeginCombo("Field", m_Field.c_str()))
				{
					if (const auto* info = ComponentRegistry::Find(m_Component))
						for (const auto& field : info->Fields)
							if (PropertyAnimation::FindField(m_Component, field) && ImGui::Selectable(field.c_str(), field == m_Field))
								m_Field = field;
					ImGui::EndCombo();
				}
				const auto id = static_cast<uint64_t>(entity.GetUUID());
				bool scrub = ImGui::SliderFloat("Time", &m_Time, 0, 1);
				const bool committed = ImGui::IsItemDeactivatedAfterEdit();
				bool sampleButton = false;
				if (ImGui::Button("Capture Key"))
					if (m_Context.Execute("animation.key", { { "entity", id }, { "component", m_Component }, { "field", m_Field }, { "time", m_Time } }))
						m_Context.CommitHistory();
				ImGui::SameLine();
				if (ImGui::Button("Sample"))
				{
					scrub = true;
					sampleButton = true;
				}
				if (entity.HasComponent<PropertyAnimationComponent>())
				{
					m_Context.EditField("animation.sample", { { "entity", id }, { "time", m_Time } }, scrub, committed || sampleButton);
					if (scene->GetPropertyAnimationSystem())
						for (const char* action : { "Play", "Pause", "Resume", "Stop" })
						{
							if (ImGui::Button(action))
								m_Context.Execute("animation.control", { { "entity", id }, { "action", action } });
							ImGui::SameLine();
						}
					ImGui::NewLine();
					if (const auto* system = scene->GetPropertyAnimationSystem())
					{
						const auto status = system->Status(entity);
						ImGui::Text("Playback: %s, time %.3f", status["failed"].get<bool>() ? "failed" : status["playing"].get<bool>() ? "playing"
																																	   : "paused/stopped",
									status["time"].get<double>());
						if (status["failed"].get<bool>())
							ImGui::TextWrapped("%s", status["error"].get_ref<const std::string&>().c_str());
					}
					const auto* info = ComponentRegistry::Find("PropertyAnimation");
					auto data = info->Serialize(entity);
					ImGui::TextWrapped("Capture numeric fields above. Edit duration, interpolation, key times and values below. Values use X for scalars and the first N lanes for vectors. Scrubbing changes the scene and supports Undo.");
					for (const auto& field : info->Fields)
					{
						const auto edit = DrawJsonValue(field, data[field], info->FieldTypes.at(field), info->EnumOptions);
						m_Context.EditField("component.set", { { "entity", id }, { "component", "PropertyAnimation" }, { "data", { { field, data[field] } } } }, edit.Changed, edit.Committed);
					}
				}
			}
			else
				ImGui::TextUnformatted("Select an entity to author property animation.");
		}
		ImGui::End();
	}
}
