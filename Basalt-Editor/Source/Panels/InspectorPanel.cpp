#include "Panels/InspectorPanel.h"

#include "Panels/JsonWidgets.h"

#include "Basalt/Math/Math.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Scene/ComponentRegistry.h"
#include "Basalt/Scene/JointFields.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/Scripting/ScriptEngine.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <functional>

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

	namespace {

		// Whether the entity gets a physics body when play starts (a RigidBody alone is not enough).
		bool HasPhysicsBody(Entity entity)
		{
			return entity.HasComponent<RigidBodyComponent>() &&
				   (entity.HasComponent<BoxColliderComponent>() || entity.HasComponent<SphereColliderComponent>() || entity.HasComponent<CapsuleColliderComponent>());
		}

		// Which entities a joint's reference field may name: entities with a physics body, but never the body
		// the joint moves on both sides. BodyEntity's 0 (Self) already names the holder; a holder that is not
		// the body may itself be the connected body.
		std::function<bool(Entity)> JointReferenceFilter(Entity holder, const JointComponent& joint, const std::string& field)
		{
			if (field == "BodyEntity")
			{
				return [holder, connected = joint.ConnectedEntity](Entity candidate) {
					return candidate != holder && candidate.GetUUID() != connected && HasPhysicsBody(candidate);
				};
			}
			return [body = joint.BodyEntity != 0 ? joint.BodyEntity : holder.GetUUID()](Entity candidate) {
				return candidate.GetUUID() != body && HasPhysicsBody(candidate);
			};
		}

	}

	void InspectorPanel::DrawEntityReference(Entity entity, const std::string& component, const std::string& field, uint64_t current, const std::function<bool(Entity)>& filter, const char* noneLabel)
	{
		Scene& scene = *entity.GetScene();
		std::string preview = noneLabel;
		if (current != 0)
		{
			const Entity target = scene.GetEntityByUUID(current);
			preview = target ? target.GetName() : "<missing " + std::to_string(current) + ">";
		}

		uint64_t selected = current;
		if (ImGui::BeginCombo(field.c_str(), preview.c_str()))
		{
			if (ImGui::Selectable(noneLabel, current == 0))
				selected = 0;
			for (Entity candidate : scene.GetAllEntitiesOrdered())
			{
				if (!filter(candidate))
					continue;
				const uint64_t id = candidate.GetUUID();
				ImGui::PushID(static_cast<int>(id ^ (id >> 32)));
				if (ImGui::Selectable(candidate.GetName().c_str(), id == current))
					selected = id;
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}

		if (selected != current)
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
				// Joint fields the joint does not use are hidden, except ones set away from their defaults: those
				// stay visible and marked, so the warning they cause can be fixed here.
				const bool isJoint = info.Name == "Joint";
				const JointComponent joint = isJoint ? entity.GetComponent<JointComponent>() : JointComponent{};
				const std::vector<std::string> ignoredJointFields = isJoint ? GetIgnoredJointFields(joint) : std::vector<std::string>{};
				for (const std::string& field : info.Fields)
				{
					if (!data.contains(field))
						continue;
					if (info.Name == "Script" && field == "Properties")
						continue;
					const bool ignored = std::ranges::find(ignoredJointFields, field) != ignoredJointFields.end();
					if (isJoint && !ignored && !JointFieldApplies(joint, field))
						continue;
					if (std::find(info.EntityFields.begin(), info.EntityFields.end(), field) != info.EntityFields.end())
					{
						// 0 is the entity itself for a joint's BodyEntity and the world for its ConnectedEntity.
						const char* noneLabel = "None";
						std::function<bool(Entity)> filter = [entity](Entity candidate) { return candidate != entity; };
						if (isJoint)
						{
							noneLabel = field == "BodyEntity" ? "Self" : "World";
							filter = JointReferenceFilter(entity, joint, field);
						}
						DrawEntityReference(entity, info.Name, field, data[field].get<uint64_t>(), filter, noneLabel);
						continue;
					}
					auto options = info.EnumOptions.find(field);
					const std::vector<std::string>* enumOptions = options != info.EnumOptions.end() ? &options->second : nullptr;
					// Layer names come from the project, not the component, so the combo is built here.
					std::vector<std::string> layerNames;
					bool unknownLayer = false;
					if (info.Name == "RigidBody" && field == "Layer")
					{
						const Ref<Project>& project = Project::GetActive();
						layerNames = project ? project->GetConfig().Physics.GetNames() : PhysicsLayers().GetNames();
						const std::string current = data[field].get<std::string>();
						if (std::ranges::find(layerNames, current) == layerNames.end())
						{
							// Keep an unknown name selectable so it stays visible until it is fixed.
							layerNames.push_back(current);
							unknownLayer = true;
						}
						enumOptions = &layerNames;
					}
					const JsonFieldResult result = DrawJsonField(field, data[field], enumOptions);
					if (unknownLayer)
						ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "Unknown layer (plays as Default)");
					if (ignored)
					{
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.2f, 1.0f));
						ImGui::TextWrapped("%s is ignored by this joint", field.c_str());
						ImGui::PopStyleColor();
					}
					if (result.Changed)
						m_Context.Execute("component.set", { { "entity", id }, { "component", info.Name }, { "data", { { field, data[field] } } } });
					if (result.Committed)
						m_Context.CommitHistory();
				}
				if (info.Name == "Script")
					DrawScriptProperties(entity, data);
				if (info.Name == "Joint")
				{
					// The joint moves BodyEntity's body, or this entity's when BodyEntity is Self.
					const Entity body = joint.BodyEntity != 0 ? scene->GetEntityByUUID(joint.BodyEntity) : entity;
					if (!body)
						ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "BodyEntity is missing; the joint is not built.");
					else if (!HasPhysicsBody(body))
						ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), joint.BodyEntity != 0 ? "BodyEntity needs a RigidBody and a collider for the joint to work." : "This entity needs a RigidBody and a collider for the joint to work.");
					else
					{
						// Physics builds the joint frame from the body's transform and skips one it cannot decompose.
						glm::vec3 translation;
						glm::quat rotation;
						glm::vec3 scale;
						if (!Math::DecomposeTransform(scene->GetWorldTransform(body), translation, rotation, scale))
							ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "The body's transform is degenerate (e.g. a zero scale); the joint is not built.");
					}
				}
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
