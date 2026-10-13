#pragma once

#include "Basalt/UI/UIDrawCommand.h"

#include <glm/glm.hpp>

#include <cstddef>
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
	};

}
