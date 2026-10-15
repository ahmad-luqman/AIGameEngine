#include "Basalt/Automation/EditorSelection.h"

#include "Basalt/Animation/AnimatedModel.h"
#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Automation/AutomationSession.h"
#include "Basalt/Automation/CommandRegistry.h"
#include "Basalt/Automation/CommandSchema.h"
#include "Basalt/Automation/EditorCommandSchemas.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Scene/ComponentRegistry.h"
#include "Basalt/Scene/Scene.h"

#include <algorithm>
#include <array>

namespace Basalt {

	namespace {
		void RequireEditableSelection(AutomationSession& session)
		{
			if (session.IsPlaying())
				throw CommandError("selection edits are only available in edit mode");
			session.PruneSelection();
			if (session.GetSelections().empty())
				throw CommandError("select at least one entity");
		}

		nlohmann::json Vec3(const glm::vec3& value)
		{
			return { value.x, value.y, value.z };
		}
	}

	std::vector<UUID> SelectionRoots(Scene& scene, const std::vector<UUID>& selection)
	{
		std::vector<UUID> roots;
		for (UUID id : selection)
		{
			Entity entity = scene.GetEntityByUUID(id);
			if (!entity)
				continue;
			bool inherited = false;
			for (Entity parent = entity.GetParent(); parent; parent = parent.GetParent())
				if (std::find(selection.begin(), selection.end(), parent.GetUUID()) != selection.end())
				{
					inherited = true;
					break;
				}
			if (!inherited && std::find(roots.begin(), roots.end(), id) == roots.end())
				roots.push_back(id);
		}
		return roots;
	}

	glm::mat4 SelectionPivot(Scene& scene, const std::vector<UUID>& selection, bool local)
	{
		glm::dvec3 center(0);
		size_t count = 0;
		glm::quat orientation(1, 0, 0, 0);
		for (UUID id : selection)
		{
			Entity entity = scene.GetEntityByUUID(id);
			if (!entity)
				continue;
			const glm::mat4 world = scene.GetWorldTransform(entity);
			if (!Math::IsFinite(world))
				continue;
			center += glm::dvec3(world[3]);
			++count;
			glm::vec3 translation, scale;
			glm::quat rotation;
			if (local && Math::IsFinite(world) && Math::DecomposeTransform(world, translation, rotation, scale))
				orientation = rotation;
		}
		return Math::ComposeTransform(glm::vec3(count ? center / static_cast<double>(count) : center), orientation, glm::vec3(1));
	}

	std::vector<UUID> SelectViewportBox(Scene& scene, const glm::mat4& viewProjection, const glm::vec2& size, const glm::vec2& start, const glm::vec2& end)
	{
		std::vector<UUID> result;
		if (size.x <= 0 || size.y <= 0 || !Math::IsFinite(viewProjection))
			return result;
		const glm::vec2 low = glm::min(start, end) / size;
		const glm::vec2 high = glm::max(start, end) / size;
		for (Entity entity : scene.GetAllEntitiesOrdered())
		{
			// UI rects are laid out in canvas space and are selected in the hierarchy.
			if (entity.HasComponent<UIRectComponent>())
				continue;
			const glm::mat4 world = scene.GetWorldTransform(entity);
			if (!Math::IsFinite(world))
				continue;
			AABB bounds;
			if (const auto* mesh = entity.TryGetComponent<MeshComponent>())
			{
				if (auto source = AssetManager::GetMesh(mesh->Mesh); source && mesh->MeshIndex < source->Meshes.size())
					bounds = source->Meshes[mesh->MeshIndex].Bounds.Transformed(world);
			}
			if (entity.HasComponent<AnimatorComponent>())
				bounds.Expand(AnimatedModelBounds(entity));
			if (!bounds.IsValid())
			{
				bounds.Expand(glm::vec3(world[3]) - glm::vec3(.25f));
				bounds.Expand(glm::vec3(world[3]) + glm::vec3(.25f));
			}
			std::array<bool, 6> outside{ true, true, true, true, true, true };
			for (int corner = 0; corner < 8; ++corner)
			{
				const glm::vec3 point((corner & 1) ? bounds.Max.x : bounds.Min.x, (corner & 2) ? bounds.Max.y : bounds.Min.y, (corner & 4) ? bounds.Max.z : bounds.Min.z);
				const glm::vec4 clip = viewProjection * glm::vec4(point, 1);
				outside[0] = outside[0] && clip.x < (2 * low.x - 1) * clip.w;
				outside[1] = outside[1] && clip.x > (2 * high.x - 1) * clip.w;
				outside[2] = outside[2] && clip.y < (1 - 2 * high.y) * clip.w;
				outside[3] = outside[3] && clip.y > (1 - 2 * low.y) * clip.w;
				outside[4] = outside[4] && clip.z < 0;
				outside[5] = outside[5] && clip.z > clip.w;
			}
			if (std::none_of(outside.begin(), outside.end(), [](bool value) { return value; }))
				result.push_back(entity.GetUUID());
		}
		return result;
	}

	nlohmann::json SelectionResult(AutomationSession& session)
	{
		session.PruneSelection();
		auto entities = nlohmann::json::array();
		for (UUID id : session.GetSelections())
			entities.push_back(static_cast<uint64_t>(id));
		return { { "selected", static_cast<uint64_t>(session.GetSelection()) }, { "entities", entities } };
	}

	void RegisterEditorSelectionCommands(CommandRegistry& registry)
	{
		using json = nlohmann::json;
		registry.Register({ "editor.select", "Replaces, adds or toggles an ordered selection. Last entity is active; null or no references clears it.", EditorCommandSchemas::Select(), [&registry](AutomationSession& session, const json& params) {
							   if (params.contains("entity") && params.contains("entities"))
								   throw CommandError("use entity or entities, not both");
							   auto references = params.value("entities", json::array());
							   if (params.contains("entity") && !params["entity"].is_null())
								   references.push_back(params["entity"]);
							   std::vector<UUID> resolved;
							   for (const auto& reference : references)
							   {
								   const UUID id = registry.Execute(session, "entity.get", { { "entity", reference } })["ID"].get<uint64_t>();
								   if (std::find(resolved.begin(), resolved.end(), id) == resolved.end())
									   resolved.push_back(id);
							   }
							   const std::string mode = params.value("mode", std::string("replace"));
							   std::vector<UUID> selection = mode == "replace" ? std::vector<UUID>{} : session.GetSelections();
							   for (UUID id : resolved)
							   {
								   auto it = std::find(selection.begin(), selection.end(), id);
								   if (mode == "toggle" && it != selection.end())
									   selection.erase(it);
								   else if (it == selection.end())
									   selection.push_back(id);
							   }
							   session.SetSelections(selection);
							   return SelectionResult(session);
						   } });

		registry.Register({ "editor.transform", "Applies a column-major world delta matrix to selected roots atomically. Selected descendants inherit once; shear is rejected.", EditorCommandSchemas::Transform(), [&registry](AutomationSession& session, const json& params) {
							   RequireEditableSelection(session);
							   glm::mat4 delta(1);
							   for (int c = 0; c < 4; ++c)
								   for (int r = 0; r < 4; ++r)
									   if (!JsonToFloat(params["delta"][c * 4 + r], delta[c][r]))
										   throw CommandError("delta must contain finite floats");
							   Scene& scene = *session.GetScene();
							   std::vector<std::pair<UUID, json>> patches;
							   for (UUID id : session.GetSelections())
								   if (scene.GetEntityByUUID(id).HasComponent<UIRectComponent>())
									   throw CommandError("UI rects use canvas coordinates; edit UIRect in the inspector");
							   for (UUID id : SelectionRoots(scene, session.GetSelections()))
							   {
								   const Entity entity = scene.GetEntityByUUID(id);
								   glm::mat4 local = delta * scene.GetWorldTransform(entity);
								   if (const Entity parent = entity.GetParent())
								   {
									   const glm::mat4 inverse = glm::inverse(scene.GetWorldTransform(parent));
									   if (!Math::IsFinite(inverse))
										   throw CommandError("selection has a singular parent transform");
									   local = inverse * local;
								   }
								   glm::vec3 translation, scale;
								   glm::quat rotation;
								   if (!Math::IsFinite(local) || !Math::DecomposeAffineTransform(local, translation, rotation, scale))
									   throw CommandError("selection transform contains shear or non-finite values");
								   patches.emplace_back(id, json{ { "Translation", Vec3(translation) }, { "Rotation", Vec3(glm::degrees(Math::EulerFromQuat(rotation))) }, { "Scale", Vec3(scale) } });
							   }
							   for (const auto& [id, data] : patches)
								   registry.Execute(session, "component.set", { { "entity", static_cast<uint64_t>(id) }, { "component", "Transform" }, { "data", data } });
							   return SelectionResult(session);
						   } });

		registry.Register({ "editor.component_set", "Sets shared component fields on every selected entity atomically; all must already have the component.", EditorCommandSchemas::ComponentSet(), [&registry](AutomationSession& session, const json& params) {
							   RequireEditableSelection(session);
							   const std::string name = params["component"].get<std::string>();
							   const ComponentInfo* info = ComponentRegistry::Find(name);
							   if (!info)
								   throw CommandError("unknown component '" + name + "'");
							   json data = params["data"];
							   for (const auto& field : info->EntityFields)
								   if (data.contains(field) && data[field].is_string())
									   data[field] = registry.Execute(session, "entity.get", { { "entity", data[field] } })["ID"];
							   // Validate each entity's merged value without mutating the live scene or copying unrelated components.
							   Scene scratch;
							   Entity probe = scratch.CreateEntity("Validation");
							   std::string error;
							   for (UUID id : session.GetSelections())
							   {
								   Entity entity = session.GetScene()->GetEntityByUUID(id);
								   if (!info->Has(entity))
									   throw CommandError("every selected entity must have " + name);
								   if (info->Has(probe) && !info->IsCore)
									   info->Remove(probe);
								   if (!info->Deserialize(probe, info->Serialize(entity), error) || !info->Deserialize(probe, data, error))
									   throw CommandError(error);
							   }
							   for (UUID id : session.GetSelections())
								   registry.Execute(session, "component.set", { { "entity", static_cast<uint64_t>(id) }, { "component", name }, { "data", data } });
							   return SelectionResult(session);
						   } });

		for (bool duplicate : { false, true })
			registry.Register({ duplicate ? "editor.duplicate" : "editor.delete", duplicate ? "Duplicates selected root trees as one edit and selects their copies." : "Deletes selected root trees as one edit.", CommandSchema::Object(), [&registry, duplicate](AutomationSession& session, const json&) {
								   RequireEditableSelection(session);
								   const auto roots = SelectionRoots(*session.GetScene(), session.GetSelections());
								   std::vector<UUID> copies;
								   if (duplicate)
								   {
									   std::vector<Entity> entities;
									   entities.reserve(roots.size());
									   for (UUID id : roots)
										   entities.push_back(session.GetScene()->GetEntityByUUID(id));
									   for (Entity copy : session.GetScene()->DuplicateEntities(entities))
										   copies.push_back(copy.GetUUID());
									   session.MarkDirty();
								   }
								   else
									   for (UUID id : roots)
										   registry.Execute(session, "entity.destroy", { { "entity", static_cast<uint64_t>(id) } });
								   session.SetSelections(copies);
								   return SelectionResult(session);
							   } });
	}

}
