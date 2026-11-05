#include "Panels/PropertyAnimationPanel.h"

#include "Panels/JsonWidgets.h"

#include "Basalt/Animation/PropertyAnimation.h"
#include "Basalt/Automation/EditorCommandSchemas.h"
#include "Basalt/Scene/Entity.h"

#include <imgui.h>

#include <algorithm>
#include <string>

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
				// A deleted target, or selecting the target itself, falls back to the selected entity.
				if (const Entity current = PropertyAnimation::ResolveTarget(entity, m_Target); !current || current == entity)
					m_Target = 0;
				if (const auto picked = DrawEntityPicker(*scene, "Target", m_Target, [&](Entity candidate) { return candidate != entity; }, "Self"))
				{
					m_Target = *picked;
					m_Field.clear();
				}
				const Entity target = PropertyAnimation::ResolveTarget(entity, m_Target);
				if (ImGui::BeginCombo("Component", m_Component.c_str()))
				{
					for (const auto& info : ComponentRegistry::GetAll())
						if (!PropertyAnimation::ListFields(target, info.Name).empty() && ImGui::Selectable(info.Name.c_str(), info.Name == m_Component))
						{
							m_Component = info.Name;
							m_Field.clear();
						}
					ImGui::EndCombo();
				}
				if (ImGui::BeginCombo("Field", m_Field.c_str()))
				{
					// Nested fields are listed by their element's name where it has one ("Layers[Upper].Weight").
					for (const std::string& field : PropertyAnimation::ListFields(target, m_Component))
						if (ImGui::Selectable(field.c_str(), field == m_Field))
							m_Field = field;
					ImGui::EndCombo();
				}
				const auto id = static_cast<uint64_t>(entity.GetUUID());
				bool scrub = ImGui::SliderFloat("Time", &m_Time, 0, 1);
				const bool committed = ImGui::IsItemDeactivatedAfterEdit();
				bool sampleButton = false;
				if (ImGui::Button("Capture Key"))
				{
					nlohmann::json key = { { "entity", id }, { "component", m_Component }, { "field", m_Field }, { "time", m_Time } };
					if (m_Target != 0)
						key["target"] = m_Target;
					if (m_Context.Execute("animation.key", key))
						m_Context.CommitHistory();
				}
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
					ImGui::TextWrapped("Capture fields above. Edit duration, interpolation, key times and values below. Values use X for scalars, integers and booleans (0 or 1) and the first N lanes for vectors; strings and enums use Text and asset references use Asset. Integer, boolean, string, enum and asset tracks always step. Target is the entity a track animates. Scrubbing changes the scene and supports Undo.");
					for (const auto& field : info->Fields)
					{
						const auto edit = DrawJsonValue(field, data[field], info->FieldTypes.at(field), info->EnumOptions, *scene);
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
