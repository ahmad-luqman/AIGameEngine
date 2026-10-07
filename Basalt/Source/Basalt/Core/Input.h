#pragma once

#include "Basalt/Core/KeyCodes.h"
#include "Basalt/Core/MouseCodes.h"

#include <glm/vec2.hpp>

namespace Basalt {

	class Event;

	// Polled input state. Fed by window events (or injected by tests / the automation API), so it works
	// identically with and without a window.
	//
	// "Down" means held. "Pressed"/"Released" are true only on the frame the transition happened.
	class Input
	{
	public:
		static bool IsKeyDown(KeyCode key);
		static bool IsKeyPressed(KeyCode key);
		static bool IsKeyReleased(KeyCode key);

		static bool IsMouseButtonDown(MouseCode button);
		static bool IsMouseButtonPressed(MouseCode button);
		static bool IsMouseButtonReleased(MouseCode button);

		static glm::vec2 GetMousePosition();
		// Mouse movement since the previous frame.
		static glm::vec2 GetMouseDelta();
		// Scroll accumulated during the current frame.
		static glm::vec2 GetMouseScroll();

		// --- State feeding ---------------------------------------------------------------------
		// Updates state from a platform event. Does not mark the event handled.
		static void OnEvent(Event& event);
		// Rolls "pressed/released this frame" into history. Call once at the end of each frame.
		static void EndFrame();
		// Clears every key and button (e.g. on focus loss or when leaving play mode).
		static void Reset();

		static void SetKeyState(KeyCode key, bool down);
		static void SetMouseButtonState(MouseCode button, bool down);
		static void SetMousePosition(const glm::vec2& position);
		static void AddMouseScroll(const glm::vec2& scroll);
	};

}
