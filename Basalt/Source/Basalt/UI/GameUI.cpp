#include "Basalt/UI/GameUI.h"

#include "Basalt/Core/Input.h"
#include "Basalt/UI/UIGeometry.h"

#include <cmath>
#include <mutex>

namespace Basalt {

	namespace {

		std::mutex s_Mutex;
		std::vector<UIDrawCommand> s_Commands;
		// The host's view without the pointer and navigation flags, which are Input's game routing. The defaults are
		// the headless view.
		struct HostView
		{
			glm::vec2 ViewportOrigin = GameUI::HostState{}.ViewportOrigin;
			glm::vec2 ViewportSize = GameUI::HostState{}.ViewportSize;
			std::optional<glm::vec2> Resolution;
		};
		HostView s_Host;

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

	void GameUI::SetResolution(const std::optional<glm::vec2>& resolution)
	{
		std::scoped_lock lock(s_Mutex);
		if (!resolution || IsUsableSize(*resolution))
			s_Host.Resolution = resolution;
	}

	glm::vec2 GameUI::GetResolution()
	{
		std::scoped_lock lock(s_Mutex);
		return s_Host.Resolution.value_or(s_Host.ViewportSize);
	}

	glm::vec2 GameUI::ToGamePosition(const glm::vec2& mouse)
	{
		std::scoped_lock lock(s_Mutex);
		return (mouse - s_Host.ViewportOrigin) * (s_Host.Resolution.value_or(s_Host.ViewportSize) / s_Host.ViewportSize);
	}

	glm::vec2 GameUI::ToGameDelta(const glm::vec2& delta)
	{
		std::scoped_lock lock(s_Mutex);
		return delta * (s_Host.Resolution.value_or(s_Host.ViewportSize) / s_Host.ViewportSize);
	}

	void GameUI::SetPointerEnabled(bool enabled)
	{
		Input::SetGamePointer(enabled);
	}

	bool GameUI::IsPointerEnabled()
	{
		return Input::HasGamePointer();
	}

	void GameUI::SetNavigationEnabled(bool enabled)
	{
		Input::SetGameFocus(enabled);
	}

	bool GameUI::IsNavigationEnabled()
	{
		return Input::HasGameFocus();
	}

	std::optional<glm::vec2> GameUI::GetPointerPosition()
	{
		if (!Input::HasMousePosition() || !Input::HasGamePointer())
			return std::nullopt;
		const glm::vec2 mouse = Input::GetMousePosition();
		std::scoped_lock lock(s_Mutex);
		return UICanvasMapping::ForViewport(s_Host.ViewportOrigin, s_Host.ViewportSize).ToCanvas(mouse);
	}

	GameUI::HostState GameUI::GetHostState()
	{
		// Input's lock is never taken while holding this one.
		const bool pointer = Input::HasGamePointer();
		const bool navigation = Input::HasGameFocus();
		std::scoped_lock lock(s_Mutex);
		return { s_Host.ViewportOrigin, s_Host.ViewportSize, pointer, navigation, s_Host.Resolution };
	}

	void GameUI::SetHostState(const HostState& state)
	{
		Input::SetGamePointer(state.PointerEnabled);
		Input::SetGameFocus(state.NavigationEnabled);
		std::scoped_lock lock(s_Mutex);
		if (IsUsableOrigin(state.ViewportOrigin))
			s_Host.ViewportOrigin = state.ViewportOrigin;
		if (IsUsableSize(state.ViewportSize))
			s_Host.ViewportSize = state.ViewportSize;
		if (!state.Resolution || IsUsableSize(*state.Resolution))
			s_Host.Resolution = state.Resolution;
	}

}
