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
		bool IsAccept(bool (*key)(KeyCode), bool (*button)(uint32_t, GamepadButton))
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
			glm::vec2 Vector;
		};

		// In Direction order. Vertical first: one move per frame, and menus are mostly columns.
		constexpr DirectionBinding DirectionBindings[] = {
			{ KeyCode::Up, GamepadButton::DPadUp, GamepadAxis::LeftY, -1.0f, { 0.0f, -1.0f } },
			{ KeyCode::Down, GamepadButton::DPadDown, GamepadAxis::LeftY, 1.0f, { 0.0f, 1.0f } },
			{ KeyCode::Left, GamepadButton::DPadLeft, GamepadAxis::LeftX, -1.0f, { -1.0f, 0.0f } },
			{ KeyCode::Right, GamepadButton::DPadRight, GamepadAxis::LeftX, 1.0f, { 1.0f, 0.0f } },
		};

		// The direction is held on the keyboard, a D-pad or a left stick. A stick counts along its dominant axis
		// only: a diagonal push moves one way, not two.
		bool IsDirectionHeld(const DirectionBinding& binding)
		{
			return Input::IsKeyDown(binding.Key) || AnyGamepad([&binding](uint32_t index) {
					   if (Input::IsGamepadButtonDown(index, binding.DPad))
						   return true;
					   const float along = Input::GetGamepadAxis(index, binding.Axis) * binding.Sign;
					   const GamepadAxis other = binding.Axis == GamepadAxis::LeftX ? GamepadAxis::LeftY : GamepadAxis::LeftX;
					   return along >= UIInteraction::StickThreshold && along > std::abs(Input::GetGamepadAxis(index, other));
				   });
		}

		// The widget nearest `from` in `direction` (a unit axis): by center distance along it plus twice the sideways
		// offset; ties go to the earlier widget; nullptr when none lies that way.
		template<typename Candidate>
		const Candidate* FindNeighbor(const std::vector<Candidate>& widgets, const Candidate& from, const glm::vec2& direction)
		{
			const glm::vec2 origin = from.Rect.Center();
			const Candidate* best = nullptr;
			float bestScore = std::numeric_limits<float>::max();
			for (const Candidate& widget : widgets)
			{
				if (!widget.Interactable || widget.ID == from.ID)
					continue;
				const glm::vec2 offset = widget.Rect.Center() - origin;
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

		// Queues the widget's callback on its target (itself when Target is 0); a widget whose callback name is
		// empty queues nothing.
		void QueueEvent(std::vector<UIWidgetEvent>& events, Entity widget, UUID target, const std::string& method, UIWidgetValue value)
		{
			if (method.empty())
				return;
			const UUID self = widget.GetUUID();
			events.push_back({ self, target != 0 ? target : self, method, value });
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

		const std::vector<Candidate> widgets = CollectWidgets(scene);
		// A held or focused widget that is gone, hidden or disabled lets go.
		const auto isLive = [&widgets](const UIWidgetID& id) {
			return std::ranges::any_of(widgets, [&id](const Candidate& widget) { return widget.Interactable && widget.ID == id; });
		};
		if (m_Focus.IsValid())
		{
			if (isLive(m_Focus))
			{
				m_FocusSeen = true;
				m_FocusPending = false;
			}
			else if (m_FocusPending)
			{
				m_FocusPending = false;
			}
			else
			{
				if (!m_FocusSeen)
					BS_CORE_WARN("UI: the focus was set to '{}', which is not a visible, interactable widget; dropped it", m_Focus.IsName() ? m_Focus.GetName() : std::to_string(static_cast<uint64_t>(m_Focus.GetEntity())));
				m_Focus = {};
			}
		}
		if (m_Pressed && !isLive(m_Pressed->ID))
			m_Pressed.reset();

		UpdatePointer(scene, widgets);
		UpdateNavigation(scene, widgets, deltaTime);
	}

	std::vector<UIWidgetEvent> UIInteraction::TakeEvents()
	{
		return std::exchange(m_Events, {});
	}

	void UIInteraction::OnSceneLoaded()
	{
		m_Hovered = {};
		m_Pressed.reset();
		m_Focus = {};
		m_AcceptHeld = false;
		m_LastImmediate.clear();
		m_ImmediateInputs.clear();
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
		if (held || (id == m_Focus && m_AcceptHeld))
			return UIWidgetState::Pressed;
		if (id == m_Hovered || id == m_Focus)
			return UIWidgetState::Highlighted;
		return UIWidgetState::Normal;
	}

	void UIInteraction::SetFocus(UIWidgetID id)
	{
		m_Focus = std::move(id);
		m_FocusPending = m_Focus.IsValid();
		m_FocusSeen = false;
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
		if (m_LastImmediate.empty() && !UILayout::HasWidgets(scene))
			return widgets;
		const glm::vec2 canvas = GameUI::GetCanvasSize();
		for (const UILayoutItem& item : UILayout::Layout(scene, canvas))
		{
			const std::optional<UIWidgetInfo> info = UILayout::GetWidget(item.Owner);
			if (!info)
				continue;
			Candidate& candidate = widgets.emplace_back(Candidate{ UIWidgetID::ForEntity(item.Owner.GetUUID()), info->Kind, item.Rect, info->Interactable });
			if (info->Kind == UIWidgetKind::Slider)
				candidate.HandleWidth = SanitizeUISlider(item.Owner.GetComponent<UISliderComponent>()).HandleWidth;
		}
		for (const UIImmediateWidget& widget : m_LastImmediate)
			widgets.push_back({ UIWidgetID::ForName(widget.ID), widget.Kind, { canvas * widget.Anchor + widget.Offset, widget.Size }, widget.Interactable, widget.HandleWidth });
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
				if (widget.Rect.Contains(*pointer))
				{
					top = &widget;
					break;
				}
			}
		}
		if (top && top->Interactable)
			m_Hovered = top->ID;

		if (m_Pressed)
		{
			// Update keeps only holds on live widgets. Without pointer input (it left the game view) the hold ends
			// without a click.
			const auto pressed = std::ranges::find_if(widgets, [this](const Candidate& widget) { return widget.ID == m_Pressed->ID; });
			BS_CORE_ASSERT(pressed != widgets.end(), "Update keeps only holds on live widgets");
			if (!pointer)
			{
				m_Pressed.reset();
				return;
			}
			// A held slider follows the pointer when it moves (not every frame, so a script that overrides the value
			// in its callback is not overridden back).
			if (pressed->Kind == UIWidgetKind::Slider && pointer->x != m_Pressed->DragX)
			{
				m_Pressed->DragX = pointer->x;
				DragSlider(scene, *pressed, pointer->x);
			}
			if (Input::IsMouseButtonDown(MouseCode::Left))
				return;
			// Only a real release over the widget clicks it; a reset (the window lost focus) just lets go.
			const bool clicked = Input::IsMouseButtonReleased(MouseCode::Left) && pressed->Kind != UIWidgetKind::Slider && m_Hovered == pressed->ID;
			m_Pressed.reset();
			if (clicked)
				Activate(scene, *pressed);
			return;
		}

		if (!pointer || !Input::IsMouseButtonPressed(MouseCode::Left))
			return;
		// The mouse takes over from keyboard/gamepad navigation: Space and Enter go back to the game until the
		// focus is set again.
		SetFocus({});
		if (!top || !top->Interactable)
			return;
		m_Pressed = Press{ top->ID, top->Kind == UIWidgetKind::Slider, pointer->x };
		if (top->Kind == UIWidgetKind::Slider)
			DragSlider(scene, *top, pointer->x);
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
		const auto isFired = [&fired](Direction direction) { return fired[static_cast<size_t>(direction)]; };

		std::vector<const Candidate*> navigable;
		for (const Candidate& widget : widgets)
		{
			if (widget.Interactable)
				navigable.push_back(&widget);
		}
		const auto findFocus = [&]() { return std::ranges::find_if(navigable, [this](const Candidate* widget) { return widget->ID == m_Focus; }); };

		if (Input::IsKeyPressed(KeyCode::Tab) && !navigable.empty())
		{
			const bool backwards = Input::IsKeyDown(KeyCode::LeftShift) || Input::IsKeyDown(KeyCode::RightShift);
			const auto it = findFocus();
			const size_t count = navigable.size();
			size_t next = 0;
			if (it == navigable.end())
				next = backwards ? count - 1 : 0;
			else
				next = (static_cast<size_t>(it - navigable.begin()) + (backwards ? count - 1 : 1)) % count;
			SetFocus(navigable[next]->ID);
		}
		else if (std::ranges::any_of(fired, [](bool f) { return f; }))
		{
			const auto it = findFocus();
			if (it == navigable.end())
			{
				// The first move only shows where the focus is.
				if (!navigable.empty())
					SetFocus(navigable.front()->ID);
			}
			else if ((*it)->Kind == UIWidgetKind::Slider && (isFired(Direction::Left) || isFired(Direction::Right)))
			{
				StepSlider(scene, **it, (isFired(Direction::Right) ? 1 : 0) - (isFired(Direction::Left) ? 1 : 0));
			}
			else
			{
				// One move per frame, in DirectionBindings order.
				for (size_t direction = 0; direction < DirectionCount; direction++)
				{
					if (!fired[direction])
						continue;
					if (const Candidate* neighbor = FindNeighbor(widgets, **it, DirectionBindings[direction].Vector))
						SetFocus(neighbor->ID);
					break;
				}
			}
		}

		const auto it = findFocus();
		const Candidate* focus = it != navigable.end() ? *it : nullptr;
		if (focus && focus->Kind != UIWidgetKind::Slider && IsAccept(&Input::IsKeyPressed, &Input::IsGamepadButtonPressed))
			Activate(scene, *focus);
		m_AcceptHeld = focus && focus->Kind != UIWidgetKind::Slider && IsAccept(&Input::IsKeyDown, &Input::IsGamepadButtonDown);
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
		Entity entity = scene.GetEntityByUUID(widget.ID.GetEntity());
		BS_CORE_ASSERT(entity, "candidates are live entities");
		if (widget.Kind == UIWidgetKind::Button)
		{
			const UIButtonComponent button = SanitizeUIButton(entity.GetComponent<UIButtonComponent>());
			QueueEvent(m_Events, entity, button.Target, button.OnClick, std::monostate{});
		}
		else if (widget.Kind == UIWidgetKind::Checkbox)
		{
			auto& checkbox = entity.GetComponent<UICheckboxComponent>();
			checkbox.Checked = !checkbox.Checked;
			QueueEvent(m_Events, entity, checkbox.Target, SanitizeUICheckbox(checkbox).OnValueChanged, checkbox.Checked);
		}
	}

	void UIInteraction::DragSlider(Scene& scene, const Candidate& widget, float pointerX)
	{
		const float fraction = UIWidgets::SliderFractionAt(widget.Rect, widget.HandleWidth, pointerX);
		if (widget.ID.IsName())
		{
			m_ImmediateInputs[widget.ID.GetName()].SliderFraction = fraction;
			return;
		}
		Entity entity = scene.GetEntityByUUID(widget.ID.GetEntity());
		BS_CORE_ASSERT(entity, "candidates are live entities");
		auto& component = entity.GetComponent<UISliderComponent>();
		const UISliderComponent slider = SanitizeUISlider(component);
		CommitSliderValue(entity, component, UIWidgets::SliderValueAtFraction(fraction, slider.Min, slider.Max, slider.Step));
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
		Entity entity = scene.GetEntityByUUID(widget.ID.GetEntity());
		BS_CORE_ASSERT(entity, "candidates are live entities");
		auto& component = entity.GetComponent<UISliderComponent>();
		const UISliderComponent slider = SanitizeUISlider(component);
		CommitSliderValue(entity, component, UIWidgets::StepSliderValue(slider.Value, steps, slider.Min, slider.Max, slider.Step));
	}

	void UIInteraction::CommitSliderValue(Entity entity, UISliderComponent& slider, float value)
	{
		if (value == slider.Value)
			return;
		slider.Value = value;
		QueueEvent(m_Events, entity, slider.Target, SanitizeUISlider(slider).OnValueChanged, value);
	}

}
