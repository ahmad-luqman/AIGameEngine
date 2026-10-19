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
		bool Vertical = false;
		std::optional<UIBox> Clip;
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
	// Each Update hit-tests transformed, clipped components and the preceding frame's immediate widgets,
	// topmost first. Widgets and blocking panels consume the pointer; plain images do not. A left press
	// clears focus, except text inputs and dropdowns take focus when pressed. Release over the same widget
	// activates it, while held sliders follow the pointer outside their rect. The wheel goes to the nearest
	// enabled scroll ancestor. Dropdowns support pointer selection and Up/Down, accept and Escape; text
	// inputs support Unicode insertion, cursor movement and deletion while the host routes keyboard input.
	// Arrows, D-pad and left stick navigate explicit targets or spatial neighbors (with repeat), and Tab
	// traverses controls in draw order. Slider-axis keys step its value; accept activates the focused control.
	// Components update their own value and queue UIWidgetEvent callbacks; immediate widgets read the next
	// UIImmediateInput. Keyboard/gamepad actions show the focus ring; a pointer press hides it.
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
		// navigation and activation remain paused, while a focused text input can still accept editing keys.
		const UIWidgetID& GetFocus() const { return m_Focus.ID; }
		// The last input modality was keyboard/gamepad; pointer presses hide focus rings.
		bool IsFocusVisible() const { return m_FocusVisible; }
		void SetFocus(UIWidgetID id);
		// Whether the pointer was over an interactable widget or blocking panel in the last Update (so a click there is the UI's,
		// not the game's).
		bool IsPointerOverWidget() const { return m_PointerBlocked; }
		UUID GetOpenDropdown() const { return m_OpenDropdown; }
		uint32_t GetDropdownHighlight() const { return m_DropdownHighlight; }
		size_t GetTextCursor() const { return m_TextCursor; }

		// The game's keyboard and gamepad navigation setting (UI.SetNavigationEnabled): a game that reads the arrows,
		// Space or the D-pad itself while widgets are visible turns it off to pause navigation and activation.
		// Pointer-focused text inputs still edit while the host routes keyboard input.
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
			bool Vertical = false;
			glm::mat3 Transform = glm::mat3(1.0f);
			std::optional<UIBox> Clip;
			std::optional<uint32_t> Option;
			bool Revealable = false;
			bool Contains(const glm::vec2& point) const { return (!Clip || Clip->Contains(point)) && ContainsUIBox(Rect, Transform, point); }
			bool IsNavigable() const
			{
				if (!Interactable || Kind == UIWidgetKind::Panel || Kind == UIWidgetKind::ScrollView || Option || Rect.Size.x <= 0.0f || Rect.Size.y <= 0.0f)
					return false;
				if (Revealable || !Clip)
					return true;
				const UIBox visible = IntersectUIBoxes(TransformUIBox(Rect, Transform), *Clip);
				return visible.Size.x > 0.0f && visible.Size.y > 0.0f;
			}
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
			// The pointer position when the slider last followed it (only actual pointer movement drags).
			glm::vec2 DragPosition = { 0.0f, 0.0f };
			std::optional<uint32_t> Option;
			bool DropdownWasOpen = false;
		};

		std::vector<Candidate> CollectWidgets(Scene& scene) const;
		void UpdatePointer(Scene& scene, const std::vector<Candidate>& widgets);
		void UpdateNavigation(Scene& scene, const std::vector<Candidate>& widgets, float deltaTime);
		// Whether a held direction fires this frame (when it starts, then repeating).
		bool Repeat(Direction direction, bool held, float deltaTime);

		// Focuses a widget that is live in this Update (navigation): no grace frame, no warning when it goes away.
		void FocusLive(const UIWidgetID& id);

		void Activate(Scene& scene, const Candidate& widget);
		void DragSlider(Scene& scene, const Candidate& widget, const glm::vec2& pointer);
		void UpdateText(Scene& scene, const std::vector<Candidate>& widgets);
		void RevealFocus(Scene& scene);
		void SelectDropdown(Scene& scene, const Candidate& widget, uint32_t index);
		void StepSlider(Scene& scene, const Candidate& widget, int steps);
		// Writes a slider component's new value and queues its callback, when the value changed.
		void CommitSliderValue(const Candidate& widget, UISliderComponent& slider, float value);

	private:
		UIWidgetID m_Hovered;
		bool m_PointerBlocked = false;
		bool m_FocusVisible = false;
		std::optional<glm::vec2> m_LastPointer;
		UUID m_OpenDropdown = 0;
		uint32_t m_DropdownHighlight = 0;
		UIWidgetID m_TextFocus;
		size_t m_TextCursor = 0;
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
