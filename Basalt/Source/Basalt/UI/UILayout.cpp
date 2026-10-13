#include "Basalt/UI/UILayout.h"

#include "Basalt/Core/Log.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/UI/UIRules.h"

#include <algorithm>
#include <mutex>
#include <unordered_set>

namespace Basalt {

	namespace {

		bool HasUIComponent(Entity entity)
		{
			return entity.HasComponent<UIRectComponent>() || entity.HasComponent<UIImageComponent>() || entity.HasComponent<UITextComponent>();
		}

		// The registry rejects invalid values, so only C++ code writing a component directly produces them.
		// The layout draws repaired values and says so, once per entity, component and problem.
		template<typename T>
		T Repaired(Entity entity, const T& component, T (*sanitize)(T), std::string (*validate)(const T&), const char* name)
		{
			T repaired = sanitize(component);
			if (repaired == component)
				return repaired;
			static std::mutex s_Mutex;
			static std::unordered_set<std::string> s_Reported;
			const std::string problem = validate(component);
			std::scoped_lock lock(s_Mutex);
			// Bounded, so a long session with many broken entities cannot grow it without end.
			if (s_Reported.size() < 1024 && s_Reported.insert(fmt::format("{}/{}/{}", static_cast<uint64_t>(entity.GetUUID()), name, problem)).second)
				BS_CORE_WARN("UILayout: the {} on '{}' has invalid values ({}); drawing corrected ones", name, entity.GetName(), problem);
			return repaired;
		}

		// The entity's rect inside `parent`, or nullopt when its UIRect hides it (and its subtree), unless
		// `ignoreVisibility`.
		std::optional<UIBox> RectInParent(Entity entity, const UIBox& parent, bool ignoreVisibility = false)
		{
			const auto* component = entity.TryGetComponent<UIRectComponent>();
			if (!component)
				return parent;
			const UIRectComponent rect = Repaired(entity, *component, &SanitizeUIRect, &ValidateUIRect, "UIRect");
			if (!rect.Visible && !ignoreVisibility)
				return std::nullopt;
			return ComputeUIRect(parent, rect);
		}

		// Depth-first in hierarchy order. Scene bounds hierarchy depth (Scene::MaxHierarchyDepth), so the
		// recursion is bounded too.
		void Visit(Scene& scene, Entity entity, const UIBox& parent, std::vector<UILayoutItem>& out)
		{
			const std::optional<UIBox> rect = RectInParent(entity, parent);
			if (!rect)
				return;
			if (HasUIComponent(entity))
				out.push_back({ entity, *rect });
			for (UUID child : entity.GetComponent<RelationshipComponent>().Children)
			{
				if (Entity childEntity = scene.GetEntityByUUID(child))
					Visit(scene, childEntity, *rect, out);
			}
		}

		// The draw commands of one laid-out entity: its image, then its text.
		void AppendCommands(const UILayoutItem& item, std::vector<UIDrawCommand>& out)
		{
			if (const auto* component = item.Owner.TryGetComponent<UIImageComponent>())
			{
				const UIImageComponent image = Repaired(item.Owner, *component, &SanitizeUIImage, &ValidateUIImage, "UIImage");
				UIDrawCommand& command = out.emplace_back();
				command.Kind = UIDrawKind::Image;
				command.Position = item.Rect.Min;
				command.Size = item.Rect.Size;
				command.Color = image.Color;
				command.Texture = image.Texture;
				command.Slice = image.Slice;
				command.SliceScale = image.SliceScale;
			}
			if (const auto* component = item.Owner.TryGetComponent<UITextComponent>())
			{
				const UITextComponent text = Repaired(item.Owner, *component, &SanitizeUIText, &ValidateUIText, "UIText");
				UIDrawCommand& command = out.emplace_back();
				command.Kind = UIDrawKind::Text;
				command.Position = item.Rect.Min;
				command.Size = item.Rect.Size;
				command.Color = text.Color;
				command.Text = text.Text;
				command.Font = text.Font;
				command.FontSize = text.FontSize;
				command.Align = { UIAlignFraction(text.HorizontalAlign), UIAlignFraction(text.VerticalAlign) };
				command.Wrap = text.Wrap;
				command.Shadow = text.Shadow;
			}
		}

		// The entity's ancestors, root first.
		std::vector<Entity> GetAncestors(Entity entity)
		{
			std::vector<Entity> ancestors;
			for (Entity current = entity.GetParent(); current; current = current.GetParent())
				ancestors.push_back(current);
			std::reverse(ancestors.begin(), ancestors.end());
			return ancestors;
		}

	}

	bool UILayout::HasUI(Scene& scene)
	{
		return !scene.GetAllEntitiesWith<UIRectComponent>().empty() || !scene.GetAllEntitiesWith<UIImageComponent>().empty() || !scene.GetAllEntitiesWith<UITextComponent>().empty();
	}

	std::vector<UILayoutItem> UILayout::Layout(Scene& scene, const glm::vec2& canvasSize)
	{
		std::vector<UILayoutItem> items;
		if (!HasUI(scene))
			return items;
		const UIBox canvas{ { 0.0f, 0.0f }, canvasSize };
		for (Entity root : scene.GetRootEntities())
			Visit(scene, root, canvas, items);
		return items;
	}

	void UILayout::BuildCommands(Scene& scene, const glm::vec2& canvasSize, std::vector<UIDrawCommand>& out)
	{
		for (const UILayoutItem& item : Layout(scene, canvasSize))
			AppendCommands(item, out);
	}

	std::optional<UIBox> UILayout::GetRect(Scene& scene, Entity entity, const glm::vec2& canvasSize)
	{
		if (!entity || entity.GetScene() != &scene || !HasUIComponent(entity))
			return std::nullopt;
		std::optional<UIBox> rect = UIBox{ { 0.0f, 0.0f }, canvasSize };
		for (Entity ancestor : GetAncestors(entity))
		{
			rect = RectInParent(ancestor, *rect);
			if (!rect)
				return std::nullopt;
		}
		return RectInParent(entity, *rect);
	}

	UIBox UILayout::GetParentRect(Scene& scene, Entity entity, const glm::vec2& canvasSize)
	{
		UIBox rect{ { 0.0f, 0.0f }, canvasSize };
		if (!entity || entity.GetScene() != &scene)
			return rect;
		for (Entity ancestor : GetAncestors(entity))
			rect = *RectInParent(ancestor, rect, true);
		return rect;
	}

}
