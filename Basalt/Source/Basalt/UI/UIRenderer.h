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

	// Resolved textures by path; a missing entry draws the image's Color instead.
	using UITextureMap = std::unordered_map<std::string, UITexture>;

	// One line of text after breaking: a range of the source string and its width in pixels.
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
		// Largest text size in pixels: bigger glyphs would not fit the font atlas. Text is drawn at its canvas
		// size times the canvas scale, capped here.
		static constexpr float MaxTextPixelSize = 1024.0f;

		// Draws `commands` onto the rectangle [origin, origin + size) of the draw list (screen pixels), which
		// shows the whole canvas (GameUI::CanvasHeight units tall).
		static void Draw(ImDrawList* drawList, const glm::vec2& origin, const glm::vec2& size, std::span<const UIDrawCommand> commands, const UITextureMap& textures);

		// Breaks `text` into lines at '\n' and, when wrapWidth > 0, between words so each line fits
		// wrapWidth pixels (a single word wider than that gets a line of its own and may overflow). Trailing
		// blanks do not count toward a line's width, and a wrapped line drops its leading blanks.
		static std::vector<UITextLine> BreakLines(ImFont* font, float pixelSize, std::string_view text, float wrapWidth);
	};

	// One frame of game UI, shared by the runtime and the editor: built and prepared in OnRender (outside the
	// ImGui frame, with a command list open for texture uploads), drawn in OnImGuiRender.
	class UIFrame
	{
	public:
		// Collects the scene's UI components (when `scene` is set) and then, with `includeImmediate`, this
		// frame's UI.* commands, laid out for a viewport of `viewportSize` pixels.
		void Build(Scene* scene, const glm::vec2& viewportSize, bool includeImmediate);
		void Clear();

		// Loads every font the commands use and looks up every texture with `lookupTexture` (which uploads it
		// to the GPU on first use). Call outside an ImGui frame: new fonts are added to the atlas.
		template<typename LookupTexture>
		void Prepare(LookupTexture&& lookupTexture)
		{
			LoadFonts();
			m_Textures.clear();
			for (const UIDrawCommand& command : m_Commands)
			{
				if (command.Kind != UIDrawKind::Image || command.Texture.empty() || m_Textures.contains(command.Texture))
					continue;
				if (std::optional<UITexture> texture = lookupTexture(command.Texture))
					m_Textures.emplace(command.Texture, *texture);
			}
		}

		void Draw(ImDrawList* drawList, const glm::vec2& origin, const glm::vec2& size) const;

		const std::vector<UIDrawCommand>& GetCommands() const { return m_Commands; }
		const UITextureMap& GetTextures() const { return m_Textures; }

	private:
		void LoadFonts() const;

	private:
		std::vector<UIDrawCommand> m_Commands;
		UITextureMap m_Textures;
	};

}
