#include "Basalt/UI/UILayout.h"

#include "Basalt/Scene/Scene.h"
#include "Basalt/UI/UIRules.h"

namespace Basalt {

	namespace {

		bool HasUIComponent(Entity entity)
		{
			return entity.HasComponent<UIRectComponent>() || entity.HasComponent<UIImageComponent>() || entity.HasComponent<UITextComponent>();
		}

		// The entity's rect inside `parent`, or nullopt when its UIRect hides it (and its subtree).
		std::optional<UIBox> RectInParent(Entity entity, const UIBox& parent)
		{
			const auto* component = entity.TryGetComponent<UIRectComponent>();
			if (!component)
				return parent;
			const UIRectComponent rect = SanitizeUIRect(*component);
			if (!rect.Visible)
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

		float HorizontalAlign(UIHorizontalAlign align)
		{
			return static_cast<float>(align) * 0.5f;
		}

		float VerticalAlign(UIVerticalAlign align)
		{
			return static_cast<float>(align) * 0.5f;
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

	void UILayout::AppendCommands(const UILayoutItem& item, std::vector<UIDrawCommand>& out)
	{
		if (const auto* component = item.Owner.TryGetComponent<UIImageComponent>())
		{
			const UIImageComponent image = SanitizeUIImage(*component);
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
			const UITextComponent text = SanitizeUIText(*component);
			UIDrawCommand& command = out.emplace_back();
			command.Kind = UIDrawKind::Text;
			command.Position = item.Rect.Min;
			command.Size = item.Rect.Size;
			command.Color = text.Color;
			command.Text = text.Text;
			command.Font = text.Font;
			command.FontSize = text.FontSize;
			command.Align = { HorizontalAlign(text.HorizontalAlign), VerticalAlign(text.VerticalAlign) };
			command.Wrap = text.Wrap;
			command.Shadow = text.Shadow;
		}
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

		// Root first, then down to the entity.
		std::vector<Entity> chain;
		for (Entity current = entity; current; current = current.GetParent())
			chain.push_back(current);

		std::optional<UIBox> rect = UIBox{ { 0.0f, 0.0f }, canvasSize };
		for (auto it = chain.rbegin(); it != chain.rend() && rect; ++it)
			rect = RectInParent(*it, *rect);
		return rect;
	}

}
