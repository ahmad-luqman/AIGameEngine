#pragma once

#include "Basalt/UI/UIDrawCommand.h"

#include <glm/glm.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct ImDrawList;
struct ImFont;

namespace Basalt {

	class Scene;

	// What the UI draws with, resolved before the ImGui frame: textures and fonts by path. A texture without
	// an entry draws the image's Color instead; a font without one draws with the built-in font.
	struct UIResources
	{
		std::unordered_map<std::string, UITexture> Textures;
		std::unordered_map<std::string, ImFont*> Fonts;
	};

	// One line of text after breaking: a range of the source string (it points into that string) and its
	// width in screen (ImGui) units.
	struct UITextLine
	{
		std::string_view Text;
		float Width = 0.0f;
	};

	// Draws UI draw commands into an ImGui draw list (the engine's game UI goes through ImGui's renderer, at
	// display resolution, after the scene).
	class UIRenderer
	{
	public:
		// Largest text size, in physical pixels. Glyphs are rasterized at ImGui's font rasterizer density (the
		// display's framebuffer scale), so the cap in ImGui units is this divided by it. Together with TrueTypeValidator's
		// MaxGlyphExtentPerLineHeight (3) it bounds a glyph bitmap to about 3072 px, inside ImGui's 8192 px font
		// atlas; change them together.
		static constexpr float MaxTextPixelSize = 1024.0f;

		// Draws `commands` onto the rectangle [origin, origin + size) of the draw list (screen units), which
		// shows the whole canvas: GameUI::CanvasHeight units tall, and `canvasSize.x` wide (anchored commands
		// are placed on that width). Fonts come from `resources` (missing ones draw with the built-in font), so
		// drawing loads no font files.
		static void Draw(ImDrawList* drawList, const glm::vec2& origin, const glm::vec2& size, const glm::vec2& canvasSize, std::span<const UIDrawCommand> commands, const UIResources& resources);

		// Breaks `text` into lines at '\n' and, when wrapWidth > 0, where ImGui's word wrap does: between
		// words, or inside a word wider than wrapWidth. Trailing blanks are left out of a line's text and
		// width, and a line that starts after a wrap drops its leading blanks.
		static std::vector<UITextLine> BreakLines(ImFont* font, float pixelSize, std::string_view text, float wrapWidth);
	};

	// One frame of game UI, shared by the runtime and the editor: built and prepared in OnRender (outside the
	// ImGui frame, with a command list open for texture uploads), drawn in OnImGuiRender. Every step uses the
	// canvas Build was given, so components and anchored commands agree even while the viewport resizes.
	class UIFrame
	{
	public:
		// Collects the scene's UI components (when `scene` is set) and then, with `includeImmediate`, this
		// frame's UI.* commands, laid out for a viewport of `viewportSize` screen units. Forgets the last
		// frame's resources: call Prepare before Draw. Publishes completed CPU assets first,
		// independently of scene rendering, so camera-less UI can resolve streamed images.
		void Build(Scene* scene, const glm::vec2& viewportSize, bool includeImmediate);
		void Clear();

		// Loads every font the commands use (so file reads, font checks and their one-time errors stay out of the
		// draw) and looks up every texture with `lookupTexture` (which uploads it on first use).
		template<typename LookupTexture>
		void Prepare(LookupTexture&& lookupTexture)
		{
			LoadFonts();
			for (const UIDrawCommand& command : m_Commands)
			{
				if (command.Kind != UIDrawKind::Image || command.Texture.empty() || m_Resources.Textures.contains(command.Texture))
					continue;
				if (std::optional<UITexture> texture = lookupTexture(command.Texture); texture && texture->Handle != 0)
					m_Resources.Textures.emplace(command.Texture, *texture);
			}
		}

		void Draw(ImDrawList* drawList, const glm::vec2& origin, const glm::vec2& size) const;

		const std::vector<UIDrawCommand>& GetCommands() const { return m_Commands; }
		const UIResources& GetResources() const { return m_Resources; }
		const glm::vec2& GetCanvasSize() const { return m_CanvasSize; }

	private:
		void LoadFonts();

	private:
		std::vector<UIDrawCommand> m_Commands;
		UIResources m_Resources;
		glm::vec2 m_CanvasSize = { 1920.0f, 1080.0f };
	};

}
