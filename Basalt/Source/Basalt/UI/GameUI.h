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

		// Queues a command for this frame. Commands with NaN or infinite placement are dropped (see
		// SanitizeUIDrawCommand), and so is everything past MaxCommands; returns whether it was queued.
		static bool Submit(UIDrawCommand command);

		// Called at the start of every scene update.
		static void BeginFrame();
		static std::vector<UIDrawCommand> GetCommands();
		static size_t GetCommandCount();

		// Canvas size for a viewport of the given pixel size.
		static glm::vec2 GetCanvasSize(const glm::vec2& viewportSize);

		// Viewport size used by GetCanvasSize() from scripts (set by the host each frame).
		static void SetViewportSize(const glm::vec2& size);
		static glm::vec2 GetViewportSize();
	};

}
