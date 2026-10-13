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
	// loads the frame's fonts in OnRender, before the ImGui frame, so drawing reads no font files (ImGui still
	// rasterizes glyphs into the atlas on demand). Main (ImGui) thread only.
	class UIFonts
	{
	public:
		// The font for a project-relative (or absolute) .ttf path, loaded on first use. An empty path, or a
		// file that is missing or invalid, gives the default font. Each failure is logged once; the file is
		// tried again once it changes (or appears). A font that loaded is kept even if its file changes.
		static ImFont* Get(const std::string& path);

		// The built-in font (added to the atlas if it has none yet).
		static ImFont* GetDefault();

		// Repairs `data` and adds it to the atlas under `name`; nullptr (with `outError` set) when it is not a
		// font ImGui can load safely. `outWarning` (optional) receives the blanked glyphs, if any.
		static ImFont* LoadFromMemory(std::vector<uint8_t> data, const std::string& name, std::string& outError, std::string* outWarning = nullptr);

		// What would make the font file at `path` draw differently than authored, without an ImGui context (for
		// headless tools): why it does not load, or which glyphs draw blank; empty when it is fine. Reads and
		// checks the file each time.
		static std::string CheckFile(const std::string& path);

		// The error of a path that failed to load; empty when it loaded, was never requested, or has changed
		// on disk since (the next Get tries again).
		static std::string GetError(const std::string& path);
		// The glyphs of a loaded font that draw blank; empty when there are none.
		static std::string GetWarning(const std::string& path);

		// Forgets every cached font and failure (the atlas keeps the fonts). The cache also empties itself when
		// its ImGui context is destroyed or another context becomes current.
		static void Clear();
	};

}
