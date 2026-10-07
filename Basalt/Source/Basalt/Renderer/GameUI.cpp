#include "Basalt/Renderer/GameUI.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace Basalt {

	namespace {

		std::mutex s_Mutex;
		std::vector<UIDrawCommand> s_Commands;
		glm::vec2 s_ViewportSize = { 1920.0f, 1080.0f };

		ImU32 ToColor(const glm::vec4& color)
		{
			const glm::vec4 clamped = glm::clamp(color, glm::vec4(0.0f), glm::vec4(1.0f));
			const auto channel = [](float value) { return static_cast<int>(std::lround(value * 255.0f)); };
			return IM_COL32(channel(clamped.r), channel(clamped.g), channel(clamped.b), channel(clamped.a));
		}

		void Push(UIDrawCommand command)
		{
			std::scoped_lock lock(s_Mutex);
			if (s_Commands.size() < GameUI::MaxCommands)
				s_Commands.push_back(std::move(command));
		}

	}

	void GameUI::Text(const std::string& text, const glm::vec2& position, float fontSize, const glm::vec4& color, UIAlign align)
	{
		UIDrawCommand command;
		command.Type = UIDrawCommand::Kind::Text;
		command.Position = position;
		command.FontSize = std::clamp(fontSize, 4.0f, 512.0f);
		command.Color = color;
		command.Align = align;
		command.Text = text;
		Push(std::move(command));
	}

	void GameUI::Rect(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color)
	{
		UIDrawCommand command;
		command.Type = UIDrawCommand::Kind::Rect;
		command.Position = position;
		command.Size = size;
		command.Color = color;
		Push(std::move(command));
	}

	void GameUI::BeginFrame()
	{
		std::scoped_lock lock(s_Mutex);
		s_Commands.clear();
	}

	std::vector<UIDrawCommand> GameUI::GetCommands()
	{
		std::scoped_lock lock(s_Mutex);
		return s_Commands;
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
		if (size.x > 0.0f && size.y > 0.0f)
			s_ViewportSize = size;
	}

	glm::vec2 GameUI::GetViewportSize()
	{
		std::scoped_lock lock(s_Mutex);
		return s_ViewportSize;
	}

	void GameUI::Draw(ImDrawList* drawList, const glm::vec2& origin, const glm::vec2& size)
	{
		if (!drawList || size.x <= 0.0f || size.y <= 0.0f)
			return;
		const float scale = size.y / CanvasHeight;
		ImFont* font = ImGui::GetFont();
		drawList->PushClipRect(ImVec2(origin.x, origin.y), ImVec2(origin.x + size.x, origin.y + size.y), true);
		for (const UIDrawCommand& command : GetCommands())
		{
			const ImVec2 position(origin.x + command.Position.x * scale, origin.y + command.Position.y * scale);
			if (command.Type == UIDrawCommand::Kind::Rect)
			{
				drawList->AddRectFilled(position, ImVec2(position.x + command.Size.x * scale, position.y + command.Size.y * scale), ToColor(command.Color));
				continue;
			}

			const float fontSize = command.FontSize * scale;
			ImVec2 textPosition = position;
			if (command.Align != UIAlign::Left)
			{
				const ImVec2 extent = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, command.Text.c_str());
				textPosition.x -= command.Align == UIAlign::Center ? extent.x * 0.5f : extent.x;
			}
			// A soft drop shadow keeps text readable over bright scenes.
			drawList->AddText(font, fontSize, ImVec2(textPosition.x + fontSize * 0.06f, textPosition.y + fontSize * 0.06f), IM_COL32(0, 0, 0, static_cast<int>(command.Color.a * 160.0f)), command.Text.c_str());
			drawList->AddText(font, fontSize, textPosition, ToColor(command.Color), command.Text.c_str());
		}
		drawList->PopClipRect();
	}

}
