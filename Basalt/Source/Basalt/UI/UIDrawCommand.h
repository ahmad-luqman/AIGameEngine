#pragma once

#include "Basalt/Scene/Components.h"
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
	// Commands built from components (UILayout) carry their laid-out box as Position/Size with Anchor and
	// Pivot at zero; immediate-mode commands carry the caller's anchor, offset and pivot. Rect commands use
	// only placement and Color.
	struct UIDrawCommand
	{
		UIDrawKind Kind = UIDrawKind::Rect;
		glm::mat3 Transform = glm::mat3(1.0f);
		float Rotation = 0.0f;
		glm::vec2 Scale = { 1.0f, 1.0f };
		std::optional<UIBox> Clip;

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
		glm::vec2 UVMin = { 0.0f, 0.0f };
		glm::vec2 UVMax = { 1.0f, 1.0f };
		uint32_t SpriteColumns = 1;
		uint32_t SpriteRows = 1;
		uint32_t SpriteFrame = 0;

		// Text: each line is placed in the box at Align (0 = left/top, 1 = right/bottom). With a zero-size box
		// (immediate-mode text without Width) Align is the text's pivot on the box position. Wrap breaks lines
		// at the box width when it is positive.
		std::string Text;
		std::string Font;
		float FontSize = UITextComponent::DefaultFontSize;
		glm::vec2 Align = { 0.0f, 0.0f };
		bool Wrap = false;
		bool Shadow = true;
	};

	// A GPU texture the UI can draw: its ImGui texture id (an nvrhi::ITexture* with the engine's ImGui backend;
	// 0 is unusable, and UIFrame::Prepare drops it) and its size in texels. Valid for the frame it was looked up
	// in (SceneRenderer::GetUITexture).
	struct UITexture
	{
		uint64_t Handle = 0;
		glm::vec2 Size = { 0.0f, 0.0f };
	};

	// The command's box on a canvas of the given size.
	UIBox ResolveUIBox(const UIDrawCommand& command, const glm::vec2& canvasSize);

	// Moves the command's values into the ranges the renderer handles, so its box stays finite: Position and
	// Size within +-UIRectComponent::MaxMagnitude (Size >= 0), Anchor, Pivot, Align and Color within 0..1, and
	// Slice, SliceScale and FontSize within their component limits. NaN or infinite placement makes the
	// whole command unusable (returns false); other non-finite values take their defaults.
	bool SanitizeUIDrawCommand(UIDrawCommand& command);

}
