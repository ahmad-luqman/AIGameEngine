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
			return entity.HasComponent<UIRectComponent>() || entity.HasComponent<UIImageComponent>() || entity.HasComponent<UITextComponent>() || entity.HasComponent<UIFocusRingComponent>() || entity.HasComponent<UINavigationComponent>() || UILayout::GetWidgetKind(entity).has_value();
		}

		// Reported problems, bounded so a long session with many broken entities cannot grow it without end.
		constexpr size_t MaxReportedProblems = 1024;

		// Whether to warn about a problem with an entity's UI: once per entity and `kind` (the problem may be met
		// every frame), and not at all after MaxReportedProblems problems (saying so once).
		bool ShouldReport(Entity entity, const char* kind)
		{
			static std::mutex s_Mutex;
			static std::unordered_set<std::string> s_Reported;
			std::scoped_lock lock(s_Mutex);
			if (s_Reported.size() > MaxReportedProblems)
				return false;
			if (s_Reported.size() == MaxReportedProblems)
			{
				s_Reported.insert({});
				BS_CORE_WARN("UILayout: more than {} UI problems; no longer reporting them", MaxReportedProblems);
				return false;
			}
			return s_Reported.insert(fmt::format("{}/{}", static_cast<uint64_t>(entity.GetUUID()), kind)).second;
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
			if (ShouldReport(entity, name))
			{
				const std::string problem = validate(component);
				BS_CORE_ASSERT(!problem.empty(), "the {} rules repair a value they accept", name);
				BS_CORE_WARN("UILayout: the {} on '{}' has invalid values ({}); drawing corrected ones", name, entity.GetName(), problem);
			}
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
		void Visit(Scene& scene, Entity entity, const UIBox& parent, std::vector<UILayoutItem>& out, const glm::mat3& parentTransform = glm::mat3(1.0f), std::optional<UIBox> clip = std::nullopt)
		{
			const std::optional<UIBox> rect = VisibleRectInParent(entity, parent);
			if (!rect)
				return;
			glm::mat3 transform = parentTransform;
			const auto* component = entity.TryGetComponent<UIRectComponent>();
			if (component)
				transform *= ComputeUITransform(*rect, SanitizeUIRect(*component));
			if (!IsUsableUITransform(*rect, transform))
			{
				if (ShouldReport(entity, "composed transform"))
					BS_CORE_WARN("UILayout: '{}' has a composed UI transform outside the supported numeric range; skipping its subtree", entity.GetName());
				return;
			}
			if (HasUIComponent(entity))
				out.push_back({ entity, *rect, transform, clip });
			if ((component && component->ClipChildren) || entity.HasComponent<UIScrollViewComponent>())
			{
				const UIBox bounds = TransformUIBox(*rect, transform);
				clip = clip ? IntersectUIBoxes(*clip, bounds) : bounds;
			}
			UIBox childRect = *rect;
			if (const auto* scroll = entity.TryGetComponent<UIScrollViewComponent>())
			{
				const auto repaired = SanitizeUIScrollView(*scroll);
				childRect.Size = glm::max(rect->Size, repaired.ContentSize);
				childRect.Min -= glm::clamp(repaired.Offset, glm::vec2(0), childRect.Size - rect->Size);
			}
			for (UUID child : entity.GetComponent<RelationshipComponent>().Children)
			{
				if (Entity childEntity = scene.GetEntityByUUID(child))
					Visit(scene, childEntity, childRect, out, transform, clip);
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
			const std::optional<UIWidgetInfo> widget = UILayout::GetWidget(entity);
			const auto is = [&widget](UIWidgetKind kind) { return widget && widget->Kind == kind; };
			const bool interactable = !widget || widget->Interactable;
			glm::vec4 imageTint = { 1.0f, 1.0f, 1.0f, 1.0f };
			if (is(UIWidgetKind::Button))
			{
				const UIButtonComponent button = Repaired(entity, entity.GetComponent<UIButtonComponent>(), &SanitizeUIButton, &ValidateUIButton, "UIButton");
				imageTint = UIWidgetTints::Of(button).For(WidgetState(interaction, entity, interactable));
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
				command.UVMin = image.UVMin;
				command.UVMax = image.UVMax;
				command.SpriteColumns = image.SpriteColumns;
				command.SpriteRows = image.SpriteRows;
				command.SpriteFrame = image.SpriteFrame;
			}
			UIBox textRect = item.Rect;
			if (is(UIWidgetKind::Slider))
			{
				const UISliderComponent slider = Repaired(entity, entity.GetComponent<UISliderComponent>(), &SanitizeUISlider, &ValidateUISlider, "UISlider");
				const glm::vec4 tint = UIWidgetTints::Of(slider).For(WidgetState(interaction, entity, interactable));
				UIWidgets::AppendSlider(item.Rect, UIWidgets::SliderFraction(slider.Value, slider.Min, slider.Max), slider.HandleWidth, slider.TrackColor, slider.FillColor, slider.HandleColor * tint, out, slider.Vertical);
			}
			else if (is(UIWidgetKind::Checkbox))
			{
				const UICheckboxComponent checkbox = Repaired(entity, entity.GetComponent<UICheckboxComponent>(), &SanitizeUICheckbox, &ValidateUICheckbox, "UICheckbox");
				const glm::vec4 tint = UIWidgetTints::Of(checkbox).For(WidgetState(interaction, entity, interactable));
				UIWidgets::AppendCheckbox(item.Rect, checkbox.Checked, checkbox.BoxColor * tint, checkbox.CheckColor, out);
				textRect = UIWidgets::CheckboxLabelBox(item.Rect);
			}
			if (is(UIWidgetKind::TextInput) || is(UIWidgetKind::Dropdown))
			{
				UITextComponent text = entity.HasComponent<UITextComponent>() ? SanitizeUIText(entity.GetComponent<UITextComponent>()) : UITextComponent{};
				glm::vec4 tint(1.0f);
				if (is(UIWidgetKind::TextInput))
				{
					const auto input = SanitizeUITextInput(entity.GetComponent<UITextInputComponent>());
					tint = UIWidgetTints::Of(input).For(WidgetState(interaction, entity, input.Interactable));
					text.Text = input.Text.empty() ? input.Placeholder : input.Text;
					if (input.Text.empty())
						text.Color.a *= 0.5f;
					if (interaction && interaction->GetFocus() == UIWidgetID::ForEntity(entity.GetUUID()))
					{
						text.Text = input.Text;
						size_t cursor = std::min(interaction->GetTextCursor(), text.Text.size());
						while (cursor > 0 && cursor < text.Text.size() && (static_cast<unsigned char>(text.Text[cursor]) & 0xc0U) == 0x80U)
							cursor--;
						text.Text.insert(cursor, "|");
					}
				}
				else
				{
					const auto dropdown = SanitizeUIDropdown(entity.GetComponent<UIDropdownComponent>());
					tint = UIWidgetTints::Of(dropdown).For(WidgetState(interaction, entity, dropdown.Interactable));
					const auto options = UIWidgets::DropdownOptions(dropdown.Options);
					text.Text = dropdown.Selected < options.size() ? std::string(options[dropdown.Selected]) : std::string();
					text.Text += "  v";
				}
				UIWidgets::AppendRect(item.Rect, glm::vec4(0.12f, 0.12f, 0.15f, 0.95f) * tint, out);
				UIDrawCommand& command = out.emplace_back();
				command.Kind = UIDrawKind::Text;
				command.Position = item.Rect.Min + glm::vec2(4.0f, 0.0f);
				command.Size = glm::max(item.Rect.Size - glm::vec2(8.0f, 0.0f), glm::vec2(0));
				command.Text = text.Text;
				command.Font = text.Font;
				command.FontSize = text.FontSize;
				command.Color = text.Color;
				command.Align = { 0.0f, 0.5f };
				command.Clip = TransformUIBox(item.Rect, item.Transform);
			}
			if (const auto* component = entity.TryGetComponent<UITextComponent>(); component && !is(UIWidgetKind::TextInput) && !is(UIWidgetKind::Dropdown))
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
			if (const auto* component = entity.TryGetComponent<UIFocusRingComponent>(); component && interaction && interaction->GetFocus() == UIWidgetID::ForEntity(entity.GetUUID()))
			{
				const auto ring = SanitizeUIFocusRing(*component);
				const float thickness = std::min({ ring.Thickness, item.Rect.Size.x * 0.5f, item.Rect.Size.y * 0.5f });
				UIWidgets::AppendRect({ item.Rect.Min, { item.Rect.Size.x, thickness } }, ring.Color, out);
				UIWidgets::AppendRect({ item.Rect.Min + glm::vec2(0, item.Rect.Size.y - thickness), { item.Rect.Size.x, thickness } }, ring.Color, out);
				UIWidgets::AppendRect({ item.Rect.Min, { thickness, item.Rect.Size.y } }, ring.Color, out);
				UIWidgets::AppendRect({ item.Rect.Min + glm::vec2(item.Rect.Size.x - thickness, 0), { thickness, item.Rect.Size.y } }, ring.Color, out);
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
		return AnyEntityWith<UIRectComponent, UIImageComponent, UITextComponent, UIFocusRingComponent, UINavigationComponent>(scene) || HasWidgets(scene);
	}

	bool UILayout::HasWidgets(Scene& scene)
	{
		return AnyEntityWith<UIButtonComponent, UISliderComponent, UICheckboxComponent, UITextInputComponent, UIDropdownComponent, UIPanelComponent, UIScrollViewComponent>(scene);
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
		{
			const size_t begin = out.size();
			AppendCommands(item, interaction, out);
			for (size_t i = begin; i < out.size(); ++i)
			{
				out[i].Transform = item.Transform;
				if (item.Clip)
					out[i].Clip = out[i].Clip ? IntersectUIBoxes(*out[i].Clip, *item.Clip) : item.Clip;
			}
		}
		if (interaction && interaction->GetOpenDropdown() != 0)
		{
			for (const UILayoutItem& item : Layout(scene, canvasSize))
			{
				if (item.Owner.GetUUID() != interaction->GetOpenDropdown())
					continue;
				const auto dropdown = SanitizeUIDropdown(item.Owner.GetComponent<UIDropdownComponent>());
				const auto options = UIWidgets::DropdownOptions(dropdown.Options);
				const UITextComponent text = item.Owner.HasComponent<UITextComponent>() ? SanitizeUIText(item.Owner.GetComponent<UITextComponent>()) : UITextComponent{};
				for (size_t index = 0; index < options.size(); index++)
				{
					UIBox row = item.Rect;
					row.Min.y += row.Size.y * static_cast<float>(index + 1);
					const size_t begin = out.size();
					UIWidgets::AppendRect(row, index == interaction->GetDropdownHighlight() ? glm::vec4(0.2f, 0.4f, 0.7f, 1.0f) : glm::vec4(0.12f, 0.12f, 0.15f, 1.0f), out);
					UIDrawCommand& command = out.emplace_back();
					command.Kind = UIDrawKind::Text;
					command.Position = row.Min + glm::vec2(4, 0);
					command.Size = glm::max(row.Size - glm::vec2(8, 0), glm::vec2(0));
					command.Text = options[index];
					command.Font = text.Font;
					command.FontSize = text.FontSize;
					command.Color = text.Color;
					command.Align = { 0.0f, 0.5f };
					for (size_t i = begin; i < out.size(); i++)
					{
						out[i].Transform = item.Transform;
						const UIBox clip = TransformUIBox(row, item.Transform);
						out[i].Clip = item.Clip ? IntersectUIBoxes(clip, *item.Clip) : clip;
					}
				}
				break;
			}
		}
	}

	std::optional<UIBox> UILayout::GetRect(Scene& scene, Entity entity, const glm::vec2& canvasSize)
	{
		if (!entity || entity.GetScene() != &scene || !HasUIComponent(entity))
			return std::nullopt;
		for (const auto& item : Layout(scene, canvasSize))
			if (item.Owner == entity)
				return item.Rect;
		return std::nullopt;
	}

	UIBox UILayout::GetParentRect(Scene& scene, Entity entity, const glm::vec2& canvasSize)
	{
		UIBox rect{ { 0.0f, 0.0f }, canvasSize };
		if (!entity || entity.GetScene() != &scene)
			return rect;
		for (Entity ancestor : GetAncestors(entity))
		{
			rect = RectInParent(ancestor, rect);
			if (const auto* scroll = ancestor.TryGetComponent<UIScrollViewComponent>())
			{
				const auto repaired = SanitizeUIScrollView(*scroll);
				const glm::vec2 content = glm::max(rect.Size, repaired.ContentSize);
				rect.Min -= glm::clamp(repaired.Offset, glm::vec2(0), content - rect.Size);
				rect.Size = content;
			}
		}
		return rect;
	}

	std::optional<UIWidgetInfo> UILayout::GetWidget(Entity entity)
	{
		const std::optional<UIWidgetKind> kind = GetWidgetKind(entity);
		if (!kind)
			return std::nullopt;
		UIWidgetInfo info;
		switch (*kind)
		{
			case UIWidgetKind::Button:
			{
				const auto& component = entity.GetComponent<UIButtonComponent>();
				const UIButtonComponent button = Repaired(entity, component, &SanitizeUIButton, &ValidateUIButton, "UIButton");
				info = { UIWidgetKind::Button, button.Interactable, button.Target, button.OnClick, component.OnClick };
				break;
			}
			case UIWidgetKind::Slider:
			{
				const auto& component = entity.GetComponent<UISliderComponent>();
				const UISliderComponent slider = Repaired(entity, component, &SanitizeUISlider, &ValidateUISlider, "UISlider");
				info = { UIWidgetKind::Slider, slider.Interactable, slider.Target, slider.OnValueChanged, component.OnValueChanged, slider.HandleWidth };
				break;
			}
			case UIWidgetKind::Checkbox:
			{
				const auto& component = entity.GetComponent<UICheckboxComponent>();
				const UICheckboxComponent checkbox = Repaired(entity, component, &SanitizeUICheckbox, &ValidateUICheckbox, "UICheckbox");
				info = { UIWidgetKind::Checkbox, checkbox.Interactable, checkbox.Target, checkbox.OnValueChanged, component.OnValueChanged };
				break;
			}
			case UIWidgetKind::TextInput:
			{
				const auto& component = entity.GetComponent<UITextInputComponent>();
				const auto widget = SanitizeUITextInput(component);
				info = { UIWidgetKind::TextInput, widget.Interactable, widget.Target, widget.OnValueChanged, component.OnValueChanged };
				break;
			}
			case UIWidgetKind::Dropdown:
			{
				const auto& component = entity.GetComponent<UIDropdownComponent>();
				const auto widget = SanitizeUIDropdown(component);
				info = { UIWidgetKind::Dropdown, widget.Interactable, widget.Target, widget.OnValueChanged, component.OnValueChanged };
				break;
			}
			case UIWidgetKind::Panel:
				info.Kind = UIWidgetKind::Panel;
				info.Interactable = false;
				break;
			case UIWidgetKind::ScrollView:
				info.Kind = UIWidgetKind::ScrollView;
				info.Interactable = entity.GetComponent<UIScrollViewComponent>().Interactable;
				break;
		}
		info.SeveralComponents = CountWidgetComponents(entity) > 1;
		if (info.SeveralComponents && ShouldReport(entity, "widgets"))
			BS_CORE_WARN("UILayout: '{}' has several widget components; it acts as its {} only", entity.GetName(), UIWidgetComponentName(info.Kind));
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
		if (entity.HasComponent<UITextInputComponent>())
			return UIWidgetKind::TextInput;
		if (entity.HasComponent<UIDropdownComponent>())
			return UIWidgetKind::Dropdown;
		if (entity.HasComponent<UIScrollViewComponent>())
			return UIWidgetKind::ScrollView;
		if (entity.HasComponent<UIPanelComponent>() && entity.GetComponent<UIPanelComponent>().BlockInput)
			return UIWidgetKind::Panel;
		return std::nullopt;
	}

	int UILayout::CountWidgetComponents(Entity entity)
	{
		return (entity.HasComponent<UIButtonComponent>() ? 1 : 0) + (entity.HasComponent<UISliderComponent>() ? 1 : 0) + (entity.HasComponent<UICheckboxComponent>() ? 1 : 0) + (entity.HasComponent<UITextInputComponent>() ? 1 : 0) + (entity.HasComponent<UIDropdownComponent>() ? 1 : 0);
	}

}
