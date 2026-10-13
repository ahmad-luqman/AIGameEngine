#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ImFont;

namespace Basalt {

	// Project TrueType fonts for the game UI, loaded into the current ImGui context's font atlas once per file.
	// ImGui rasterizes glyphs on demand at any size, so one entry serves every font size. Every file goes
	// through RepairTrueTypeFont before ImGui parses it: glyphs stb cannot draw safely are blanked (with a
	// warning), and a font that cannot be made safe is not loaded. Needs an ImGui context. UIFrame::Prepare
	// loads the frame's fonts in OnRender, before the ImGui frame, so drawing never adds to the atlas.
	class UIFonts
	{
	public:
		// The font for a project-relative (or absolute) .ttf path, loaded on first use. An empty path, or a
		// file that is missing or invalid, gives the default font. Each failure is logged once; the file is
		// tried again once it changes (or appears).
		static ImFont* Get(const std::string& path);

		// The built-in font (added to the atlas if it has none yet).
		static ImFont* GetDefault();

		// Repairs a copy of `data` and adds it to the atlas under `name`; nullptr (with `outError` set) when the
		// data is not a font ImGui can load safely.
		static ImFont* LoadFromMemory(std::vector<uint8_t> data, const std::string& name, std::string& outError);

		// Whether the font file at `path` would load, without an ImGui context (for headless tools): why not,
		// or an empty string. Reads and checks the file each time.
		static std::string CheckFile(const std::string& path);

		// The error of a path that failed to load; empty when it loaded or was never requested.
		static std::string GetError(const std::string& path);

		// Forgets every cached font and failure (the atlas keeps the fonts). Runs by itself when the cached
		// fonts' ImGui context is destroyed; a change of current context also resets the cache.
		static void Clear();
	};

}
