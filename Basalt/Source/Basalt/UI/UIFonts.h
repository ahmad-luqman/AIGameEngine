#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ImFont;
struct ImGuiContext;

namespace Basalt {

	// Project TrueType fonts for the game UI, loaded into the current ImGui context's font atlas once per file.
	// ImGui rasterizes glyphs on demand at any size, so one entry serves every font size. Every file is checked
	// with ValidateTrueTypeFont before ImGui parses it. Needs an ImGui context; load fonts outside an ImGui
	// frame (UIFrame::Prepare does, in OnRender). Glyphs stb cannot draw safely are blanked (RepairTrueTypeFont)
	// with a warning; a font that cannot be made safe is not loaded.
	class UIFonts
	{
	public:
		// The font for a project-relative (or absolute) .ttf path, loaded on first use. An empty path, or a
		// file that is missing or invalid, gives the default font; each failure is logged once (until Clear).
		static ImFont* Get(const std::string& path);

		// The built-in font (added to the atlas if it has none yet).
		static ImFont* GetDefault();

		// Repairs a copy of `data` and adds it to the atlas under `name`; nullptr (with `outError` set) when the
		// data is not a font ImGui can load safely.
		static ImFont* LoadFromMemory(const std::vector<uint8_t>& data, const std::string& name, std::string& outError);

		// The error of a path that failed to load; empty when it loaded or was never requested.
		static std::string GetError(const std::string& path);

		// Forgets every cached font and failure (the atlas keeps the fonts). Call before destroying the ImGui
		// context; a change of context also resets the cache.
		static void Clear();
	};

}
