#include "Basalt/UI/GameUI.h"

#include "Basalt/Core/Input.h"
#include "Basalt/UI/UIGeometry.h"

#include <cmath>
#include <mutex>

namespace Basalt {

	namespace {

		std::mutex s_Mutex;
		std::vector<UIDrawCommand> s_Commands;
		// The defaults are the headless view.
		GameUI::HostState s_Host;

		bool IsUsableSize(const glm::vec2& size)
		{
			return std::isfinite(size.x) && std::isfinite(size.y) && size.x > 0.0f && size.y > 0.0f;
		}

		bool IsUsableOrigin(const glm::vec2& origin)
		{
			return std::isfinite(origin.x) && std::isfinite(origin.y);
		}

	}

	GameUI::SubmitResult GameUI::Submit(UIDrawCommand command)
	{
		if (!SanitizeUIDrawCommand(command))
			return SubmitResult::InvalidPlacement;
		std::scoped_lock lock(s_Mutex);
		if (s_Commands.size() >= MaxCommands)
			return SubmitResult::QueueFull;
		s_Commands.push_back(std::move(command));
		return SubmitResult::Queued;
	}

	void GameUI::BeginFrame()
	{
		std::scoped_lock lock(s_Mutex);
		s_Commands.clear();
	}

	void GameUI::AppendCommands(std::vector<UIDrawCommand>& out)
	{
		std::scoped_lock lock(s_Mutex);
		out.insert(out.end(), s_Commands.begin(), s_Commands.end());
	}

	size_t GameUI::GetCommandCount()
	{
		std::scoped_lock lock(s_Mutex);
		return s_Commands.size();
	}

	glm::vec2 GameUI::GetCanvasSize(const glm::vec2& viewportSize)
	{
		const float aspect = viewportSize.y > 0.0f ? viewportSize.x / viewportSize.y : 16.0f / 9.0f;
		return { CanvasHeight * aspect, CanvasHeight };
	}

	void GameUI::SetViewportSize(const glm::vec2& size)
	{
		std::scoped_lock lock(s_Mutex);
		if (IsUsableSize(size))
			s_Host.ViewportSize = size;
	}

	glm::vec2 GameUI::GetViewportSize()
	{
		std::scoped_lock lock(s_Mutex);
		return s_Host.ViewportSize;
	}

	void GameUI::SetViewportOrigin(const glm::vec2& origin)
	{
		std::scoped_lock lock(s_Mutex);
		if (IsUsableOrigin(origin))
			s_Host.ViewportOrigin = origin;
	}

	glm::vec2 GameUI::GetViewportOrigin()
	{
		std::scoped_lock lock(s_Mutex);
		return s_Host.ViewportOrigin;
	}

	void GameUI::SetPointerEnabled(bool enabled)
	{
		std::scoped_lock lock(s_Mutex);
		s_Host.PointerEnabled = enabled;
	}

	bool GameUI::IsPointerEnabled()
	{
		std::scoped_lock lock(s_Mutex);
		return s_Host.PointerEnabled;
	}

	void GameUI::SetNavigationEnabled(bool enabled)
	{
		std::scoped_lock lock(s_Mutex);
		s_Host.NavigationEnabled = enabled;
	}

	bool GameUI::IsNavigationEnabled()
	{
		std::scoped_lock lock(s_Mutex);
		return s_Host.NavigationEnabled;
	}

	std::optional<glm::vec2> GameUI::GetPointerPosition()
	{
		if (!Input::HasMousePosition())
			return std::nullopt;
		const glm::vec2 mouse = Input::GetMousePosition();
		std::scoped_lock lock(s_Mutex);
		if (!s_Host.PointerEnabled)
			return std::nullopt;
		return UICanvasMapping::ForViewport(s_Host.ViewportOrigin, s_Host.ViewportSize).ToCanvas(mouse);
	}

	GameUI::HostState GameUI::GetHostState()
	{
		std::scoped_lock lock(s_Mutex);
		return s_Host;
	}

	void GameUI::SetHostState(const HostState& state)
	{
		std::scoped_lock lock(s_Mutex);
		if (IsUsableOrigin(state.ViewportOrigin))
			s_Host.ViewportOrigin = state.ViewportOrigin;
		if (IsUsableSize(state.ViewportSize))
			s_Host.ViewportSize = state.ViewportSize;
		s_Host.PointerEnabled = state.PointerEnabled;
		s_Host.NavigationEnabled = state.NavigationEnabled;
	}

}
