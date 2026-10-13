#include "Basalt/UI/UIRenderer.h"

#include "Basalt/UI/GameUI.h"
#include "Basalt/UI/UIFonts.h"
#include "Basalt/UI/UILayout.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace Basalt {

	namespace {

		ImU32 ToColor(const glm::vec4& color)
		{
			const glm::vec4 clamped = glm::clamp(color, glm::vec4(0.0f), glm::vec4(1.0f));
			const auto channel = [](float value) { return static_cast<int>(std::lround(value * 255.0f)); };
			return IM_COL32(channel(clamped.r), channel(clamped.g), channel(clamped.b), channel(clamped.a));
		}

		ImVec2 ToImVec(const glm::vec2& value)
		{
			return { value.x, value.y };
		}

		bool IsBlank(char c)
		{
			return c == ' ' || c == '\t' || c == '\r';
		}

		// Canvas-to-screen mapping of one Draw call.
		struct CanvasMapping
		{
			glm::vec2 Origin;
			float Scale;

			glm::vec2 ToScreen(const glm::vec2& canvasPoint) const { return Origin + canvasPoint * Scale; }
			UIBox ToScreen(const UIBox& box) const { return { ToScreen(box.Min), box.Size * Scale }; }
		};

		void DrawImage(ImDrawList* drawList, const CanvasMapping& mapping, const UIDrawCommand& command, const UIBox& box, const UITextureMap& textures)
		{
			const ImU32 color = ToColor(command.Color);
			const UIBox screen = mapping.ToScreen(box);
			auto texture = command.Texture.empty() ? textures.end() : textures.find(command.Texture);
			// No image, or one that failed to load (logged by AssetManager): fill with the color.
			if (texture == textures.end() || texture->second.Handle == 0)
			{
				drawList->AddRectFilled(ToImVec(screen.Min), ToImVec(screen.Max()), color);
				return;
			}

			const ImTextureRef id(static_cast<ImTextureID>(texture->second.Handle));
			if (command.Slice == glm::vec4(0.0f))
			{
				drawList->AddImage(id, ToImVec(screen.Min), ToImVec(screen.Max()), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), color);
				return;
			}
			for (const UIQuad& quad : ComputeNineSlice(box, texture->second.Size, command.Slice, command.SliceScale))
			{
				if (quad.Box.Size.x <= 0.0f || quad.Box.Size.y <= 0.0f)
					continue;
				const UIBox patch = mapping.ToScreen(quad.Box);
				drawList->AddImage(id, ToImVec(patch.Min), ToImVec(patch.Max()), ToImVec(quad.UVMin), ToImVec(quad.UVMax), color);
			}
		}

		void DrawText(ImDrawList* drawList, const CanvasMapping& mapping, const UIDrawCommand& command, const UIBox& box)
		{
			if (command.Text.empty())
				return;
			ImFont* font = UIFonts::Get(command.Font);
			const float pixelSize = std::clamp(command.FontSize * mapping.Scale, 1.0f, UIRenderer::MaxTextPixelSize);
			const UIBox screen = mapping.ToScreen(box);
			const float wrapWidth = command.Wrap && screen.Size.x > 0.0f ? screen.Size.x : 0.0f;
			const std::vector<UITextLine> lines = UIRenderer::BreakLines(font, pixelSize, command.Text, wrapWidth);

			const float blockHeight = pixelSize * static_cast<float>(lines.size());
			const float top = AlignInBox(screen, { 0.0f, blockHeight }, command.Align).y;
			const ImU32 color = ToColor(command.Color);
			const ImU32 shadow = IM_COL32(0, 0, 0, static_cast<int>(std::clamp(command.Color.a, 0.0f, 1.0f) * 160.0f));
			const float shadowOffset = pixelSize * 0.06f;
			for (size_t i = 0; i < lines.size(); i++)
			{
				const UITextLine& line = lines[i];
				if (line.Text.empty())
					continue;
				const glm::vec2 position(AlignInBox(screen, { line.Width, 0.0f }, command.Align).x, top + pixelSize * static_cast<float>(i));
				const char* begin = line.Text.data();
				const char* end = begin + line.Text.size();
				if (command.Shadow)
					drawList->AddText(font, pixelSize, ImVec2(position.x + shadowOffset, position.y + shadowOffset), shadow, begin, end);
				drawList->AddText(font, pixelSize, ToImVec(position), color, begin, end);
			}
		}

	}

	std::vector<UITextLine> UIRenderer::BreakLines(ImFont* font, float pixelSize, std::string_view text, float wrapWidth)
	{
		std::vector<UITextLine> lines;
		const char* cursor = text.data();
		const char* end = cursor + text.size();
		while (true)
		{
			const char* lineEnd = wrapWidth > 0.0f ? font->CalcWordWrapPosition(pixelSize, cursor, end, wrapWidth) : std::find(cursor, end, '\n');
			// Always make progress, whatever the wrap position says.
			if (lineEnd <= cursor && cursor < end && *cursor != '\n')
			{
				lineEnd = cursor + 1;
				while (lineEnd < end && (static_cast<unsigned char>(*lineEnd) & 0xC0) == 0x80)
					++lineEnd;
			}
			const char* visibleEnd = lineEnd;
			while (visibleEnd > cursor && IsBlank(visibleEnd[-1]))
				--visibleEnd;
			UITextLine& line = lines.emplace_back();
			line.Text = std::string_view(cursor, static_cast<size_t>(visibleEnd - cursor));
			if (visibleEnd > cursor)
				line.Width = font->CalcTextSizeA(pixelSize, FLT_MAX, 0.0f, cursor, visibleEnd).x;

			if (lineEnd >= end)
				break;
			if (*lineEnd == '\n')
			{
				cursor = lineEnd + 1;
				continue;
			}
			// A wrapped line: the next one starts at the next word.
			cursor = lineEnd;
			while (cursor < end && IsBlank(*cursor))
				++cursor;
			if (cursor >= end)
				break;
		}
		return lines;
	}

	void UIRenderer::Draw(ImDrawList* drawList, const glm::vec2& origin, const glm::vec2& size, std::span<const UIDrawCommand> commands, const UITextureMap& textures)
	{
		if (!drawList || commands.empty() || !(size.x > 0.0f) || !(size.y > 0.0f))
			return;
		const CanvasMapping mapping{ origin, size.y / GameUI::CanvasHeight };
		const glm::vec2 canvas = GameUI::GetCanvasSize(size);
		drawList->PushClipRect(ToImVec(origin), ToImVec(origin + size), true);
		for (const UIDrawCommand& command : commands)
		{
			const UIBox box = ResolveUIBox(command, canvas);
			switch (command.Kind)
			{
				case UIDrawKind::Rect:
				{
					const UIBox screen = mapping.ToScreen(box);
					drawList->AddRectFilled(ToImVec(screen.Min), ToImVec(screen.Max()), ToColor(command.Color));
					break;
				}
				case UIDrawKind::Image:
					DrawImage(drawList, mapping, command, box, textures);
					break;
				case UIDrawKind::Text:
					DrawText(drawList, mapping, command, box);
					break;
			}
		}
		drawList->PopClipRect();
	}

	void UIFrame::Build(Scene* scene, const glm::vec2& viewportSize, bool includeImmediate)
	{
		m_Commands.clear();
		if (scene)
			UILayout::BuildCommands(*scene, GameUI::GetCanvasSize(viewportSize), m_Commands);
		if (includeImmediate)
		{
			std::vector<UIDrawCommand> immediate = GameUI::GetCommands();
			m_Commands.insert(m_Commands.end(), std::make_move_iterator(immediate.begin()), std::make_move_iterator(immediate.end()));
		}
	}

	void UIFrame::Clear()
	{
		m_Commands.clear();
		m_Textures.clear();
	}

	void UIFrame::LoadFonts() const
	{
		for (const UIDrawCommand& command : m_Commands)
		{
			if (command.Kind == UIDrawKind::Text && !command.Font.empty())
				UIFonts::Get(command.Font);
		}
	}

	void UIFrame::Draw(ImDrawList* drawList, const glm::vec2& origin, const glm::vec2& size) const
	{
		UIRenderer::Draw(drawList, origin, size, m_Commands, m_Textures);
	}

}
