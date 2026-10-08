#include "Panels/InspectorPanel.h"

#include "Panels/JsonWidgets.h"

#include "Basalt/Project/Project.h"
#include "Basalt/Scene/ComponentRegistry.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/Scripting/ScriptEngine.h"

#include <imgui.h>
#include <imgui_stdlib.h>

namespace Basalt {

	const nlohmann::json* InspectorPanel::GetScriptDefaults(const std::string& scriptPath)
	{
		std::error_code error;
		const auto writeTime = std::filesystem::last_write_time(Project::ResolvePath(scriptPath), error);
		auto it = m_ScriptCache.find(scriptPath);
		if (it == m_ScriptCache.end() || it->second.WriteTime != writeTime)
		{
			ScriptCacheEntry entry;
			entry.WriteTime = writeTime;
			if (auto properties = ScriptEngine::LoadScriptProperties(scriptPath, entry.Error))
				entry.Properties = *properties;
			it = m_ScriptCache.insert_or_assign(scriptPath, std::move(entry)).first;
		}
		if (!it->second.Error.empty())
		{
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", it->second.Error.c_str());
			return nullptr;
		}
		return &it->second.Properties;
	}

	void InspectorPanel::DrawScriptProperties(Entity entity, const nlohmann::json& component)
	{
		const std::string script = component.value("Script", std::string());
		if (script.empty())
			return;
		const nlohmann::json* defaults = GetScriptDefaults(script);
		if (!defaults || defaults->empty())
			return;

		ImGui::SeparatorText("Properties");
		const nlohmann::json overrides = component.value("Properties", nlohmann::json::object());
		for (auto it = defaults->begin(); it != defaults->end(); ++it)
		{
			const bool overridden = overrides.contains(it.key());
			nlohmann::json value = overridden ? overrides[it.key()] : it.value();
			if (overridden)
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 1.0f, 1.0f));
			const JsonFieldResult result = DrawJsonField(it.key(), value);
			if (overridden)
				ImGui::PopStyleColor();

			nlohmann::json newOverrides = overrides;
			bool apply = result.Changed;
			if (result.Changed)
				newOverrides[it.key()] = value;
			if (overridden && ImGui::BeginPopupContextItem())
			{
				if (ImGui::MenuItem("Reset to default"))
				{
					newOverrides.erase(it.key());
					apply = true;
				}
				ImGui::EndPopup();
			}
			if (apply)
			{
				m_Context.Execute("component.set", { { "entity", static_cast<uint64_t>(entity.GetUUID()) }, { "component", "Script" }, { "data", { { "Properties", newOverrides } } } });
				if (result.Committed || !result.Changed)
					m_Context.CommitHistory();
			}
			else if (result.Committed)
			{
				m_Context.CommitHistory();
			}
		}
	}

	void InspectorPanel::DrawEntityReference(Entity entity, const std::string& component, const std::string& field, uint64_t current, const char* noneLabel)
	{
		Scene& scene = *entity.GetScene();
		const Entity target = current != 0 ? scene.GetEntityByUUID(current) : Entity{};
		const std::string preview = current == 0 ? noneLabel : target ? target.GetName()
																	  : "<missing " + std::to_string(current) + ">";

		uint64_t selected = current;
		bool changed = false;
		if (ImGui::BeginCombo(field.c_str(), preview.c_str()))
		{
			if (ImGui::Selectable(noneLabel, current == 0))
			{
				selected = 0;
				changed = true;
			}
			// Only entities with a rigid body can be joined to; the entity itself is excluded.
			for (Entity candidate : scene.GetAllEntitiesOrdered())
			{
				if (candidate == entity || !candidate.HasComponent<RigidBodyComponent>())
					continue;
				const uint64_t id = candidate.GetUUID();
				ImGui::PushID(static_cast<int>(id ^ (id >> 32)));
				if (ImGui::Selectable(candidate.GetName().c_str(), id == current))
				{
					selected = id;
					changed = true;
				}
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}

		if (changed && selected != current)
		{
			if (m_Context.Execute("component.set", { { "entity", static_cast<uint64_t>(entity.GetUUID()) }, { "component", component }, { "data", { { field, selected } } } }))
				m_Context.CommitHistory();
		}
	}

	void InspectorPanel::OnImGuiRender()
	{
		ImGui::Begin("Inspector");
		Scene* scene = m_Context.Session.GetScene().get();
		Entity entity = scene ? scene->GetEntityByUUID(m_Context.Session.GetSelection()) : Entity{};
		if (!entity)
		{
			ImGui::TextDisabled("Select an entity to edit its components.");
			ImGui::End();
			return;
		}

		const uint64_t id = entity.GetUUID();
		std::string name = entity.GetName();
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::InputText("##Name", &name, ImGuiInputTextFlags_EnterReturnsTrue) && !name.empty())
		{
			if (m_Context.Execute("entity.rename", { { "entity", id }, { "name", name } }))
				m_Context.CommitHistory();
		}
		ImGui::TextDisabled("ID %llu", static_cast<unsigned long long>(id));

		std::string componentToRemove;
		for (const ComponentInfo& info : ComponentRegistry::GetAll())
		{
			if (info.Name == "Tag" || !info.Has(entity))
				continue;

			ImGui::PushID(info.Name.c_str());
			const bool open = ImGui::CollapsingHeader(info.Name.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
			if (!info.IsCore)
			{
				ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
				if (ImGui::SmallButton("x"))
					componentToRemove = info.Name;
			}

			if (open)
			{
				nlohmann::json data = info.Serialize(entity);
				for (const std::string& field : info.Fields)
				{
					if (!data.contains(field))
						continue;
					if (info.Name == "Script" && field == "Properties")
						continue;
					if (info.Name == "Joint" && field == "ConnectedEntity")
					{
						DrawEntityReference(entity, info.Name, field, data[field].get<uint64_t>(), "World");
						continue;
					}
					auto options = info.EnumOptions.find(field);
					const JsonFieldResult result = DrawJsonField(field, data[field], options != info.EnumOptions.end() ? &options->second : nullptr);
					if (result.Changed)
						m_Context.Execute("component.set", { { "entity", id }, { "component", info.Name }, { "data", { { field, data[field] } } } });
					if (result.Committed)
						m_Context.CommitHistory();
				}
				if (info.Name == "Script")
					DrawScriptProperties(entity, data);
			}
			ImGui::PopID();
		}

		if (!componentToRemove.empty())
		{
			if (m_Context.Execute("component.remove", { { "entity", id }, { "component", componentToRemove } }))
				m_Context.CommitHistory();
		}

		ImGui::Separator();
		if (ImGui::Button("Add Component", ImVec2(-1.0f, 0.0f)))
			ImGui::OpenPopup("AddComponent");
		if (ImGui::BeginPopup("AddComponent"))
		{
			for (const ComponentInfo& info : ComponentRegistry::GetAll())
			{
				if (info.IsCore || info.Has(entity))
					continue;
				if (ImGui::MenuItem(info.Name.c_str()))
				{
					if (m_Context.Execute("component.set", { { "entity", id }, { "component", info.Name }, { "data", nlohmann::json::object() } }))
						m_Context.CommitHistory();
				}
			}
			ImGui::EndPopup();
		}

		ImGui::End();
	}

}
