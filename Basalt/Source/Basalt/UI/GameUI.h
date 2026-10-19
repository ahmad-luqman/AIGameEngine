#pragma once

#include "Basalt/UI/UIDrawCommand.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <optional>
#include <vector>

namespace Basalt {

	// Immediate-mode screen-space UI for games (score, lives, menus): the UI.* Lua functions. Scripts submit
	// commands every frame on the UI canvas (CanvasHeight units tall, width following the aspect ratio,
	// origin top-left). Commands persist until the next scene update, so a paused game keeps showing its UI.
	// UIFrame draws them over the UI components.
	class GameUI
	{
	public:
		static constexpr float CanvasHeight = 1080.0f;
		static constexpr size_t MaxCommands = 65536;

		enum class SubmitResult
		{
			Queued,
			// NaN or infinite placement (see SanitizeUIDrawCommand); nothing was queued.
			InvalidPlacement,
			// MaxCommands are already queued this frame; nothing was queued.
			QueueFull
		};

		// Queues a command for this frame, sanitized (SanitizeUIDrawCommand).
		[[nodiscard]] static SubmitResult Submit(UIDrawCommand command);

		// Called at the start of every scene update that advances a frame (not while paused), and when play
		// starts and stops.
		static void BeginFrame();
		// A bounded, intersecting canvas clip stack, cleared with the frame.
		static bool PushClip(const UIBox& box);
		static bool PopClip();
		static std::optional<UIBox> GetClip();
		// Protected script calls preserve their caller's stack, including when Lua reports an error.
		static std::vector<UIBox> CaptureClipStack();
		// Returns whether the callback left a different stack; restores the supplied snapshot.
		static bool RestoreClipStack(std::vector<UIBox> clips);
		// Appends this frame's commands to `out`, in submission order.
		static void AppendCommands(std::vector<UIDrawCommand>& out);
		static size_t GetCommandCount();

		// Canvas size for a viewport of the given pixel size, and for the current viewport.
		static glm::vec2 GetCanvasSize(const glm::vec2& viewportSize);
		static glm::vec2 GetCanvasSize() { return GetCanvasSize(GetViewportSize()); }

		// Viewport size used by GetCanvasSize() from scripts (set by the host each frame; 1920x1080 until then,
		// e.g. headless). Sizes that are not positive and finite are ignored.
		static void SetViewportSize(const glm::vec2& size);
		static glm::vec2 GetViewportSize();

		// Where the viewport's top-left corner is in Input's mouse coordinates (window coordinates; 0,0 until set,
		// e.g. headless). Together with the viewport size it maps the mouse onto the canvas for the widgets; the
		// size must then be in the same units as the mouse (window units, not framebuffer pixels).
		// Non-finite origins are ignored.
		static void SetViewportOrigin(const glm::vec2& origin);
		static glm::vec2 GetViewportOrigin();

		// The game's size in the units its scripts see the mouse in (Lua Input.GetMousePosition: 0,0 top-left,
		// this size bottom-right): the editor's Game view sets the game's resolution, so scripts read game pixels
		// rather than editor window units. nullopt (the default, and what standalone hosts use) means the viewport
		// size: script coordinates are Input's relative to the viewport origin (Input's own in the runtime and
		// headless, where the origin is 0,0). A size that is not positive and finite is a programmer error (asserted)
		// and ignored.
		static void SetResolution(const std::optional<glm::vec2>& resolution);
		static glm::vec2 GetResolution();
		// Input's mouse coordinates (window units) in the game's: relative to the viewport origin, scaled from the
		// viewport size to the resolution. A delta is only scaled.
		static glm::vec2 ToGamePosition(const glm::vec2& mouse);
		static glm::vec2 ToGameDelta(const glm::vec2& delta);

		// Whether the widgets, and the game's scripts, take pointer input (the mouse is over the game view) and
		// navigation input (keyboard and gamepads: the game view has focus). The flags are Input's game routing
		// (Input::SetGamePointer, Input::SetGameFocus), so the widgets and GameInput always agree. Both true until a
		// host says otherwise; the editor follows its view panels' hover and focus.
		static void SetPointerEnabled(bool enabled);
		static bool IsPointerEnabled();
		static void SetNavigationEnabled(bool enabled);
		static bool IsNavigationEnabled();

		// The mouse position on the canvas of the current viewport; nullopt while pointer input is disabled, while the
		// game asks for a locked cursor (the position is then a virtual one that wanders off the view) or before the
		// first mouse position arrives.
		static std::optional<glm::vec2> GetPointerPosition();

		// Everything a host sets about its view, to save and restore around a run that needs the headless view (the
		// defaults). PointerEnabled and NavigationEnabled are Input's game routing, read and written through.
		struct HostState
		{
			glm::vec2 ViewportOrigin = { 0.0f, 0.0f };
			glm::vec2 ViewportSize = { 1920.0f, 1080.0f };
			bool PointerEnabled = true;
			bool NavigationEnabled = true;
			std::optional<glm::vec2> Resolution;

			bool operator==(const HostState&) const = default;
		};
		static HostState GetHostState();
		// Sets every field at once (an origin, size or resolution that is not usable is ignored, like its setter).
		static void SetHostState(const HostState& state);
	};

}
