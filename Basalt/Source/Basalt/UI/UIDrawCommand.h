#pragma once

#include "Basalt/UI/UIGeometry.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace Basalt {

	enum class UIDrawKind
	{
		Rect = 0,
		Image,
		Text
	};

	// One thing to draw on the UI canvas, from a UI component or an immediate-mode UI.* call. Placement is
	// resolved when drawing (ResolveUIBox), so anchored commands follow the canvas as the window resizes.
	struct UIDrawCommand
	{
		UIDrawKind Kind = UIDrawKind::Rect;

		// The box's top-left is Anchor * canvas size + Position - Pivot * Size (canvas units).
		glm::vec2 Anchor = { 0.0f, 0.0f };
		glm::vec2 Position = { 0.0f, 0.0f };
		glm::vec2 Size = { 0.0f, 0.0f };
		glm::vec2 Pivot = { 0.0f, 0.0f };
		// Fill or tint, and the text color (display color and opacity, 0..1).
		glm::vec4 Color = { 1.0f, 1.0f, 1.0f, 1.0f };

		// Image: project-relative path; empty fills with Color. Slice/SliceScale as in UIImageComponent.
		std::string Texture;
		glm::vec4 Slice = { 0.0f, 0.0f, 0.0f, 0.0f };
		float SliceScale = 1.0f;

		// Text: each line is placed in the box at Align (0 = left/top, 1 = right/bottom). With a zero-size box
		// (immediate-mode text) Align is the text's pivot on the box position. Wrap breaks lines at the box
		// width when it is positive.
		std::string Text;
		std::string Font;
		float FontSize = 32.0f;
		glm::vec2 Align = { 0.0f, 0.0f };
		bool Wrap = false;
		bool Shadow = true;
	};

	// A GPU texture the UI can draw: its ImGui texture id (an nvrhi::ITexture* with the engine's ImGui backend)
	// and its size in texels.
	struct UITexture
	{
		uint64_t Handle = 0;
		glm::vec2 Size = { 0.0f, 0.0f };
	};

	// The command's box on a canvas of the given size.
	UIBox ResolveUIBox(const UIDrawCommand& command, const glm::vec2& canvasSize);

	// Moves the command's values into the ranges the renderer handles: font size, colors, slices and alignment
	// are clamped; NaN or infinite placement makes the whole command unusable (returns false).
	bool SanitizeUIDrawCommand(UIDrawCommand& command);

}
