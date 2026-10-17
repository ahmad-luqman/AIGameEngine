#pragma once

#include "Basalt/Core/Input.h"
#include "Basalt/Project/Project.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace Basalt {

	// Where the Game view shows its image inside the panel.
	struct GameViewRect
	{
		// Top-left corner, relative to the panel's content origin, in logical (ImGui) units, as is Size.
		glm::vec2 Origin{ 0.0f };
		glm::vec2 Size{ 0.0f };
	};

	// The largest rectangle with the game's aspect ratio that fits the panel, centred (bars fill the rest).
	// Degenerate sizes (zero or negative) give an empty rectangle.
	GameViewRect FitLetterbox(const glm::vec2& panel, const glm::uvec2& game);

	// What the editor's Game view does with the cursor the game asks for (Input::GetCursorMode), decided once per
	// frame before the scene updates. A pure function of the editor's state, so it can be tested without a window.
	struct GameCursorInput
	{
		// What the game asks for (Normal outside Play).
		CursorMode Requested = CursorMode::Normal;
		bool ViewFocused = false;
		bool ViewHovered = false;
		bool EscapePressed = false;
		bool Clicked = false;
		// Whether Escape handed the cursor back to the editor (the previous decision's Released).
		bool Released = false;
	};

	struct GameCursorDecision
	{
		// What the window does: Locked captures the mouse, Normal leaves it to the editor (Hidden only hides it over
		// the game image, through ImGui, so the rest of the editor keeps its cursor).
		CursorMode Window = CursorMode::Normal;
		bool HideOverView = false;
		// Escape released the cursor (until the Game view is clicked again, or the game stops asking).
		bool Released = false;
		// This Escape released the cursor: it is the editor's, not the game's (Input::WithholdFromGame).
		bool ConsumeEscape = false;
	};

	GameCursorDecision DecideGameCursor(const GameCursorInput& input);

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

	private:
		// Only Apply sets it, so it is always within [MinResolution, MaxResolution].
		std::optional<glm::uvec2> m_Override;
	};

}
