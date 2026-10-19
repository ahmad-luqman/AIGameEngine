#include "Basalt/UI/UIInteraction.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Input.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/UI/GameUI.h"
#include "Basalt/UI/UILayout.h"
#include "Basalt/UI/UIRules.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <utility>

namespace Basalt {

	namespace {

		// Whether any connected gamepad satisfies `test`.
		template<typename Test>
		bool AnyGamepad(Test&& test)
		{
			for (uint32_t index = 0; index < Input::MaxGamepads; index++)
			{
				if (Input::IsGamepadConnected(index) && test(index))
					return true;
			}
			return false;
		}

		// Enter, keypad Enter, Space or any gamepad's A, by `key` and `button` (Down or Pressed).
		template<typename KeyRead, typename ButtonRead>
		bool IsAccept(KeyRead&& key, ButtonRead&& button)
		{
			return key(KeyCode::Enter) || key(KeyCode::KPEnter) || key(KeyCode::Space) || AnyGamepad([button](uint32_t index) { return button(index, GamepadButton::A); });
		}

		// The inputs that move the focus one way, and that way on the canvas.
		struct DirectionBinding
		{
			KeyCode Key;
			GamepadButton DPad;
			GamepadAxis Axis;
			// The sign of the stick axis in this direction.
			float Sign;
			// The stick's other axis, and whether this direction wins when both are pushed equally far.
			GamepadAxis CrossAxis;
			bool WinsTies;
			glm::vec2 Vector;
		};

		// In Direction order. Vertical first (one move per frame, and menus are mostly columns): a stick pushed exactly
		// diagonally moves vertically.
		constexpr DirectionBinding DirectionBindings[] = {
			{ KeyCode::Up, GamepadButton::DPadUp, GamepadAxis::LeftY, -1.0f, GamepadAxis::LeftX, true, { 0.0f, -1.0f } },
			{ KeyCode::Down, GamepadButton::DPadDown, GamepadAxis::LeftY, 1.0f, GamepadAxis::LeftX, true, { 0.0f, 1.0f } },
			{ KeyCode::Left, GamepadButton::DPadLeft, GamepadAxis::LeftX, -1.0f, GamepadAxis::LeftY, false, { -1.0f, 0.0f } },
			{ KeyCode::Right, GamepadButton::DPadRight, GamepadAxis::LeftX, 1.0f, GamepadAxis::LeftY, false, { 1.0f, 0.0f } },
		};

		// Whether a gamepad's left stick is pushed in the binding's direction, along its dominant axis only: a diagonal
		// push moves one way, not two.
		bool IsStickPushed(uint32_t index, const DirectionBinding& binding)
		{
			const float along = GameInput::GetGamepadAxis(index, binding.Axis) * binding.Sign;
			const float across = std::abs(GameInput::GetGamepadAxis(index, binding.CrossAxis));
			return along >= UIInteraction::StickThreshold && (along > across || (binding.WinsTies && along == across));
		}

		// The direction is held on the keyboard, a D-pad or a left stick.
		bool IsDirectionHeld(const DirectionBinding& binding)
		{
			return GameInput::IsKeyDown(binding.Key) || AnyGamepad([&binding](uint32_t index) { return GameInput::IsGamepadButtonDown(index, binding.DPad) || IsStickPushed(index, binding); });
		}

		// The widget nearest `from` in `direction` (a unit axis): by center distance along it plus twice the sideways
		// offset; ties go to the earlier widget; nullptr when none lies that way.
		template<typename Candidate>
		const Candidate* FindNeighbor(const std::vector<Candidate>& widgets, const Candidate& from, const glm::vec2& direction)
		{
			const glm::vec2 origin = glm::vec2(from.Transform * glm::vec3(from.Rect.Center(), 1.0f));
			const Candidate* best = nullptr;
			float bestScore = std::numeric_limits<float>::max();
			for (const Candidate& widget : widgets)
			{
				if (!widget.IsNavigable() || widget.ID == from.ID)
					continue;
				const glm::vec2 offset = glm::vec2(widget.Transform * glm::vec3(widget.Rect.Center(), 1.0f)) - origin;
				const float along = glm::dot(offset, direction);
				if (along <= 0.0f)
					continue;
				const float sideways = std::abs(offset.x * direction.y - offset.y * direction.x);
				const float score = along + 2.0f * sideways;
				if (score < bestScore)
				{
					bestScore = score;
					best = &widget;
				}
			}
			return best;
		}

		// The live entity of a widget component candidate.
		Entity WidgetEntity(Scene& scene, const UIWidgetID& id)
		{
			Entity entity = scene.GetEntityByUUID(id.GetEntity());
			BS_CORE_ASSERT(entity, "candidates are live entities");
			return entity;
		}

	}

	UIWidgetID UIWidgetID::ForEntity(UUID entity)
	{
		BS_CORE_ASSERT(entity != 0, "a widget entity has a UUID");
		UIWidgetID id;
		id.m_Entity = entity;
		return id;
	}

	UIWidgetID UIWidgetID::ForName(std::string name)
	{
		BS_CORE_ASSERT(!name.empty(), "an immediate-mode widget id is not empty");
		UIWidgetID id;
		id.m_Name = std::move(name);
		return id;
	}

	void UIInteraction::Update(Scene& scene, float deltaTime)
	{
		m_Events.clear();
		m_ImmediateInputs.clear();
		m_LastImmediate = std::move(m_Immediate);
		m_Immediate.clear();
		m_ImmediateIDs.clear();

		if (m_OpenDropdown != 0)
		{
			Entity owner = scene.GetEntityByUUID(m_OpenDropdown);
			if (!owner || !owner.HasComponent<UIDropdownComponent>() || !owner.GetComponent<UIDropdownComponent>().Interactable || !UILayout::GetRect(scene, owner, GameUI::GetCanvasSize()))
				m_OpenDropdown = 0;
		}
		const std::vector<Candidate> widgets = CollectWidgets(scene);
		// A held or focused widget that is gone, hidden or disabled lets go.
		const auto isLive = [&widgets](const UIWidgetID& id) {
			return std::ranges::any_of(widgets, [&id](const Candidate& widget) { return widget.IsNavigable() && widget.ID == id; });
		};
		if (m_Focus.ID.IsValid())
		{
			if (isLive(m_Focus.ID))
			{
				m_Focus.Phase = FocusPhase::Seen;
			}
			else if (m_Focus.Phase == FocusPhase::New)
			{
				m_Focus.Phase = FocusPhase::Unseen;
			}
			else
			{
				if (m_Focus.Phase == FocusPhase::Unseen)
				{
					const Entity entity = m_Focus.ID.IsEntity() ? scene.GetEntityByUUID(m_Focus.ID.GetEntity()) : Entity();
					const std::string name = entity ? entity.GetName() : (m_Focus.ID.IsName() ? m_Focus.ID.GetName() : std::to_string(static_cast<uint64_t>(m_Focus.ID.GetEntity())));
					BS_CORE_WARN("UI: the focus was set to '{}', which is not a visible, interactable widget; dropped it", name);
				}
				SetFocus({});
			}
		}
		if (m_Pressed && !isLive(m_Pressed->ID))
			m_Pressed.reset();

		UpdatePointer(scene, widgets);
		UpdateNavigation(scene, widgets, deltaTime);
		UpdateText(scene, widgets);
	}

	std::vector<UIWidgetEvent> UIInteraction::TakeEvents()
	{
		return std::exchange(m_Events, {});
	}

	void UIInteraction::OnSceneLoaded()
	{
		m_Hovered = {};
		m_OpenDropdown = 0;
		m_TextFocus = {};
		m_PointerBlocked = false;
		m_LastPointer.reset();
		m_Pressed.reset();
		SetFocus({});
		// The load comes at the end of a frame, after the old scene's scripts drew this frame's widgets.
		m_Immediate.clear();
		m_ImmediateIDs.clear();
		m_HeldTime.fill(-1.0f);
	}

	UIInteraction::SubmitResult UIInteraction::SubmitImmediate(UIImmediateWidget widget, UIImmediateInput& outInput)
	{
		BS_CORE_ASSERT(!widget.ID.empty() && widget.Size.x > 0.0f && widget.Size.y > 0.0f, "immediate-mode widgets have an id and an area");
		if (m_ImmediateIDs.contains(widget.ID))
			return SubmitResult::DuplicateID;
		if (m_Immediate.size() >= MaxImmediateWidgets)
			return SubmitResult::TooMany;
		auto it = m_ImmediateInputs.find(widget.ID);
		// The input arrived for last frame's widget: one drawn disabled now does not act on it.
		outInput = it != m_ImmediateInputs.end() && widget.Interactable ? it->second : UIImmediateInput{};
		outInput.State = GetState(UIWidgetID::ForName(widget.ID), widget.Interactable);
		m_ImmediateIDs.insert(widget.ID);
		m_Immediate.push_back(std::move(widget));
		return SubmitResult::Accepted;
	}

	UIWidgetState UIInteraction::GetState(const UIWidgetID& id, bool interactable) const
	{
		if (!interactable)
			return UIWidgetState::Disabled;
		if (!id.IsValid())
			return UIWidgetState::Normal;
		const bool held = m_Pressed && m_Pressed->ID == id && (m_Hovered == id || m_Pressed->IsDrag);
		if (held || (id == m_Focus.ID && m_AcceptHeld))
			return UIWidgetState::Pressed;
		if (id == m_Hovered || id == m_Focus.ID)
			return UIWidgetState::Highlighted;
		return UIWidgetState::Normal;
	}

	void UIInteraction::SetFocus(UIWidgetID id)
	{
		m_Focus = { std::move(id), FocusPhase::New };
		m_AcceptHeld = false;
	}

	void UIInteraction::FocusLive(const UIWidgetID& id)
	{
		m_Focus = { id, FocusPhase::Seen };
		m_AcceptHeld = false;
	}

	void UIInteraction::SetNavigationEnabled(bool enabled)
	{
		m_NavigationEnabled = enabled;
		if (!enabled)
			SetFocus({});
	}

	std::vector<UIInteraction::Candidate> UIInteraction::CollectWidgets(Scene& scene) const
	{
		std::vector<Candidate> widgets;
		const glm::vec2 canvas = GameUI::GetCanvasSize();
		std::optional<Candidate> open;
		for (const UILayoutItem& item : UILayout::Layout(scene, canvas))
		{
			if (const std::optional<UIWidgetInfo> info = UILayout::GetWidget(item.Owner))
			{
				const UUID entity = item.Owner.GetUUID();
				Candidate candidate{ UIWidgetID::ForEntity(entity), info->Kind, item.Rect, info->Interactable, info->HandleWidth, info->ResolveTarget(entity), info->Callback };
				candidate.Transform = item.Transform;
				candidate.Clip = item.Clip;
				if (const auto* slider = item.Owner.TryGetComponent<UISliderComponent>())
					candidate.Vertical = slider->Vertical;
				widgets.push_back(candidate);
				if (entity == m_OpenDropdown)
					open = candidate;
			}
		}
		if (open)
		{
			const UIDropdownComponent dropdown = SanitizeUIDropdown(scene.GetEntityByUUID(m_OpenDropdown).GetComponent<UIDropdownComponent>());
			const auto options = UIWidgets::DropdownOptions(dropdown.Options);
			for (uint32_t i = 0; i < options.size(); i++)
			{
				Candidate option = *open;
				option.Rect.Min.y += option.Rect.Size.y * static_cast<float>(i + 1);
				option.Option = i;
				widgets.push_back(option);
			}
		}
		for (const UIImmediateWidget& widget : m_LastImmediate)
		{
			Candidate candidate{ UIWidgetID::ForName(widget.ID), widget.Kind, { canvas * widget.Anchor + widget.Offset, widget.Size }, widget.Interactable, widget.HandleWidth, 0, {} };
			candidate.Vertical = widget.Vertical;
			candidate.Clip = widget.Clip;
			widgets.push_back(candidate);
		}
		return widgets;
	}

	void UIInteraction::UpdatePointer(Scene& scene, const std::vector<Candidate>& widgets)
	{
		const std::optional<glm::vec2> pointer = GameUI::GetPointerPosition();
		m_Hovered = {};
		const Candidate* top = nullptr;
		if (pointer)
		{
			// Topmost first. A disabled widget still covers the ones below it.
			for (const Candidate& widget : std::ranges::reverse_view(widgets))
			{
				if (widget.Contains(*pointer))
				{
					top = &widget;
					break;
				}
			}
		}
		if (top && top->Option && pointer != m_LastPointer)
			m_DropdownHighlight = *top->Option;
		m_LastPointer = pointer;
		m_PointerBlocked = top && (top->Interactable || top->Kind == UIWidgetKind::Panel);
		if (top && top->Interactable)
			m_Hovered = top->ID;

		if (pointer && top)
		{
			// Only scroll views in the hit entity's ancestry receive the wheel; overlays cannot scroll a view behind them.
			Entity hit = top->ID.IsEntity() ? scene.GetEntityByUUID(top->ID.GetEntity()) : Entity();
			for (Entity current = hit; current; current = current.GetParent())
			{
				if (auto* component = current.TryGetComponent<UIScrollViewComponent>(); component && component->Interactable)
				{
					const auto rect = UILayout::GetRect(scene, current, GameUI::GetCanvasSize());
					if (rect)
					{
						const auto view = SanitizeUIScrollView(*component);
						glm::vec2 scroll = GameInput::GetMouseScroll();
						if (GameInput::IsKeyDown(KeyCode::LeftShift) || GameInput::IsKeyDown(KeyCode::RightShift))
							std::swap(scroll.x, scroll.y);
						component->Offset = glm::clamp(view.Offset - scroll * view.ScrollSpeed, glm::vec2(0), glm::max(view.ContentSize - rect->Size, glm::vec2(0)));
					}
					break;
				}
			}
		}

		if (m_Pressed)
		{
			// Without pointer input (it left the game view) the hold ends without a click.
			const auto pressed = std::ranges::find_if(widgets, [this](const Candidate& widget) { return widget.ID == m_Pressed->ID; });
			BS_CORE_ASSERT(pressed != widgets.end(), "Update keeps only holds on live widgets");
			if (!pointer)
			{
				m_Pressed.reset();
				return;
			}
			// A held slider follows the pointer when it moves (not every frame, so a script that overrides the value
			// in its callback is not overridden back).
			if (pressed->Kind == UIWidgetKind::Slider && *pointer != m_Pressed->DragPosition)
			{
				m_Pressed->DragPosition = *pointer;
				DragSlider(scene, *pressed, *pointer);
			}
			// The game's view of the button (GameInput), like the scripts': a button held when the game got the pointer
			// back is not the game's.
			if (GameInput::IsMouseButtonDown(MouseCode::Left))
				return;
			// Only a real release over the widget clicks it; a reset (the window lost focus) just lets go.
			const bool clicked = GameInput::IsMouseButtonReleased(MouseCode::Left) && pressed->Kind != UIWidgetKind::Slider && m_Hovered == pressed->ID;
			m_Pressed.reset();
			if (clicked)
			{
				if (top && top->Option)
					SelectDropdown(scene, *top, *top->Option);
				else
					Activate(scene, *pressed);
			}
			return;
		}

		if (!pointer || !GameInput::IsMouseButtonPressed(MouseCode::Left))
			return;
		// The mouse takes over from keyboard/gamepad navigation: Space and Enter go back to the game until the
		// focus is set again.
		SetFocus({});
		if (m_OpenDropdown != 0 && (!top || top->ID != UIWidgetID::ForEntity(m_OpenDropdown)))
			m_OpenDropdown = 0;
		if (!top || !top->Interactable || top->Kind == UIWidgetKind::Panel || top->Kind == UIWidgetKind::ScrollView)
			return;
		if (top->Kind == UIWidgetKind::TextInput || top->Kind == UIWidgetKind::Dropdown)
			FocusLive(top->ID);
		m_Pressed = Press{ top->ID, top->Kind == UIWidgetKind::Slider, *pointer };
		if (top->Kind == UIWidgetKind::Slider)
			DragSlider(scene, *top, *pointer);
	}

	void UIInteraction::UpdateNavigation(Scene& scene, const std::vector<Candidate>& widgets, float deltaTime)
	{
		if (!m_NavigationEnabled || !GameUI::IsNavigationEnabled())
		{
			m_HeldTime.fill(-1.0f);
			m_AcceptHeld = false;
			return;
		}

		std::array<bool, DirectionCount> fired = {};
		for (size_t direction = 0; direction < DirectionCount; direction++)
			fired[direction] = Repeat(static_cast<Direction>(direction), IsDirectionHeld(DirectionBindings[direction]), deltaTime);
		const bool left = fired[static_cast<size_t>(Direction::Left)];
		const bool right = fired[static_cast<size_t>(Direction::Right)];

		std::vector<const Candidate*> navigable;
		for (const Candidate& widget : widgets)
		{
			if (widget.IsNavigable())
				navigable.push_back(&widget);
		}
		// The focused widget's place in `navigable`; its size when nothing navigable is focused.
		const auto focusIndex = [&]() { return static_cast<size_t>(std::ranges::find_if(navigable, [this](const Candidate* widget) { return widget->ID == m_Focus.ID; }) - navigable.begin()); };

		if (GameInput::IsKeyPressed(KeyCode::Tab) && !navigable.empty())
		{
			const bool backwards = GameInput::IsKeyDown(KeyCode::LeftShift) || GameInput::IsKeyDown(KeyCode::RightShift);
			const size_t count = navigable.size();
			const size_t current = focusIndex();
			size_t next = 0;
			if (current == count)
				next = backwards ? count - 1 : 0;
			else
				next = (current + (backwards ? count - 1 : 1)) % count;
			FocusLive(navigable[next]->ID);
		}
		else if (std::ranges::any_of(fired, [](bool f) { return f; }))
		{
			const size_t current = focusIndex();
			if (current == navigable.size())
			{
				// The first move only shows where the focus is.
				if (!navigable.empty())
					FocusLive(navigable.front()->ID);
			}
			else if (navigable[current]->Kind == UIWidgetKind::TextInput && (left || right))
			{
				// Cursor movement is handled by UpdateText, never spatial navigation.
			}
			else if (navigable[current]->Kind == UIWidgetKind::Dropdown && m_OpenDropdown != 0)
			{
				const Entity owner = WidgetEntity(scene, navigable[current]->ID);
				const auto& dropdown = owner.GetComponent<UIDropdownComponent>();
				const auto count = UIWidgets::DropdownOptions(dropdown.Options).size();
				if (count != 0)
				{
					if (fired[0])
						m_DropdownHighlight = m_DropdownHighlight == 0 ? static_cast<uint32_t>(count - 1) : m_DropdownHighlight - 1;
					if (fired[1])
						m_DropdownHighlight = static_cast<uint32_t>((m_DropdownHighlight + 1) % count);
				}
			}
			else if (navigable[current]->Kind == UIWidgetKind::Slider && (navigable[current]->Vertical ? (fired[0] || fired[1]) : (left || right)))
			{
				StepSlider(scene, *navigable[current], navigable[current]->Vertical ? (fired[0] ? 1 : 0) - (fired[1] ? 1 : 0) : (right ? 1 : 0) - (left ? 1 : 0));
			}
			else
			{
				// One move per frame, in DirectionBindings order.
				for (size_t direction = 0; direction < DirectionCount; direction++)
				{
					if (!fired[direction])
						continue;
					const Candidate& from = *navigable[current];
					UUID explicitTarget = 0;
					if (from.ID.IsEntity())
					{
						if (const auto* navigation = WidgetEntity(scene, from.ID).TryGetComponent<UINavigationComponent>())
						{
							const UUID targets[] = { navigation->Up, navigation->Down, navigation->Left, navigation->Right };
							explicitTarget = targets[direction];
						}
					}
					if (explicitTarget != 0)
					{
						const auto target = std::ranges::find_if(navigable, [explicitTarget](const Candidate* c) { return c->ID == UIWidgetID::ForEntity(explicitTarget); });
						if (target != navigable.end())
							FocusLive((*target)->ID);
					}
					else if (const Candidate* neighbor = FindNeighbor(widgets, from, DirectionBindings[direction].Vector))
						FocusLive(neighbor->ID);
					break;
				}
			}
		}

		if (GameInput::IsKeyPressed(KeyCode::Escape))
			m_OpenDropdown = 0;
		if (m_OpenDropdown != 0 && m_Focus.ID != UIWidgetID::ForEntity(m_OpenDropdown))
			m_OpenDropdown = 0;
		const size_t current = focusIndex();
		const Candidate* focus = current < navigable.size() ? navigable[current] : nullptr;
		if (focus && focus->Kind != UIWidgetKind::Slider && IsAccept(&GameInput::IsKeyPressed, &GameInput::IsGamepadButtonPressed))
			Activate(scene, *focus);
		m_AcceptHeld = focus && focus->Kind != UIWidgetKind::Slider && IsAccept([](KeyCode key) { return GameInput::IsKeyDown(key); }, [](uint32_t index, GamepadButton button) { return GameInput::IsGamepadButtonDown(index, button); });
	}

	bool UIInteraction::Repeat(Direction direction, bool held, float deltaTime)
	{
		const auto index = static_cast<size_t>(direction);
		float& time = m_HeldTime[index];
		float& next = m_NextRepeat[index];
		if (!held)
		{
			time = -1.0f;
			return false;
		}
		if (time < 0.0f)
		{
			time = 0.0f;
			next = RepeatDelay;
			return true;
		}
		time += deltaTime;
		if (time < next)
			return false;
		// At most one repeat per frame: after a frame that skipped repeats, the next one is an interval away
		// rather than due at once.
		next += RepeatInterval;
		if (next <= time)
			next = time + RepeatInterval;
		return true;
	}

	void UIInteraction::Activate(Scene& scene, const Candidate& widget)
	{
		if (widget.ID.IsName())
		{
			m_ImmediateInputs[widget.ID.GetName()].Activated = true;
			return;
		}
		if (widget.Kind == UIWidgetKind::TextInput)
		{
			FocusLive(widget.ID);
			return;
		}
		if (widget.Kind == UIWidgetKind::Dropdown)
		{
			if (m_OpenDropdown == widget.ID.GetEntity())
				SelectDropdown(scene, widget, m_DropdownHighlight);
			else
			{
				const auto dropdown = SanitizeUIDropdown(WidgetEntity(scene, widget.ID).GetComponent<UIDropdownComponent>());
				if (!dropdown.Options.empty())
				{
					m_OpenDropdown = widget.ID.GetEntity();
					m_DropdownHighlight = dropdown.Selected;
				}
			}
			return;
		}
		UIWidgetValue value;
		if (widget.Kind == UIWidgetKind::Checkbox)
		{
			auto& checkbox = WidgetEntity(scene, widget.ID).GetComponent<UICheckboxComponent>();
			checkbox.Checked = !checkbox.Checked;
			value = checkbox.Checked;
		}
		if (!widget.Callback.empty())
			m_Events.push_back({ widget.ID.GetEntity(), widget.Target, widget.Callback, value });
	}

	void UIInteraction::DragSlider(Scene& scene, const Candidate& widget, const glm::vec2& pointer)
	{
		const glm::vec2 local = glm::vec2(glm::inverse(widget.Transform) * glm::vec3(pointer, 1.0f));
		const UIBox rect = widget.Vertical ? UIBox{ { widget.Rect.Min.y, widget.Rect.Min.x }, { widget.Rect.Size.y, widget.Rect.Size.x } } : widget.Rect;
		const float at = UIWidgets::SliderFractionAt(rect, widget.HandleWidth, widget.Vertical ? local.y : local.x);
		const float fraction = widget.Vertical ? 1.0f - at : at;
		if (widget.ID.IsName())
		{
			m_ImmediateInputs[widget.ID.GetName()].SliderFraction = fraction;
			return;
		}
		auto& component = WidgetEntity(scene, widget.ID).GetComponent<UISliderComponent>();
		const UISliderComponent slider = SanitizeUISlider(component);
		CommitSliderValue(widget, component, UIWidgets::SliderValueAtFraction(fraction, slider.Min, slider.Max, slider.Step));
	}

	void UIInteraction::StepSlider(Scene& scene, const Candidate& widget, int steps)
	{
		if (steps == 0)
			return;
		if (widget.ID.IsName())
		{
			m_ImmediateInputs[widget.ID.GetName()].SliderSteps += steps;
			return;
		}
		auto& component = WidgetEntity(scene, widget.ID).GetComponent<UISliderComponent>();
		const UISliderComponent slider = SanitizeUISlider(component);
		CommitSliderValue(widget, component, UIWidgets::StepSliderValue(slider.Value, steps, slider.Min, slider.Max, slider.Step));
	}

	void UIInteraction::CommitSliderValue(const Candidate& widget, UISliderComponent& slider, float value)
	{
		if (value == slider.Value)
			return;
		slider.Value = value;
		if (!widget.Callback.empty())
			m_Events.push_back({ widget.ID.GetEntity(), widget.Target, widget.Callback, value });
	}

	void UIInteraction::SelectDropdown(Scene& scene, const Candidate& widget, uint32_t index)
	{
		m_OpenDropdown = 0;
		FocusLive(widget.ID);
		auto& dropdown = WidgetEntity(scene, widget.ID).GetComponent<UIDropdownComponent>();
		if (index >= UIWidgets::DropdownOptions(dropdown.Options).size() || index == dropdown.Selected)
			return;
		dropdown.Selected = index;
		if (!widget.Callback.empty())
			m_Events.push_back({ widget.ID.GetEntity(), widget.Target, widget.Callback, static_cast<float>(index) });
	}

	void UIInteraction::UpdateText(Scene& scene, const std::vector<Candidate>& widgets)
	{
		const auto focus = std::ranges::find_if(widgets, [this](const Candidate& widget) { return widget.ID == m_Focus.ID && widget.Kind == UIWidgetKind::TextInput && widget.IsNavigable(); });
		if (focus == widgets.end())
		{
			m_TextFocus = {};
			return;
		}
		auto& component = WidgetEntity(scene, focus->ID).GetComponent<UITextInputComponent>();
		const auto repaired = SanitizeUITextInput(component);
		if (m_TextFocus != focus->ID)
		{
			m_TextFocus = focus->ID;
			m_TextCursor = repaired.Text.size();
		}
		m_TextCursor = std::min(m_TextCursor, repaired.Text.size());
		// External text changes may put the old byte offset inside a multibyte code point.
		while (m_TextCursor > 0 && m_TextCursor < repaired.Text.size() && (static_cast<unsigned char>(repaired.Text[m_TextCursor]) & 0xc0U) == 0x80U)
			m_TextCursor--;
		if (!m_NavigationEnabled || !GameUI::IsNavigationEnabled() || !Input::HasGameFocus())
			return;
		std::string text = repaired.Text;
		if (GameInput::IsKeyPressed(KeyCode::Left))
			m_TextCursor = UIWidgets::PreviousCharacter(text, m_TextCursor);
		if (GameInput::IsKeyPressed(KeyCode::Right))
			m_TextCursor = UIWidgets::NextCharacter(text, m_TextCursor);
		if (GameInput::IsKeyPressed(KeyCode::Home))
			m_TextCursor = 0;
		if (GameInput::IsKeyPressed(KeyCode::End))
			m_TextCursor = text.size();
		if (repaired.ReadOnly)
			return;
		if (GameInput::IsKeyPressed(KeyCode::Backspace) && m_TextCursor != 0)
		{
			const size_t previous = UIWidgets::PreviousCharacter(text, m_TextCursor);
			text.erase(previous, m_TextCursor - previous);
			m_TextCursor = previous;
		}
		if (GameInput::IsKeyPressed(KeyCode::Delete))
			text.erase(m_TextCursor, UIWidgets::NextCharacter(text, m_TextCursor) - m_TextCursor);
		const std::string typed = GameInput::GetTextInput();
		if (UIWidgets::IsSingleLineUTF8(typed))
		{
			size_t length = UIWidgets::TextLength(text);
			for (size_t i = 0; i < typed.size() && length < repaired.MaxLength; length++)
			{
				const size_t next = UIWidgets::NextCharacter(typed, i);
				text.insert(m_TextCursor, typed.substr(i, next - i));
				m_TextCursor += next - i;
				i = next;
			}
		}
		if (text != component.Text)
		{
			component.Text = std::move(text);
			if (!focus->Callback.empty())
				m_Events.push_back({ focus->ID.GetEntity(), focus->Target, focus->Callback, component.Text });
		}
	}

}
