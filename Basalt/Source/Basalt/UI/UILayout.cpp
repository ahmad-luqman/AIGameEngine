#include "Basalt/UI/UILayout.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/UI/UIInteraction.h"
#include "Basalt/UI/UIRules.h"

#include <algorithm>
#include <mutex>
#include <unordered_set>

namespace Basalt {

	namespace {

		bool HasUIComponent(Entity entity)
		{
			return entity.HasComponent<UIRectComponent>() || entity.HasComponent<UIImageComponent>() || entity.HasComponent<UITextComponent>() || UILayout::GetWidgetKind(entity).has_value();
		}

		// Reported problems, bounded so a long session with many broken entities cannot grow it without end.
		constexpr size_t MaxReportedProblems = 1024;

		// Warns about a problem with an entity's UI once per entity and `kind` (the problem may be met every
		// frame). `makeMessage` builds the warning only when it is logged.
		template<typename MakeMessage>
		void WarnOnce(Entity entity, const char* kind, MakeMessage&& makeMessage)
		{
			static std::mutex s_Mutex;
			static std::unordered_set<std::string> s_Reported;
			std::scoped_lock lock(s_Mutex);
			if (s_Reported.size() > MaxReportedProblems)
				return;
			if (s_Reported.size() == MaxReportedProblems)
			{
				s_Reported.insert({});
				BS_CORE_WARN("UILayout: more than {} UI problems; no longer reporting them", MaxReportedProblems);
				return;
			}
			if (s_Reported.insert(fmt::format("{}/{}", static_cast<uint64_t>(entity.GetUUID()), kind)).second)
				BS_CORE_WARN("{}", makeMessage());
		}

		// The registry rejects invalid values, so only C++ code writing a component directly produces them.
		// The layout draws repaired values and says so, once per entity and component (the value may change
		// every frame, as when C++ animates it).
		template<typename T>
		T Repaired(Entity entity, const T& component, T (*sanitize)(T), std::string (*validate)(const T&), const char* name)
		{
			T repaired = sanitize(component);
			if (repaired == component)
				return repaired;
			WarnOnce(entity, name, [&] {
				const std::string problem = validate(component);
				BS_CORE_ASSERT(!problem.empty(), "the {} rules repair a value they accept", name);
				return fmt::format("UILayout: the {} on '{}' has invalid values ({}); drawing corrected ones", name, entity.GetName(), problem);
			});
			return repaired;
		}

		// The entity's rect inside `parent` (its UIRect's, or the parent's when it has none), whether visible
		// or not.
		UIBox RectInParent(Entity entity, const UIBox& parent)
		{
			const auto* component = entity.TryGetComponent<UIRectComponent>();
			return component ? ComputeUIRect(parent, Repaired(entity, *component, &SanitizeUIRect, &ValidateUIRect, "UIRect")) : parent;
		}

		// Whether a UIRect hides the entity (and its subtree).
		bool IsHidden(Entity entity)
		{
			const auto* component = entity.TryGetComponent<UIRectComponent>();
			return component && !component->Visible;
		}

		// RectInParent, or nullopt when the entity is hidden.
		std::optional<UIBox> VisibleRectInParent(Entity entity, const UIBox& parent)
		{
			const UIBox rect = RectInParent(entity, parent);
			return IsHidden(entity) ? std::nullopt : std::optional(rect);
		}

		// Depth-first in hierarchy order. Scene bounds hierarchy depth (Scene::MaxHierarchyDepth), so the
		// recursion is bounded too.
		void Visit(Scene& scene, Entity entity, const UIBox& parent, std::vector<UILayoutItem>& out)
		{
			const std::optional<UIBox> rect = VisibleRectInParent(entity, parent);
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

		// The state a widget is drawn in.
		UIWidgetState WidgetState(const UIInteraction* interaction, Entity entity, bool interactable)
		{
			if (!interaction)
				return interactable ? UIWidgetState::Normal : UIWidgetState::Disabled;
			return interaction->GetState(UIWidgetID::ForEntity(entity.GetUUID()), interactable);
		}

		// The draw commands of one laid-out entity: its image (tinted by a button), its slider or checkbox, then its
		// text (beside a checkbox's box).
		void AppendCommands(const UILayoutItem& item, const UIInteraction* interaction, std::vector<UIDrawCommand>& out)
		{
			const Entity entity = item.Owner;
			const std::optional<UIWidgetInfo> info = UILayout::GetWidget(entity);
			const std::optional<UIWidgetKind> widget = info ? std::optional(info->Kind) : std::nullopt;
			glm::vec4 imageTint = { 1.0f, 1.0f, 1.0f, 1.0f };
			if (widget == UIWidgetKind::Button)
			{
				const UIButtonComponent button = Repaired(entity, entity.GetComponent<UIButtonComponent>(), &SanitizeUIButton, &ValidateUIButton, "UIButton");
				imageTint = UIWidgetTints::Of(button).For(WidgetState(interaction, entity, button.Interactable));
			}
			if (const auto* component = entity.TryGetComponent<UIImageComponent>())
			{
				const UIImageComponent image = Repaired(entity, *component, &SanitizeUIImage, &ValidateUIImage, "UIImage");
				UIDrawCommand& command = out.emplace_back();
				command.Kind = UIDrawKind::Image;
				command.Position = item.Rect.Min;
				command.Size = item.Rect.Size;
				command.Color = image.Color * imageTint;
				command.Texture = image.Texture;
				command.Slice = image.Slice;
				command.SliceScale = image.SliceScale;
			}
			UIBox textRect = item.Rect;
			if (widget == UIWidgetKind::Slider)
			{
				const UISliderComponent slider = Repaired(entity, entity.GetComponent<UISliderComponent>(), &SanitizeUISlider, &ValidateUISlider, "UISlider");
				const glm::vec4 tint = UIWidgetTints::Of(slider).For(WidgetState(interaction, entity, slider.Interactable));
				UIWidgets::AppendSlider(item.Rect, UIWidgets::SliderFraction(slider.Value, slider.Min, slider.Max), slider.HandleWidth, slider.TrackColor, slider.FillColor, slider.HandleColor * tint, out);
			}
			else if (widget == UIWidgetKind::Checkbox)
			{
				const UICheckboxComponent checkbox = Repaired(entity, entity.GetComponent<UICheckboxComponent>(), &SanitizeUICheckbox, &ValidateUICheckbox, "UICheckbox");
				const glm::vec4 tint = UIWidgetTints::Of(checkbox).For(WidgetState(interaction, entity, checkbox.Interactable));
				UIWidgets::AppendCheckbox(item.Rect, checkbox.Checked, checkbox.BoxColor * tint, checkbox.CheckColor, out);
				textRect = UIWidgets::CheckboxLabelBox(item.Rect);
			}
			if (const auto* component = entity.TryGetComponent<UITextComponent>())
			{
				const UITextComponent text = Repaired(entity, *component, &SanitizeUIText, &ValidateUIText, "UIText");
				UIDrawCommand& command = out.emplace_back();
				command.Kind = UIDrawKind::Text;
				command.Position = textRect.Min;
				command.Size = textRect.Size;
				command.Color = text.Color;
				command.Text = text.Text;
				command.Font = text.Font;
				command.FontSize = text.FontSize;
				command.Align = { UIAlignFraction(text.HorizontalAlign), UIAlignFraction(text.VerticalAlign) };
				command.Wrap = text.Wrap;
				command.Shadow = text.Shadow;
			}
		}

		template<typename... Components>
		bool AnyEntityWith(Scene& scene)
		{
			return (!scene.GetAllEntitiesWith<Components>().empty() || ...);
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
		return AnyEntityWith<UIRectComponent, UIImageComponent, UITextComponent>(scene) || HasWidgets(scene);
	}

	bool UILayout::HasWidgets(Scene& scene)
	{
		return AnyEntityWith<UIButtonComponent, UISliderComponent, UICheckboxComponent>(scene);
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
		const UIInteraction* interaction = scene.GetUIInteraction();
		for (const UILayoutItem& item : Layout(scene, canvasSize))
			AppendCommands(item, interaction, out);
	}

	std::optional<UIBox> UILayout::GetRect(Scene& scene, Entity entity, const glm::vec2& canvasSize)
	{
		if (!entity || entity.GetScene() != &scene || !HasUIComponent(entity))
			return std::nullopt;
		UIBox rect{ { 0.0f, 0.0f }, canvasSize };
		for (Entity ancestor : GetAncestors(entity))
		{
			const std::optional<UIBox> next = VisibleRectInParent(ancestor, rect);
			if (!next)
				return std::nullopt;
			rect = *next;
		}
		return VisibleRectInParent(entity, rect);
	}

	UIBox UILayout::GetParentRect(Scene& scene, Entity entity, const glm::vec2& canvasSize)
	{
		UIBox rect{ { 0.0f, 0.0f }, canvasSize };
		if (!entity || entity.GetScene() != &scene)
			return rect;
		for (Entity ancestor : GetAncestors(entity))
			rect = RectInParent(ancestor, rect);
		return rect;
	}

	std::optional<UIWidgetInfo> UILayout::GetWidget(Entity entity)
	{
		std::optional<UIWidgetInfo> info;
		if (const auto* component = entity.TryGetComponent<UIButtonComponent>())
		{
			const UIButtonComponent button = Repaired(entity, *component, &SanitizeUIButton, &ValidateUIButton, "UIButton");
			info = UIWidgetInfo{ UIWidgetKind::Button, button.Interactable, button.Target, button.OnClick };
		}
		else if (const auto* slider = entity.TryGetComponent<UISliderComponent>())
		{
			const UISliderComponent repaired = Repaired(entity, *slider, &SanitizeUISlider, &ValidateUISlider, "UISlider");
			info = UIWidgetInfo{ UIWidgetKind::Slider, repaired.Interactable, repaired.Target, repaired.OnValueChanged };
		}
		else if (const auto* checkbox = entity.TryGetComponent<UICheckboxComponent>())
		{
			const UICheckboxComponent repaired = Repaired(entity, *checkbox, &SanitizeUICheckbox, &ValidateUICheckbox, "UICheckbox");
			info = UIWidgetInfo{ UIWidgetKind::Checkbox, repaired.Interactable, repaired.Target, repaired.OnValueChanged };
		}
		if (info && CountWidgetComponents(entity) > 1)
			WarnOnce(entity, "widgets", [&] { return fmt::format("UILayout: '{}' has several widget components; it acts as its {} only", entity.GetName(), UIWidgetComponentName(info->Kind)); });
		return info;
	}

	std::optional<UIWidgetKind> UILayout::GetWidgetKind(Entity entity)
	{
		if (entity.HasComponent<UIButtonComponent>())
			return UIWidgetKind::Button;
		if (entity.HasComponent<UISliderComponent>())
			return UIWidgetKind::Slider;
		if (entity.HasComponent<UICheckboxComponent>())
			return UIWidgetKind::Checkbox;
		return std::nullopt;
	}

	int UILayout::CountWidgetComponents(Entity entity)
	{
		return (entity.HasComponent<UIButtonComponent>() ? 1 : 0) + (entity.HasComponent<UISliderComponent>() ? 1 : 0) + (entity.HasComponent<UICheckboxComponent>() ? 1 : 0);
	}

}
