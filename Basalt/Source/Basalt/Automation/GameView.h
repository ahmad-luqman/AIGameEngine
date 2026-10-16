#pragma once

#include "Basalt/Project/Project.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace Basalt {

	// Where the Game view shows its image inside the panel.
	struct GameViewRect
	{
		// Top-left corner, relative to the panel's content origin, in screen units.
		glm::vec2 Origin{ 0.0f };
		glm::vec2 Size{ 0.0f };
		// Screen units per game pixel (below 1 when the panel is smaller than the game resolution).
		float Scale = 0.0f;
	};

	// The largest rectangle with the game's aspect ratio that fits the panel, centred (bars fill the rest).
	// Degenerate sizes (zero or negative) give an empty rectangle.
	GameViewRect FitLetterbox(const glm::vec2& panel, const glm::uvec2& game);

	// Game view options for the editor and editor.game_view: the resolution the game renders at is the
	// project's window size (what the exported game opens with) unless a session-only override is set.
	class GameViewSettings
	{
	public:
		// The window sizes the runtime accepts.
		static constexpr uint32_t MinResolution = ProjectConfig::MinWindowSize;
		static constexpr uint32_t MaxResolution = ProjectConfig::MaxWindowSize;
		// Used without a project.
		static constexpr glm::uvec2 FallbackResolution{ 1600, 900 };

		// {resolution: [w, h] | null (back to the project's), focus: bool, path: string}.
		static nlohmann::json Schema();
		// Applies the resolution part of a patch; nothing changes when it fails.
		bool Apply(const nlohmann::json& patch, std::string& outError);

		glm::uvec2 Resolve(const ProjectConfig* project) const;
		const std::optional<glm::uvec2>& GetOverride() const { return m_Override; }
		void SetOverride(std::optional<glm::uvec2> resolution) { m_Override = resolution; }

	private:
		std::optional<glm::uvec2> m_Override;
	};

}
