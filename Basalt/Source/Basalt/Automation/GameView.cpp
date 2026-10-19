#include "Basalt/Automation/GameView.h"

#include "Basalt/Automation/CommandSchema.h"
#include "Basalt/Project/Project.h"

#include <algorithm>

namespace Basalt {

	GameViewRect FitLetterbox(const glm::vec2& panel, const glm::uvec2& game)
	{
		if (!(panel.x > 0.0f && panel.y > 0.0f) || game.x == 0 || game.y == 0)
			return {};
		const glm::vec2 gameSize(static_cast<float>(game.x), static_cast<float>(game.y));
		const float scale = std::min(panel.x / gameSize.x, panel.y / gameSize.y);
		GameViewRect rect;
		rect.Size = gameSize * scale;
		rect.Origin = (panel - rect.Size) * 0.5f;
		return rect;
	}

	GameCursorDecision DecideGameCursor(const GameCursorInput& input)
	{
		GameCursorDecision decision;
		decision.Released = input.Released;
		if (input.Requested != CursorMode::Locked)
			decision.Released = false;
		else if (!input.Released && input.ViewFocused && input.EscapePressed)
			decision.Released = decision.ConsumeEscape = true;
		else if (input.Released && input.ViewHovered && input.Clicked)
		{
			decision.Released = false;
			decision.ConsumeClick = true;
		}
		const bool active = input.ViewFocused && !decision.Released;
		decision.LockWindow = active && input.Requested == CursorMode::Locked;
		decision.HideOverView = active && input.Requested == CursorMode::Hidden;
		return decision;
	}

	GameViewRouting RouteGameView(const GameViewRoutingInput& input)
	{
		GameViewRouting routing;
		routing.Focus = input.HasImage && input.ViewFocused;
		routing.Pointer = routing.Focus && ((input.ViewHovered && input.MouseOverImage) || input.CursorLocked);
		return routing;
	}

	bool IsInsideRect(const glm::vec2& point, const glm::vec2& min, const glm::vec2& size)
	{
		return glm::all(glm::greaterThanEqual(point, min)) && glm::all(glm::lessThan(point, min + size));
	}

	nlohmann::json GameViewSettings::Schema()
	{
		namespace S = CommandSchema;
		auto resolution = S::Nullable({ { "type", "array" }, { "items", S::Integer("pixels", MinResolution, MaxResolution) }, { "minItems", 2 }, { "maxItems", 2 }, { "description", "[width, height] the Game view renders at for this session; null uses the project's window size" } });
		return S::Object({ { "stats", S::Boolean("Show the play-mode statistics overlay") }, { "resolution", resolution }, { "focus", S::Boolean("Bring the Game view tab to the front") }, { "path", S::String("Save the Game view's image (with game UI) to this .png", true) } });
	}

	bool GameViewSettings::Apply(const nlohmann::json& patch, std::string& outError)
	{
		if (!CommandSchema::Validate(Schema(), patch, outError))
			return false;
		if (patch.contains("stats"))
			m_ShowStats = patch["stats"].get<bool>();
		const auto it = patch.find("resolution");
		if (it == patch.end())
			return true;
		if (it->is_null())
		{
			m_Override.reset();
			return true;
		}
		m_Override = glm::uvec2((*it)[0].get<uint32_t>(), (*it)[1].get<uint32_t>());
		return true;
	}

	glm::uvec2 GameViewSettings::Resolve(const ProjectConfig* project) const
	{
		if (m_Override)
			return *m_Override;
		if (!project)
			return FallbackResolution;
		// A hand-edited project file can hold any size; render within the range the runtime accepts.
		return { std::clamp(project->WindowWidth, MinResolution, MaxResolution), std::clamp(project->WindowHeight, MinResolution, MaxResolution) };
	}

}
