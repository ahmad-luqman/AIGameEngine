#include "Panels/SceneHierarchyPanel.h"

#include "Basalt/Scene/Scene.h"

#include <imgui.h>

#include <algorithm>

namespace Basalt {

	namespace {

		struct CreateOption
		{
			const char* Label;
			const char* Name;
			nlohmann::json Components;
		};

		const std::vector<CreateOption>& GetCreateOptions()
		{
			static const std::vector<CreateOption> s_Options = {
				{ "Empty", "Entity", nlohmann::json::object() },
				{ "Cube", "Cube", { { "Mesh", { { "Mesh", "builtin://Cube" } } } } },
				{ "Sphere", "Sphere", { { "Mesh", { { "Mesh", "builtin://Sphere" } } } } },
				{ "Plane", "Plane", { { "Mesh", { { "Mesh", "builtin://Plane" } } }, { "Transform", { { "Scale", { 10, 1, 10 } } } } } },
				{ "Cylinder", "Cylinder", { { "Mesh", { { "Mesh", "builtin://Cylinder" } } } } },
				{ "Capsule", "Capsule", { { "Mesh", { { "Mesh", "builtin://Capsule" } } } } },
				{ "Character", "Character", { { "Mesh", { { "Mesh", "builtin://Capsule" } } }, { "CapsuleCollider", nlohmann::json::object() }, { "CharacterController", nlohmann::json::object() } } },
				{ "Camera", "Camera", { { "Camera", nlohmann::json::object() } } },
				{ "Directional Light", "Directional Light", { { "DirectionalLight", nlohmann::json::object() }, { "Transform", { { "Rotation", { -50, 30, 0 } } } } } },
				{ "Point Light", "Point Light", { { "PointLight", nlohmann::json::object() } } },
				{ "Spot Light", "Spot Light", { { "SpotLight", nlohmann::json::object() } } },
				{ "Sky Light", "Sky Light", { { "SkyLight", nlohmann::json::object() } } },
				{ "Audio Source", "Audio Source", { { "AudioSource", nlohmann::json::object() } } },
				{ "Particle Emitter", "Particle Emitter", { { "ParticleEmitter", nlohmann::json::object() } } },
				{ "UI Panel", "Panel", { { "UIRect", nlohmann::json::object() }, { "UIImage", { { "Color", { 0.0, 0.0, 0.0, 0.5 } } } } } },
				{ "UI Text", "Text", { { "UIRect", nlohmann::json::object() }, { "UIText", nlohmann::json::object() } } },
				{ "UI Button", "Button", { { "UIRect", { { "Size", { 240, 64 } } } }, { "UIImage", { { "Color", { 0.2, 0.24, 0.32, 0.95 } } } }, { "UIText", { { "Text", "Button" }, { "HorizontalAlign", "Center" }, { "VerticalAlign", "Middle" } } }, { "UIButton", nlohmann::json::object() } } },
				{ "UI Slider", "Slider", { { "UIRect", { { "Size", { 320, 32 } } } }, { "UISlider", nlohmann::json::object() } } },
				{ "UI Checkbox", "Checkbox", { { "UIRect", { { "Size", { 320, 40 } } } }, { "UIText", { { "Text", "Checkbox" }, { "VerticalAlign", "Middle" }, { "Wrap", false } } }, { "UICheckbox", nlohmann::json::object() } } },
			};
			return s_Options;
		}

	}

	void SceneHierarchyPanel::DrawCreateMenu(Entity parent)
	{
		for (const CreateOption& option : GetCreateOptions())
		{
			if (!ImGui::MenuItem(option.Label))
				continue;
			nlohmann::json params = { { "name", option.Name }, { "components", option.Components } };
			if (parent)
				params["parent"] = static_cast<uint64_t>(parent.GetUUID());
			if (auto result = m_Context.Execute("entity.create", params))
			{
				m_Context.Session.SetSelection((*result)["id"].get<uint64_t>());
				m_Context.CommitHistory();
			}
		}
	}

	void SceneHierarchyPanel::DrawEntityNode(Entity entity)
	{
		const UUID uuid = entity.GetUUID();
		m_Visible.push_back(uuid);
		const std::vector<Entity> children = entity.GetChildren();

		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnDoubleClick;
		if (m_Context.Session.IsSelected(uuid))
			flags |= ImGuiTreeNodeFlags_Selected;
		if (children.empty())
			flags |= ImGuiTreeNodeFlags_Leaf;

		const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint64_t>(uuid))), flags, "%s", entity.GetName().c_str());
		if ((ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) || (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !m_Context.Session.IsSelected(uuid)))
		{
			const auto& io = ImGui::GetIO();
			const auto anchor = std::find(m_PreviousVisible.begin(), m_PreviousVisible.end(), m_Anchor);
			const auto clicked = std::find(m_PreviousVisible.begin(), m_PreviousVisible.end(), uuid);
			if (io.KeyShift && anchor != m_PreviousVisible.end() && clicked != m_PreviousVisible.end())
			{
				auto ids = nlohmann::json::array();
				for (auto it = std::min(anchor, clicked); it <= std::max(anchor, clicked); ++it)
					if (*it != uuid)
						ids.push_back(static_cast<uint64_t>(*it));
				ids.push_back(static_cast<uint64_t>(uuid));
				m_Context.Execute("editor.select", { { "entities", ids }, { "mode", io.KeyCtrl || io.KeySuper ? "add" : "replace" } });
			}
			else
			{
				m_Context.Execute("editor.select", { { "entity", static_cast<uint64_t>(uuid) }, { "mode", io.KeyCtrl || io.KeySuper ? "toggle" : "replace" } });
				m_Anchor = uuid;
			}
		}

		if (ImGui::BeginDragDropSource())
		{
			const uint64_t id = uuid;
			ImGui::SetDragDropPayload(EntityPayloadType, &id, sizeof(id));
			ImGui::TextUnformatted(entity.GetName().c_str());
			ImGui::EndDragDropSource();
		}
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(EntityPayloadType))
			{
				const uint64_t child = *static_cast<const uint64_t*>(payload->Data);
				if (m_Context.Execute("entity.set_parent", { { "entity", child }, { "parent", static_cast<uint64_t>(uuid) } }))
					m_Context.CommitHistory();
			}
			ImGui::EndDragDropTarget();
		}

		if (ImGui::BeginPopupContextItem())
		{
			if (ImGui::BeginMenu("Create Child"))
			{
				DrawCreateMenu(entity);
				ImGui::EndMenu();
			}
			if (ImGui::MenuItem("Duplicate Selection", "Ctrl+D"))
			{
				m_Context.Execute("editor.duplicate");
				m_Context.CommitHistory();
			}
			if (ImGui::MenuItem("Unparent", nullptr, false, static_cast<bool>(entity.GetParent())))
			{
				if (m_Context.Execute("entity.set_parent", { { "entity", static_cast<uint64_t>(uuid) }, { "parent", nullptr } }))
					m_Context.CommitHistory();
			}
			if (ImGui::MenuItem("Delete Selection", "Del"))
				m_PendingDelete = true;
			ImGui::EndPopup();
		}

		if (open)
		{
			for (Entity child : children)
				DrawEntityNode(child);
			ImGui::TreePop();
		}
	}

	void SceneHierarchyPanel::OnImGuiRender()
	{
		ImGui::Begin("Hierarchy");
		m_PreviousVisible = std::move(m_Visible);
		m_Visible.clear();
		Scene* scene = m_Context.Session.GetScene().get();
		if (scene)
		{
			for (Entity root : scene->GetRootEntities())
				DrawEntityNode(root);

			// Dropping onto empty space makes an entity a root.
			const ImVec2 available = ImGui::GetContentRegionAvail();
			ImGui::Dummy(ImVec2(std::max(available.x, 1.0f), std::max(available.y, 40.0f)));
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(EntityPayloadType))
				{
					const uint64_t child = *static_cast<const uint64_t*>(payload->Data);
					if (m_Context.Execute("entity.set_parent", { { "entity", child }, { "parent", nullptr } }))
						m_Context.CommitHistory();
				}
				ImGui::EndDragDropTarget();
			}
			if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
				m_Context.Execute("editor.select");

			if (ImGui::BeginPopupContextWindow("HierarchyContext", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
			{
				DrawCreateMenu({});
				ImGui::EndPopup();
			}
		}

		if (m_PendingDelete)
		{
			m_Context.Execute("editor.delete");
			m_Context.CommitHistory();
			m_PendingDelete = false;
		}
		ImGui::End();
	}

}
