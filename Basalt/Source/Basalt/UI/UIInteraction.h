#pragma once

#include "Basalt/Core/UUID.h"
#include "Basalt/UI/UIGeometry.h"
#include "Basalt/UI/UIWidgets.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace Basalt {

	class Scene;

	// Identifies a widget across frames: a widget component by its entity, an immediate-mode widget (UI.Button,
	// ...) by its id string.
	struct UIWidgetID
	{
		UUID Entity = 0;
		std::string Name;

		static UIWidgetID ForEntity(UUID entity) { return { entity, {} }; }
		static UIWidgetID ForName(std::string name) { return { 0, std::move(name) }; }

		bool IsValid() const { return Entity != 0 || !Name.empty(); }
		bool operator==(const UIWidgetID&) const = default;
	};

	// A user interaction with a widget component, for its target's script: Target:Method(widget[, value]).
	// Value is empty for a click, a number for a slider and a boolean for a checkbox.
	struct UIWidgetEvent
	{
		UUID Widget = 0;
		// The component's Target, or the widget itself when that is 0.
		UUID Target = 0;
		std::string Method;
		std::variant<std::monostate, float, bool> Value;
	};

	// An immediate-mode widget a script drew this frame; it takes input from the next frame on. Placed like a
	// UIDrawCommand: its top-left is Anchor * canvas size + Offset, so it follows the canvas when it resizes.
	struct UIImmediateWidget
	{
		std::string ID;
		UIWidgetKind Kind = UIWidgetKind::Button;
		glm::vec2 Anchor = { 0.0f, 0.0f };
		glm::vec2 Offset = { 0.0f, 0.0f };
		glm::vec2 Size = { 0.0f, 0.0f };
		bool Interactable = true;
		// Sliders only.
		float HandleWidth = 0.0f;
	};

	// What the user did to an immediate-mode widget this frame.
	struct UIImmediateInput
	{
		UIWidgetState State = UIWidgetState::Normal;
		// A button was clicked, or a checkbox toggled.
		bool Activated = false;
		// A slider was dragged to this fraction of its range.
		std::optional<float> SliderFraction;
		// A slider was moved this many navigation steps (positive = right) while focused.
		int SliderSteps = 0;
	};

	// The pointer, keyboard and gamepad side of the game UI while a scene plays: which widget is hovered, held and
	// focused, and what the user did to it. Created by Scene::OnRuntimeStart in Play mode (editing and simulating
	// draw every widget in its normal state).
	//
	// Every frame Update hit-tests the visible widget components (UILayout) and the immediate-mode widgets drawn
	// the frame before, topmost first: immediate widgets over components, later ones over earlier ones. Only
	// widgets take pointer input; a plain UIImage does not block it. Pressing the left mouse button on a widget
	// holds and focuses it; releasing it over the same widget clicks a button or toggles a checkbox, and a held
	// slider follows the pointer. Arrows, the D-pad and the left stick (with repeat after RepeatDelay, every
	// RepeatInterval) move the focus to the nearest widget in that direction, Tab/Shift+Tab through all of them in
	// draw order; Enter, Space or gamepad A activate the focused widget, and Left/Right move a focused slider.
	// Widget components change their own Value/Checked and queue a UIWidgetEvent; immediate widgets read their
	// UIImmediateInput when the script draws them again.
	class UIInteraction
	{
	public:
		static constexpr float RepeatDelay = 0.4f;
		static constexpr float RepeatInterval = 0.1f;
		// How far a stick must be pushed to navigate.
		static constexpr float StickThreshold = 0.5f;
		// Immediate-mode widgets per frame; more is almost certainly a loop that never ends.
		static constexpr size_t MaxImmediateWidgets = 4096;

		// Processes this frame's input (deltaTime drives the navigation repeat). Call once per played frame, before
		// the scripts update.
		void Update(Scene& scene, float deltaTime);
		// The widget component events of the last Update, in the order they happened.
		std::vector<UIWidgetEvent> TakeEvents();

		enum class SubmitResult
		{
			Accepted,
			// Another widget already used the id this frame.
			DuplicateID,
			// MaxImmediateWidgets were already drawn this frame.
			TooMany
		};

		// Registers an immediate-mode widget drawn this frame and returns, in `outInput`, what the user did to it
		// in the last Update.
		[[nodiscard]] SubmitResult SubmitImmediate(UIImmediateWidget widget, UIImmediateInput& outInput);

		// How the widget looks this frame.
		UIWidgetState GetState(const UIWidgetID& id, bool interactable) const;

		// The focused widget (invalid when none). A focus that is not a visible, interactable widget is dropped
		// in the next Update.
		const UIWidgetID& GetFocus() const { return m_Focus; }
		void SetFocus(UIWidgetID id);
		// Whether the pointer was over an interactable widget in the last Update (so a click there is the UI's,
		// not the game's).
		bool IsPointerOverWidget() const { return m_Hovered.IsValid(); }

	private:
		struct Candidate
		{
			UIWidgetID ID;
			UIWidgetKind Kind = UIWidgetKind::Button;
			UIBox Rect;
			bool Interactable = true;
			float HandleWidth = 0.0f;
		};

		enum Direction
		{
			Up = 0,
			Down,
			Left,
			Right,
			DirectionCount
		};

		std::vector<Candidate> CollectWidgets(Scene& scene) const;
		void UpdatePointer(Scene& scene, const std::vector<Candidate>& widgets);
		void UpdateNavigation(Scene& scene, const std::vector<Candidate>& widgets, float deltaTime);
		// Whether a held direction fires this frame (when it starts, then repeating).
		bool Repeat(Direction direction, bool held, float deltaTime);

		void Activate(Scene& scene, const Candidate& widget);
		void SetSliderFraction(Scene& scene, const Candidate& widget, float fraction);
		void StepSlider(Scene& scene, const Candidate& widget, int steps);

	private:
		UIWidgetID m_Hovered;
		UIWidgetID m_Pressed;
		UIWidgetID m_Focus;
		// The held widget is a slider, which stays pressed while the pointer drags it off its rect.
		bool m_PressedIsDrag = false;
		// The accept button is held on the focused widget.
		bool m_AcceptHeld = false;
		// Seconds each direction has been held (negative: not held), and when it repeats next.
		std::array<float, DirectionCount> m_HeldTime = { -1.0f, -1.0f, -1.0f, -1.0f };
		std::array<float, DirectionCount> m_NextRepeat = {};

		std::vector<UIImmediateWidget> m_Immediate;
		std::vector<UIImmediateWidget> m_LastImmediate;
		std::unordered_set<std::string> m_ImmediateIDs;
		std::unordered_map<std::string, UIImmediateInput> m_ImmediateInputs;

		std::vector<UIWidgetEvent> m_Events;
	};

}
