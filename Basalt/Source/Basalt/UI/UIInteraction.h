#pragma once

#include "Basalt/Core/UUID.h"
#include "Basalt/UI/UIGeometry.h"
#include "Basalt/UI/UIWidgetEvent.h"
#include "Basalt/UI/UIWidgets.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Basalt {

	class Scene;

	// Identifies a widget across frames: a widget component by its entity, or an immediate-mode widget (UI.Button,
	// ...) by its id string. Holds one of the two, or nothing (the default: no widget).
	class UIWidgetID
	{
	public:
		UIWidgetID() = default;
		// `entity` is not 0.
		static UIWidgetID ForEntity(UUID entity);
		// `name` is not empty.
		static UIWidgetID ForName(std::string name);

		bool IsValid() const { return IsEntity() || IsName(); }
		bool IsEntity() const { return m_Entity != 0; }
		bool IsName() const { return !m_Name.empty(); }
		// 0 unless IsEntity().
		UUID GetEntity() const { return m_Entity; }
		// Empty unless IsName().
		const std::string& GetName() const { return m_Name; }

		bool operator==(const UIWidgetID&) const = default;

	private:
		UUID m_Entity = 0;
		std::string m_Name;
	};

	// An immediate-mode widget a script drew this frame; it takes input from the next frame on. Placed like a
	// UIDrawCommand: its top-left is Anchor * canvas size + Offset, so it follows the canvas when it resizes.
	struct UIImmediateWidget
	{
		// Not empty.
		std::string ID;
		UIWidgetKind Kind = UIWidgetKind::Button;
		glm::vec2 Anchor = { 0.0f, 0.0f };
		glm::vec2 Offset = { 0.0f, 0.0f };
		// Positive.
		glm::vec2 Size = { 0.0f, 0.0f };
		bool Interactable = true;
		// Sliders only.
		float HandleWidth = 0.0f;
	};

	// What the user did to an immediate-mode widget this frame. A widget drawn disabled this frame gets no Activated,
	// SliderFraction or SliderSteps, even when the input arrived while it was enabled.
	struct UIImmediateInput
	{
		UIWidgetState State = UIWidgetState::Normal;
		// A button was clicked, or a checkbox toggled.
		bool Activated = false;
		// A slider was dragged to this fraction of its range (applied before SliderSteps).
		std::optional<float> SliderFraction;
		// A slider was moved this many navigation stops (positive = towards Max) while focused.
		int SliderSteps = 0;
	};

	// The pointer, keyboard and gamepad side of the game UI while a scene plays: which widget is hovered, held and
	// focused, and what the user did to it. Created by Scene::OnRuntimeStart in Play mode (editing and simulating
	// draw every widget normal, or disabled when not Interactable).
	//
	// Every frame Update hit-tests the visible widget components (UILayout) and the immediate-mode widgets drawn
	// the frame before, topmost first: immediate widgets over components, later ones over earlier ones. Only
	// widgets take pointer input; a plain UIImage does not block it. Any left press in the game view drops the
	// keyboard/gamepad focus, and one on a widget holds it; a real release over the same widget clicks a button or
	// toggles a checkbox, and a held slider follows the pointer as it moves. Arrows, the D-pad and the left stick (with
	// repeat after RepeatDelay, every RepeatInterval) move the focus to the nearest widget in that direction,
	// Tab/Shift+Tab through all of them in draw order; Enter, keypad Enter, Space or gamepad A activate the focused
	// widget on the press, and Left/Right move a focused slider. Widget components change their own Value/Checked
	// and queue a UIWidgetEvent; immediate widgets read their UIImmediateInput when the script draws them again.
	class UIInteraction
	{
	public:
		// Seconds a direction is held before it repeats, and between repeats.
		static constexpr float RepeatDelay = 0.4f;
		static constexpr float RepeatInterval = 0.1f;
		// How far a stick must be pushed to navigate (along its dominant axis).
		static constexpr float StickThreshold = 0.5f;
		// Immediate-mode widgets per frame; more is almost certainly a loop that never ends.
		static constexpr size_t MaxImmediateWidgets = 4096;

		// Processes this frame's input (deltaTime drives the navigation repeat). Call once per played frame, before
		// the scripts update.
		void Update(Scene& scene, float deltaTime);
		// The widget component events of the last Update, in the order they happened.
		std::vector<UIWidgetEvent> TakeEvents();
		// The scene was replaced (Scene.Load, at the end of a frame): nothing is hovered, held or focused any more, and
		// the immediate-mode widgets the old scene drew are gone. The navigation setting stays (the Lua state does).
		void OnSceneLoaded();

		enum class SubmitResult
		{
			Accepted,
			// Another widget already used the id this frame.
			DuplicateID,
			// MaxImmediateWidgets were already drawn this frame.
			TooMany
		};

		// Registers an immediate-mode widget drawn this frame and returns, in `outInput`, what the user did to it
		// in the last Update. On DuplicateID or TooMany nothing is registered and `outInput` is left as it was.
		[[nodiscard]] SubmitResult SubmitImmediate(UIImmediateWidget widget, UIImmediateInput& outInput);

		// How the widget looks this frame.
		UIWidgetState GetState(const UIWidgetID& id, bool interactable) const;

		// The focused widget (invalid when none). A focus that is not a visible, interactable widget is dropped in an
		// Update, with a warning when it never was one; a newly set focus gets one Update to appear (an immediate-mode
		// widget set from OnCreate, a widget shown as it is focused). A script may set it while navigation is off;
		// the keyboard and gamepads then still do not act on it.
		const UIWidgetID& GetFocus() const { return m_Focus.ID; }
		void SetFocus(UIWidgetID id);
		// Whether the pointer was over an interactable widget in the last Update (so a click there is the UI's,
		// not the game's).
		bool IsPointerOverWidget() const { return m_Hovered.IsValid(); }

		// The game's keyboard and gamepad navigation setting (UI.SetNavigationEnabled): a game that reads the arrows,
		// Space or the D-pad itself while widgets are visible turns it off, so they never focus or activate a widget.
		// Turning it off drops the focus. On by default. The host's switch (GameUI::SetNavigationEnabled, e.g. the
		// editor view without focus) pauses navigation too but keeps the focus; this reports the game's setting.
		void SetNavigationEnabled(bool enabled);
		bool IsNavigationEnabled() const { return m_NavigationEnabled; }

	private:
		struct Candidate
		{
			UIWidgetID ID;
			UIWidgetKind Kind = UIWidgetKind::Button;
			UIBox Rect;
			bool Interactable = true;
			float HandleWidth = 0.0f;
			// Widget components: the resolved callback target and method (from UILayout::GetWidget).
			UUID Target = 0;
			std::string Callback;
		};

		// How sure the focus is that its widget exists.
		enum class FocusPhase
		{
			// Just set: it gets one Update to appear.
			New,
			// Set, and not a widget in the last Update: dropped, with a warning, if it is not one in the next.
			Unseen,
			// Was a live widget: dropped silently when it goes away.
			Seen
		};

		struct Focus
		{
			UIWidgetID ID;
			FocusPhase Phase = FocusPhase::New;
		};

		enum class Direction
		{
			Up = 0,
			Down,
			Left,
			Right
		};
		static constexpr size_t DirectionCount = 4;

		// The widget held by the pointer.
		struct Press
		{
			UIWidgetID ID;
			// A slider: it stays pressed while the pointer drags it off its rect.
			bool IsDrag = false;
			// The pointer's x when the slider last followed it (it follows only when the pointer moves).
			float DragX = 0.0f;
		};

		std::vector<Candidate> CollectWidgets(Scene& scene) const;
		void UpdatePointer(Scene& scene, const std::vector<Candidate>& widgets);
		void UpdateNavigation(Scene& scene, const std::vector<Candidate>& widgets, float deltaTime);
		// Whether a held direction fires this frame (when it starts, then repeating).
		bool Repeat(Direction direction, bool held, float deltaTime);

		// Focuses a widget that is live in this Update (navigation): no grace frame, no warning when it goes away.
		void FocusLive(const UIWidgetID& id);

		void Activate(Scene& scene, const Candidate& widget);
		void DragSlider(Scene& scene, const Candidate& widget, float pointerX);
		void StepSlider(Scene& scene, const Candidate& widget, int steps);
		// Writes a slider component's new value and queues its callback, when the value changed.
		void CommitSliderValue(const Candidate& widget, UISliderComponent& slider, float value);

	private:
		UIWidgetID m_Hovered;
		std::optional<Press> m_Pressed;
		Focus m_Focus;
		// The accept button is held on the focused widget.
		bool m_AcceptHeld = false;
		bool m_NavigationEnabled = true;
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
