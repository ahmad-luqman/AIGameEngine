#include "Basalt/UI/UIInteraction.h"

#include "Basalt/Core/Input.h"
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

		bool IsAcceptPressed()
		{
			return Input::IsKeyPressed(KeyCode::Enter) || Input::IsKeyPressed(KeyCode::KPEnter) || Input::IsKeyPressed(KeyCode::Space) || AnyGamepad([](uint32_t index) { return Input::IsGamepadButtonPressed(index, GamepadButton::A); });
		}

		bool IsAcceptDown()
		{
			return Input::IsKeyDown(KeyCode::Enter) || Input::IsKeyDown(KeyCode::KPEnter) || Input::IsKeyDown(KeyCode::Space) || AnyGamepad([](uint32_t index) { return Input::IsGamepadButtonDown(index, GamepadButton::A); });
		}

		// The navigation direction is held on the keyboard, a D-pad or a left stick.
		bool IsDirectionHeld(KeyCode key, GamepadButton dpad, GamepadAxis axis, float sign)
		{
			return Input::IsKeyDown(key) || AnyGamepad([&](uint32_t index) { return Input::IsGamepadButtonDown(index, dpad) || Input::GetGamepadAxis(index, axis) * sign >= UIInteraction::StickThreshold; });
		}

		// Moves the focus from `from` to the widget whose center is nearest in `direction` (a unit axis),
		// penalizing sideways offset twice as much as distance along it; ties go to the earlier widget.
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

		// The value an event carries for a component's callback, or nothing when it has no callback.
		template<typename T>
		void QueueEvent(std::vector<UIWidgetEvent>& events, Entity entity, const T& widget, const std::string& method, std::variant<std::monostate, float, bool> value)
		{
			if (method.empty())
				return;
			const UUID self = entity.GetUUID();
			events.push_back({ self, widget.Target != 0 ? widget.Target : self, method, value });
		}

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
		if (m_Focus.IsValid() && !isLive(m_Focus))
			m_Focus = {};
		if (m_Pressed.IsValid() && !isLive(m_Pressed))
			m_Pressed = {};

		UpdatePointer(scene, widgets);
		UpdateNavigation(scene, widgets, deltaTime);
	}

	std::vector<UIWidgetEvent> UIInteraction::TakeEvents()
	{
		return std::exchange(m_Events, {});
	}

	UIInteraction::SubmitResult UIInteraction::SubmitImmediate(UIImmediateWidget widget, UIImmediateInput& outInput)
	{
		if (m_ImmediateIDs.contains(widget.ID))
			return SubmitResult::DuplicateID;
		if (m_Immediate.size() >= MaxImmediateWidgets)
			return SubmitResult::TooMany;
		const UIWidgetID id = UIWidgetID::ForName(widget.ID);
		auto it = m_ImmediateInputs.find(widget.ID);
		outInput = it != m_ImmediateInputs.end() ? it->second : UIImmediateInput{};
		outInput.State = GetState(id, widget.Interactable);
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
		if ((id == m_Pressed && (m_Hovered == id || m_PressedIsDrag)) || (id == m_Focus && m_AcceptHeld))
			return UIWidgetState::Pressed;
		if (id == m_Hovered || id == m_Focus)
			return UIWidgetState::Highlighted;
		return UIWidgetState::Normal;
	}

	void UIInteraction::SetFocus(UIWidgetID id)
	{
		m_Focus = std::move(id);
		m_AcceptHeld = false;
	}

	std::vector<UIInteraction::Candidate> UIInteraction::CollectWidgets(Scene& scene) const
	{
		std::vector<Candidate> widgets;
		const glm::vec2 canvas = GameUI::GetCanvasSize();
		for (const UILayoutItem& item : UILayout::Layout(scene, canvas))
		{
			Candidate candidate{ UIWidgetID::ForEntity(item.Owner.GetUUID()), UIWidgetKind::Button, item.Rect };
			// An entity with several widget components acts as the first of these (the layout reports it).
			if (const auto* button = item.Owner.TryGetComponent<UIButtonComponent>())
			{
				candidate.Interactable = button->Interactable;
			}
			else if (const auto* slider = item.Owner.TryGetComponent<UISliderComponent>())
			{
				candidate.Kind = UIWidgetKind::Slider;
				candidate.Interactable = slider->Interactable;
				candidate.HandleWidth = SanitizeUISlider(*slider).HandleWidth;
			}
			else if (const auto* checkbox = item.Owner.TryGetComponent<UICheckboxComponent>())
			{
				candidate.Kind = UIWidgetKind::Checkbox;
				candidate.Interactable = checkbox->Interactable;
			}
			else
			{
				continue;
			}
			widgets.push_back(std::move(candidate));
		}
		for (const UIImmediateWidget& widget : m_LastImmediate)
		{
			const UIBox rect{ canvas * widget.Anchor + widget.Offset, widget.Size };
			widgets.push_back({ UIWidgetID::ForName(widget.ID), widget.Kind, rect, widget.Interactable, widget.HandleWidth });
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
				if (widget.Rect.Contains(*pointer))
				{
					top = &widget;
					break;
				}
			}
		}
		if (top && top->Interactable)
			m_Hovered = top->ID;

		if (m_Pressed.IsValid())
		{
			const auto pressed = std::ranges::find_if(widgets, [this](const Candidate& widget) { return widget.ID == m_Pressed; });
			// Without pointer input (it left the game view) the hold ends without a click.
			if (!pointer || pressed == widgets.end())
			{
				m_Pressed = {};
				return;
			}
			if (pressed->Kind == UIWidgetKind::Slider)
				SetSliderFraction(scene, *pressed, UIWidgets::SliderFractionAt(pressed->Rect, pressed->HandleWidth, pointer->x));
			if (!Input::IsMouseButtonDown(MouseCode::Left))
			{
				m_Pressed = {};
				if (pressed->Kind != UIWidgetKind::Slider && m_Hovered == pressed->ID)
					Activate(scene, *pressed);
			}
			return;
		}

		if (!pointer || !top || !top->Interactable || !Input::IsMouseButtonPressed(MouseCode::Left))
			return;
		m_Pressed = top->ID;
		m_PressedIsDrag = top->Kind == UIWidgetKind::Slider;
		m_Focus = top->ID;
		if (top->Kind == UIWidgetKind::Slider)
			SetSliderFraction(scene, *top, UIWidgets::SliderFractionAt(top->Rect, top->HandleWidth, pointer->x));
	}

	void UIInteraction::UpdateNavigation(Scene& scene, const std::vector<Candidate>& widgets, float deltaTime)
	{
		if (!GameUI::IsNavigationEnabled())
		{
			m_HeldTime.fill(-1.0f);
			m_AcceptHeld = false;
			return;
		}

		std::array<bool, DirectionCount> fired = {};
		fired[Up] = Repeat(Up, IsDirectionHeld(KeyCode::Up, GamepadButton::DPadUp, GamepadAxis::LeftY, -1.0f), deltaTime);
		fired[Down] = Repeat(Down, IsDirectionHeld(KeyCode::Down, GamepadButton::DPadDown, GamepadAxis::LeftY, 1.0f), deltaTime);
		fired[Left] = Repeat(Left, IsDirectionHeld(KeyCode::Left, GamepadButton::DPadLeft, GamepadAxis::LeftX, -1.0f), deltaTime);
		fired[Right] = Repeat(Right, IsDirectionHeld(KeyCode::Right, GamepadButton::DPadRight, GamepadAxis::LeftX, 1.0f), deltaTime);
		const bool anyDirection = std::ranges::any_of(fired, [](bool f) { return f; });

		std::vector<const Candidate*> navigable;
		for (const Candidate& widget : widgets)
		{
			if (widget.Interactable)
				navigable.push_back(&widget);
		}
		const auto findFocus = [&]() -> const Candidate* {
			const auto it = std::ranges::find_if(navigable, [this](const Candidate* widget) { return widget->ID == m_Focus; });
			return it != navigable.end() ? *it : nullptr;
		};

		if (Input::IsKeyPressed(KeyCode::Tab) && !navigable.empty())
		{
			const bool backwards = Input::IsKeyDown(KeyCode::LeftShift) || Input::IsKeyDown(KeyCode::RightShift);
			const auto it = std::ranges::find_if(navigable, [this](const Candidate* widget) { return widget->ID == m_Focus; });
			const size_t count = navigable.size();
			size_t next = 0;
			if (it == navigable.end())
				next = backwards ? count - 1 : 0;
			else
				next = (static_cast<size_t>(it - navigable.begin()) + (backwards ? count - 1 : 1)) % count;
			SetFocus(navigable[next]->ID);
		}
		else if (anyDirection)
		{
			const Candidate* focus = findFocus();
			if (!focus)
			{
				// The first move only shows where the focus is.
				if (!navigable.empty())
					SetFocus(navigable.front()->ID);
			}
			else if (focus->Kind == UIWidgetKind::Slider && (fired[Left] || fired[Right]))
			{
				StepSlider(scene, *focus, (fired[Right] ? 1 : 0) - (fired[Left] ? 1 : 0));
			}
			else
			{
				// One move per frame; vertical first (menus are mostly columns).
				static constexpr glm::vec2 Directions[DirectionCount] = { { 0.0f, -1.0f }, { 0.0f, 1.0f }, { -1.0f, 0.0f }, { 1.0f, 0.0f } };
				for (int direction = 0; direction < DirectionCount; direction++)
				{
					if (!fired[direction])
						continue;
					if (const Candidate* neighbor = FindNeighbor(widgets, *focus, Directions[direction]))
						SetFocus(neighbor->ID);
					break;
				}
			}
		}

		const Candidate* focus = findFocus();
		if (focus && IsAcceptPressed() && focus->Kind != UIWidgetKind::Slider)
			Activate(scene, *focus);
		m_AcceptHeld = focus && focus->Kind != UIWidgetKind::Slider && IsAcceptDown();
	}

	bool UIInteraction::Repeat(Direction direction, bool held, float deltaTime)
	{
		float& time = m_HeldTime[direction];
		if (!held)
		{
			time = -1.0f;
			return false;
		}
		if (time < 0.0f)
		{
			time = 0.0f;
			m_NextRepeat[direction] = RepeatDelay;
			return true;
		}
		time += deltaTime;
		if (time < m_NextRepeat[direction])
			return false;
		// At most one repeat per frame: after a frame that skipped repeats, the next one is an interval away
		// rather than due at once.
		m_NextRepeat[direction] += RepeatInterval;
		if (m_NextRepeat[direction] <= time)
			m_NextRepeat[direction] = time + RepeatInterval;
		return true;
	}

	void UIInteraction::Activate(Scene& scene, const Candidate& widget)
	{
		if (!widget.ID.Name.empty())
		{
			m_ImmediateInputs[widget.ID.Name].Activated = true;
			return;
		}
		Entity entity = scene.GetEntityByUUID(widget.ID.Entity);
		if (!entity)
			return;
		if (widget.Kind == UIWidgetKind::Button)
		{
			if (const auto* button = entity.TryGetComponent<UIButtonComponent>())
				QueueEvent(m_Events, entity, *button, button->OnClick, std::monostate{});
		}
		else if (auto* checkbox = entity.TryGetComponent<UICheckboxComponent>())
		{
			checkbox->Checked = !checkbox->Checked;
			QueueEvent(m_Events, entity, *checkbox, checkbox->OnValueChanged, checkbox->Checked);
		}
	}

	void UIInteraction::SetSliderFraction(Scene& scene, const Candidate& widget, float fraction)
	{
		if (!widget.ID.Name.empty())
		{
			m_ImmediateInputs[widget.ID.Name].SliderFraction = fraction;
			return;
		}
		Entity entity = scene.GetEntityByUUID(widget.ID.Entity);
		auto* component = entity ? entity.TryGetComponent<UISliderComponent>() : nullptr;
		if (!component)
			return;
		const UISliderComponent slider = SanitizeUISlider(*component);
		const float value = UIWidgets::SnapSliderValue(slider.Min + fraction * (slider.Max - slider.Min), slider.Min, slider.Max, slider.Step);
		if (value == component->Value)
			return;
		component->Value = value;
		QueueEvent(m_Events, entity, *component, component->OnValueChanged, value);
	}

	void UIInteraction::StepSlider(Scene& scene, const Candidate& widget, int steps)
	{
		if (steps == 0)
			return;
		if (!widget.ID.Name.empty())
		{
			m_ImmediateInputs[widget.ID.Name].SliderSteps += steps;
			return;
		}
		Entity entity = scene.GetEntityByUUID(widget.ID.Entity);
		auto* component = entity ? entity.TryGetComponent<UISliderComponent>() : nullptr;
		if (!component)
			return;
		const UISliderComponent slider = SanitizeUISlider(*component);
		const float step = UIWidgets::SliderNavigationStep(slider.Min, slider.Max, slider.Step);
		const float value = UIWidgets::SnapSliderValue(slider.Value + static_cast<float>(steps) * step, slider.Min, slider.Max, slider.Step);
		if (value == component->Value)
			return;
		component->Value = value;
		QueueEvent(m_Events, entity, *component, component->OnValueChanged, value);
	}

}
