#pragma once

#include <glm/glm.hpp>

#include <mutex>
#include <string>
#include <vector>

struct ImDrawList;

namespace Basalt {

	enum class UIAlign
	{
		Left = 0,
		Center,
		Right
	};

	struct UIDrawCommand
	{
		enum class Kind
		{
			Text,
			Rect
		};
		Kind Type = Kind::Text;
		glm::vec2 Position = { 0.0f, 0.0f };
		glm::vec2 Size = { 0.0f, 0.0f };
		glm::vec4 Color = { 1.0f, 1.0f, 1.0f, 1.0f };
		float FontSize = 32.0f;
		UIAlign Align = UIAlign::Left;
		std::string Text;
	};

	// Immediate-mode screen-space UI for games (score, lives, menus). Scripts submit commands every frame on
	// a virtual canvas that is CanvasHeight units tall (width follows the aspect ratio), origin top-left.
	// Commands persist until the next scene update, so a paused game keeps showing its UI.
	class GameUI
	{
	public:
		static constexpr float CanvasHeight = 1080.0f;

		static void Text(const std::string& text, const glm::vec2& position, float fontSize, const glm::vec4& color, UIAlign align);
		static void Rect(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color);

		// Called at the start of every scene update.
		static void BeginFrame();
		static std::vector<UIDrawCommand> GetCommands();
		static size_t GetCommandCount();

		// Canvas size for a viewport of the given pixel size.
		static glm::vec2 GetCanvasSize(const glm::vec2& viewportSize);
		// Draws the commands into an ImGui draw list covering the rectangle [origin, origin + size).
		static void Draw(ImDrawList* drawList, const glm::vec2& origin, const glm::vec2& size);

		// Viewport size used by GetCanvasSize() from scripts (set by the host each frame).
		static void SetViewportSize(const glm::vec2& size);
		static glm::vec2 GetViewportSize();

		static constexpr size_t MaxCommands = 65536;
	};

}
