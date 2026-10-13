#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Basalt {

	// Checks TrueType font files before Dear ImGui's font loader (stb_truetype) sees them. stb trusts the
	// file: it reads tables, glyph outlines and character maps without bounds checks, follows composite
	// glyphs recursively, and asserts on formats and geometry it does not handle. The check follows every read
	// stb makes for a font.
	//
	// Problems with the font as a whole (truncated tables, a character map that reads out of bounds or maps
	// to glyphs that do not exist, unsupported formats) make it unusable. Problems confined to one glyph
	// (malformed outline data, composite cycles or excessive nesting, oversized or inverted boxes) only make
	// that glyph unusable: RepairTrueTypeFont blanks such glyphs, so the font loads and they draw nothing.
	//
	// Supported: single TrueType-outline fonts (.ttf, or .otf with glyf outlines) with a Unicode cmap of
	// format 0, 4, 6, 12 or 13. Rejected with a reason: collections (.ttc), CFF outlines, other cmaps.

	// Largest font file accepted (bytes).
	constexpr size_t MaxTrueTypeFontSize = size_t{ 32 } * 1024 * 1024;
	// Largest glyph width or height, in multiples of the font's line height (ascent - descent): keeps glyph
	// bitmaps inside the font atlas at the largest UI text size.
	constexpr int MaxGlyphExtentPerLineHeight = 3;
	// Deepest composite-glyph nesting (real fonts use one or two levels).
	constexpr int MaxCompositeGlyphDepth = 8;

	struct TrueTypeFontCheck
	{
		struct BrokenGlyph
		{
			uint32_t Index = 0;
			// Offset of the glyph's data in the file (where RepairTrueTypeFont blanks it).
			uint64_t Offset = 0;
			std::string Problem;
		};

		// What makes the whole font unusable; empty when it can be loaded (after blanking BrokenGlyphs).
		std::string Error;
		// Glyphs stb cannot draw safely, in glyph order (empty when Error is set).
		std::vector<BrokenGlyph> BrokenGlyphs;
	};

	TrueTypeFontCheck CheckTrueTypeFont(std::span<const uint8_t> data);

	// The first problem found (a font-wide one, or "glyph N: ..."); empty when stb can load the font as is.
	std::string ValidateTrueTypeFont(std::span<const uint8_t> data);

	// Makes `data` safe to load: blanks every broken glyph in place (no contours, empty box). Returns the
	// font-wide problem when it cannot be made safe; `outRepaired` (optional) receives the blanked glyphs.
	std::string RepairTrueTypeFont(std::vector<uint8_t>& data, std::vector<TrueTypeFontCheck::BrokenGlyph>* outRepaired = nullptr);

}
