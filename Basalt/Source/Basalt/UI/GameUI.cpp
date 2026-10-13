#include "Basalt/UI/GameUI.h"

#include "Basalt/Core/Input.h"
#include "Basalt/UI/UIGeometry.h"

#include <cmath>
#include <mutex>

namespace Basalt {

	namespace {

		std::mutex s_Mutex;
		std::vector<UIDrawCommand> s_Commands;
		glm::vec2 s_ViewportSize = { 1920.0f, 1080.0f };
		glm::vec2 s_ViewportOrigin = { 0.0f, 0.0f };
		bool s_PointerEnabled = true;
		bool s_NavigationEnabled = true;

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
		if (std::isfinite(size.x) && std::isfinite(size.y) && size.x > 0.0f && size.y > 0.0f)
			s_ViewportSize = size;
	}

	glm::vec2 GameUI::GetViewportSize()
	{
		std::scoped_lock lock(s_Mutex);
		return s_ViewportSize;
	}

	void GameUI::SetViewportOrigin(const glm::vec2& origin)
	{
		std::scoped_lock lock(s_Mutex);
		if (std::isfinite(origin.x) && std::isfinite(origin.y))
			s_ViewportOrigin = origin;
	}

	glm::vec2 GameUI::GetViewportOrigin()
	{
		std::scoped_lock lock(s_Mutex);
		return s_ViewportOrigin;
	}

	void GameUI::SetPointerEnabled(bool enabled)
	{
		std::scoped_lock lock(s_Mutex);
		s_PointerEnabled = enabled;
	}

	bool GameUI::IsPointerEnabled()
	{
		std::scoped_lock lock(s_Mutex);
		return s_PointerEnabled;
	}

	void GameUI::SetNavigationEnabled(bool enabled)
	{
		std::scoped_lock lock(s_Mutex);
		s_NavigationEnabled = enabled;
	}

	bool GameUI::IsNavigationEnabled()
	{
		std::scoped_lock lock(s_Mutex);
		return s_NavigationEnabled;
	}

	std::optional<glm::vec2> GameUI::GetPointerPosition()
	{
		const glm::vec2 mouse = Input::GetMousePosition();
		std::scoped_lock lock(s_Mutex);
		if (!s_PointerEnabled)
			return std::nullopt;
		return UICanvasMapping::ForViewport(s_ViewportOrigin, s_ViewportSize).ToCanvas(mouse);
	}

}
