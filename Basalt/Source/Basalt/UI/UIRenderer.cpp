#include "Basalt/UI/UIRenderer.h"

#include "Basalt/Asset/AssetManager.h"
#include "Basalt/UI/GameUI.h"
#include "Basalt/UI/UIFonts.h"
#include "Basalt/UI/UILayout.h"

#include <imgui.h>
#include <imgui_internal.h>

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

		void DrawImage(ImDrawList* drawList, const UICanvasMapping& mapping, const UIDrawCommand& command, const UIBox& box, const UIResources& resources)
		{
			const ImU32 color = ToColor(command.Color);
			const UIBox screen = mapping.ToScreen(box);
			auto texture = command.Texture.empty() ? resources.Textures.end() : resources.Textures.find(command.Texture);
			// No image, or one that failed to load or upload (logged once): fill with the color.
			if (texture == resources.Textures.end() || texture->second.Handle == 0)
			{
				drawList->AddRectFilled(ToImVec(screen.Min), ToImVec(screen.Max()), color);
				return;
			}

			const ImTextureRef id(static_cast<ImTextureID>(texture->second.Handle));
			const uint32_t columns = std::clamp(command.SpriteColumns, 1u, 4096u), rows = std::clamp(command.SpriteRows, 1u, 4096u);
			const uint32_t frame = std::min(command.SpriteFrame, columns * rows - 1);
			const glm::vec2 uvSize = (command.UVMax - command.UVMin) / glm::vec2(columns, rows);
			const glm::vec2 uvMin = command.UVMin + glm::vec2(frame % columns, frame / columns) * uvSize;
			if (command.Slice == glm::vec4(0.0f))
			{
				drawList->AddImage(id, ToImVec(screen.Min), ToImVec(screen.Max()), ToImVec(uvMin), ToImVec(uvMin + uvSize), color);
				return;
			}
			for (const UIQuad& quad : ComputeNineSlice(box, texture->second.Size * glm::abs(uvSize), command.Slice, command.SliceScale))
			{
				// Written to skip NaN sizes too.
				if (!(quad.Box.Size.x > 0.0f) || !(quad.Box.Size.y > 0.0f))
					continue;
				const UIBox patch = mapping.ToScreen(quad.Box);
				drawList->AddImage(id, ToImVec(patch.Min), ToImVec(patch.Max()), ToImVec(uvMin + quad.UVMin * uvSize), ToImVec(uvMin + quad.UVMax * uvSize), color);
			}
		}

		void DrawText(ImDrawList* drawList, const UICanvasMapping& mapping, const UIDrawCommand& command, const UIBox& box, const UIResources& resources)
		{
			if (command.Text.empty())
				return;
			auto loaded = command.Font.empty() ? resources.Fonts.end() : resources.Fonts.find(command.Font);
			ImFont* font = loaded != resources.Fonts.end() ? loaded->second : UIFonts::GetDefault();
			// Glyphs are rasterized at the font's current density. ImGui sets it only on fonts it pushes, so set
			// the display's density (the framebuffer scale) for this draw: text stays sharp on HiDPI displays.
			const float density = std::max(ImGui::GetFontRasterizerDensity(), 1.0f);
			const float previousDensity = font->CurrentRasterizerDensity;
			font->CurrentRasterizerDensity = density;
			const float pixelSize = std::clamp(command.FontSize * mapping.Scale, 1.0f, UIRenderer::MaxTextPixelSize / density);
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
			font->CurrentRasterizerDensity = previousDensity;
		}

	}

	glm::vec2 UIRenderer::MeasureText(const std::string& text, float fontSize, const std::string& fontPath, float wrapWidth)
	{
		if (text.empty())
			return glm::vec2(0);
		ImGuiContext* temporary = nullptr;
		if (!ImGui::GetCurrentContext())
		{
			temporary = ImGui::CreateContext();
			ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
			ImGui::GetIO().IniFilename = nullptr;
		}
		ImFont* font = UIFonts::Get(fontPath);
		fontSize = std::clamp(std::isfinite(fontSize) ? fontSize : UITextComponent::DefaultFontSize, UITextComponent::MinFontSize, UITextComponent::MaxFontSize);
		const auto lines = BreakLines(font, fontSize, text, std::isfinite(wrapWidth) ? std::max(wrapWidth, 0.0f) : 0.0f);
		glm::vec2 size(0.0f, fontSize * static_cast<float>(lines.size()));
		for (const auto& line : lines)
			size.x = std::max(size.x, line.Width);
		if (temporary)
			ImGui::DestroyContext(temporary);
		return size;
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
			// A wrapped line: the next one starts at the next word. Blanks before a newline end with it, as in
			// ImGui's own wrapping, rather than leave an empty line.
			cursor = lineEnd;
			while (cursor < end && IsBlank(*cursor))
				++cursor;
			if (cursor >= end)
				break;
			if (*cursor == '\n')
				++cursor;
		}
		return lines;
	}

	void UIRenderer::Draw(ImDrawList* drawList, const glm::vec2& origin, const glm::vec2& size, const glm::vec2& canvasSize, std::span<const UIDrawCommand> commands, const UIResources& resources)
	{
		if (!drawList || commands.empty() || !(size.x > 0.0f) || !(size.y > 0.0f))
			return;
		const UICanvasMapping mapping = UICanvasMapping::ForViewport(origin, size);
		drawList->PushClipRect(ToImVec(origin), ToImVec(origin + size), true);
		for (const UIDrawCommand& command : commands)
		{
			if (command.Clip)
			{
				const UIBox clip = mapping.ToScreen(*command.Clip);
				if (!(clip.Size.x > 0) || !(clip.Size.y > 0))
					continue;
				drawList->PushClipRect(ToImVec(clip.Min), ToImVec(clip.Max()), true);
			}
			const UIBox box = ResolveUIBox(command, canvasSize);
			UIRectComponent local;
			local.Pivot = command.Pivot;
			local.Rotation = command.Rotation;
			local.Scale = command.Scale;
			const glm::mat3 transform = command.Transform * ComputeUITransform(box, local);
			if (!IsUsableUITransform(box, transform))
			{
				if (command.Clip)
					drawList->PopClipRect();
				continue;
			}
			const bool transformed = transform != glm::mat3(1.0f);
			const ImVec4 actualClip = drawList->_CmdHeader.ClipRect;
			if (transformed)
				drawList->PushClipRect(ImVec2(-FLT_MAX, -FLT_MAX), ImVec2(FLT_MAX, FLT_MAX), false);
			const int commandBegin = drawList->CmdBuffer.Size - 1;
			const int vertexBegin = drawList->VtxBuffer.Size;
			switch (command.Kind)
			{
				case UIDrawKind::Rect:
				{
					const UIBox screen = mapping.ToScreen(box);
					drawList->AddRectFilled(ToImVec(screen.Min), ToImVec(screen.Max()), ToColor(command.Color));
					break;
				}
				case UIDrawKind::Image:
					DrawImage(drawList, mapping, command, box, resources);
					break;
				case UIDrawKind::Text:
					DrawText(drawList, mapping, command, box, resources);
					break;
			}
			bool validVertices = true;
			if (transformed)
			{
				for (int i = vertexBegin; i < drawList->VtxBuffer.Size; ++i)
				{
					ImDrawVert& vertex = drawList->VtxBuffer[i];
					const glm::vec2 canvas = mapping.ToCanvas({ vertex.pos.x, vertex.pos.y });
					vertex.pos = ToImVec(mapping.ToScreen(glm::vec2(transform * glm::vec3(canvas, 1.0f))));
					validVertices = validVertices && std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y) && std::abs(vertex.pos.x) <= MaxUICoordinate && std::abs(vertex.pos.y) <= MaxUICoordinate;
				}
			}
			if (!validVertices)
			{
				for (int i = vertexBegin; i < drawList->VtxBuffer.Size; ++i)
					drawList->VtxBuffer[i].pos = ImVec2(0, 0);
			}
			if (transformed)
			{
				// Text's CPU glyph culling must see the untransformed positions; the GPU scissor uses the final clip.
				for (int i = commandBegin; i < drawList->CmdBuffer.Size; ++i)
					drawList->CmdBuffer[i].ClipRect = validVertices ? actualClip : ImVec4(0, 0, 0, 0);
				drawList->PopClipRect();
			}
			if (command.Clip)
				drawList->PopClipRect();
		}
		drawList->PopClipRect();
	}

	void UIFrame::Build(Scene* scene, const glm::vec2& viewportSize, bool includeImmediate)
	{
		// Camera-less menu scenes still need a publication fence before resolving UI textures.
		AssetManager::PublishCompleted();
		m_Commands.clear();
		m_Resources = {};
		m_CanvasSize = GameUI::GetCanvasSize(viewportSize);
		if (scene)
			UILayout::BuildCommands(*scene, m_CanvasSize, m_Commands);
		if (includeImmediate)
			GameUI::AppendCommands(m_Commands);
	}

	void UIFrame::Clear()
	{
		m_Commands.clear();
		m_Resources = {};
	}

	void UIFrame::LoadFonts()
	{
		for (const UIDrawCommand& command : m_Commands)
		{
			if (command.Kind == UIDrawKind::Text && !command.Font.empty() && !m_Resources.Fonts.contains(command.Font))
				m_Resources.Fonts.emplace(command.Font, UIFonts::Get(command.Font));
		}
	}

	void UIFrame::Draw(ImDrawList* drawList, const glm::vec2& origin, const glm::vec2& size) const
	{
		UIRenderer::Draw(drawList, origin, size, m_CanvasSize, m_Commands, m_Resources);
	}

}
