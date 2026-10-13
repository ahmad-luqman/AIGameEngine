#include <doctest/doctest.h>

#include <Basalt/Core/FileSystem.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Project/Project.h>
#include <Basalt/UI/TrueTypeValidator.h>
#include <Basalt/UI/UIFonts.h>

#include "TestUtils.h"
#include "UITestUtils.h"

#include <imgui.h>

// A private copy of the stb_truetype Dear ImGui compiles (all functions static), to check its lookups
// directly on fonts the font check accepts.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace Basalt;
using namespace BasaltTest;

namespace {

	bool IsSimple(int16_t contours)
	{
		return contours > 0;
	}

	bool IsComposite(int16_t contours)
	{
		return contours < 0;
	}

	// Simple glyphs whose box rises above the baseline (letters), with room for `minLength` bytes.
	std::vector<uint32_t> FindTallSimpleGlyphs(const std::vector<uint8_t>& data, size_t minLength)
	{
		std::vector<uint32_t> glyphs = FindGlyphsWhere(data, IsSimple, minLength);
		std::erase_if(glyphs, [&](uint32_t glyph) { return GlyphTop(data, glyph) <= 0; });
		return glyphs;
	}

	// Rewrites a glyph as a simple glyph of two contours: three on-curve points, then one point whose flag is
	// `lastFlags`. Box (0, 0)..(50, 50), points (10, 10)..(40, 40).
	void MakeTwoContourGlyph(std::vector<uint8_t>& data, uint32_t glyph, uint8_t lastFlags)
	{
		const auto [offset, length] = FindGlyph(data, glyph);
		REQUIRE(length >= 28);
		std::vector<uint8_t> bytes;
		for (uint16_t value : { 2, 0, 0, 50, 50 }) // contours, xMin, yMin, xMax, yMax
			AppendU16(bytes, value);
		for (uint16_t value : { 2, 3, 0 }) // contour end points, no instructions
			AppendU16(bytes, value);
		bytes.insert(bytes.end(), { 0x37, 0x37, 0x37, lastFlags });    // on-curve, x and y as positive bytes
		bytes.insert(bytes.end(), { 10, 10, 10, 10, 10, 10, 10, 10 }); // x deltas, y deltas
		REQUIRE(bytes.size() == 28);
		std::memcpy(&data[offset], bytes.data(), bytes.size());
	}

	// Exactly `glyph` is broken, for a reason containing `fragment`; the font itself is fine.
	void ExpectBroken(const std::vector<uint8_t>& font, uint32_t glyph, const char* fragment)
	{
		const TrueTypeFontCheck check = CheckTrueTypeFont(font);
		CHECK_MESSAGE(check.Error.empty(), check.Error);
		REQUIRE(check.BrokenGlyphs.size() == 1);
		CHECK(check.BrokenGlyphs[0].Index == glyph);
		CHECK(check.BrokenGlyphs[0].Offset == FindGlyph(font, glyph).first);
		CHECK_MESSAGE(Contains(check.BrokenGlyphs[0].Problem, fragment), check.BrokenGlyphs[0].Problem);
		CHECK(ValidateTrueTypeFont(font) == "glyph " + std::to_string(glyph) + ": " + check.BrokenGlyphs[0].Problem);
	}

	struct CmapRecord
	{
		uint16_t Platform;
		uint16_t Encoding;
		std::vector<uint8_t> Subtable;
	};

	// A 'cmap' table with the given encoding records, each with its own subtable.
	std::vector<uint8_t> BuildCmap(const std::vector<CmapRecord>& records)
	{
		std::vector<uint8_t> table;
		AppendU16(table, 0);
		AppendU16(table, static_cast<uint16_t>(records.size()));
		uint32_t offset = static_cast<uint32_t>(4 + 8 * records.size());
		for (const CmapRecord& record : records)
		{
			AppendU16(table, record.Platform);
			AppendU16(table, record.Encoding);
			AppendU32(table, offset);
			offset += static_cast<uint32_t>(record.Subtable.size());
		}
		for (const CmapRecord& record : records)
			table.insert(table.end(), record.Subtable.begin(), record.Subtable.end());
		return table;
	}

	std::vector<uint8_t> CmapFormat0(const std::map<uint8_t, uint8_t>& glyphs)
	{
		std::vector<uint8_t> table;
		AppendU16(table, 0);
		AppendU16(table, 262);
		AppendU16(table, 0);
		for (int code = 0; code < 256; code++)
		{
			auto it = glyphs.find(static_cast<uint8_t>(code));
			table.push_back(it != glyphs.end() ? it->second : 0);
		}
		return table;
	}

	std::vector<uint8_t> CmapFormat6(uint16_t first, const std::vector<uint16_t>& glyphs)
	{
		std::vector<uint8_t> table;
		AppendU16(table, 6);
		AppendU16(table, static_cast<uint16_t>(10 + 2 * glyphs.size()));
		AppendU16(table, 0);
		AppendU16(table, first);
		AppendU16(table, static_cast<uint16_t>(glyphs.size()));
		for (uint16_t glyph : glyphs)
			AppendU16(table, glyph);
		return table;
	}

	struct CmapGroup
	{
		uint32_t Start;
		uint32_t End;
		uint32_t Glyph;
	};

	// Format 12 or 13; `declaredGroups` overrides the group count written in the header.
	std::vector<uint8_t> CmapFormat12(uint16_t format, const std::vector<CmapGroup>& groups, std::optional<uint32_t> declaredGroups = std::nullopt)
	{
		std::vector<uint8_t> table;
		AppendU16(table, format);
		AppendU16(table, 0);
		AppendU32(table, static_cast<uint32_t>(16 + 12 * groups.size()));
		AppendU32(table, 0);
		AppendU32(table, declaredGroups.value_or(static_cast<uint32_t>(groups.size())));
		for (const CmapGroup& group : groups)
		{
			AppendU32(table, group.Start);
			AppendU32(table, group.End);
			AppendU32(table, group.Glyph);
		}
		return table;
	}

	// Karla with its character map replaced.
	std::vector<uint8_t> KarlaWithCmap(const std::vector<CmapRecord>& records)
	{
		std::vector<uint8_t> font = ReadKarla();
		ReplaceTable(font, "cmap", BuildCmap(records));
		return font;
	}

	// Loads `font` into the current ImGui context and rasterizes printable ASCII at a few sizes.
	ImFont* LoadAndRasterize(const std::vector<uint8_t>& font)
	{
		std::string error;
		ImFont* loaded = UIFonts::LoadFromMemory(font, "Test", error);
		REQUIRE_MESSAGE(loaded, error);
		for (float size : { 8.0f, 32.0f, 200.0f })
		{
			ImFontBaked* baked = loaded->GetFontBaked(size);
			for (ImWchar c = 32; c < 127; c++)
				baked->FindGlyph(c);
		}
		return loaded;
	}

	// Codepoints (of the 16-bit ones) for which stb's own lookup gives a glyph past the last one.
	int CountGlyphsPastTheEnd(std::vector<uint8_t>& font)
	{
		stbtt_fontinfo info{};
		REQUIRE(stbtt_InitFont(&info, font.data(), 0));
		int past = 0;
		for (int codepoint = 0; codepoint <= 0xFFFF; codepoint++)
		{
			if (stbtt_FindGlyphIndex(&info, codepoint) >= info.numGlyphs)
				past++;
		}
		return past;
	}

}

TEST_SUITE("UIFonts")
{
	TEST_CASE("Font-wide problems make a font unusable, with the reason")
	{
		const std::vector<uint8_t> karla = ReadKarla();
		REQUIRE(ValidateTrueTypeFont(karla).empty());
		const auto expectError = [](const std::vector<uint8_t>& font, const char* fragment) {
			const TrueTypeFontCheck check = CheckTrueTypeFont(font);
			CHECK_MESSAGE(Contains(check.Error, fragment), check.Error);
			CHECK(check.BrokenGlyphs.empty());
		};

		expectError(std::vector<uint8_t>(50, 0), "too small");
		expectError(std::vector<uint8_t>(MaxTrueTypeFontSize + 1, 0), "larger than 32 MB");
		expectError(std::vector<uint8_t>(4096, 0x41), "not a TrueType font");
		std::vector<uint8_t> cff = karla;
		std::memcpy(cff.data(), "OTTO", 4);
		expectError(cff, "CFF");
		std::vector<uint8_t> collection = karla;
		std::memcpy(collection.data(), "ttcf", 4);
		expectError(collection, "collections");

		// Truncation cuts tables short.
		const std::vector<uint8_t> truncated(karla.begin(), karla.begin() + static_cast<std::ptrdiff_t>(karla.size() / 2));
		const std::string truncatedError = ValidateTrueTypeFont(truncated);
		CHECK_MESSAGE((Contains(truncatedError, "outside the file") || Contains(truncatedError, "truncated")), truncatedError);

		// A table directory pointing past the end of the file.
		std::vector<uint8_t> farTable = karla;
		WriteU32(farTable, FindTableRecord(farTable, "head") + 8, 0x7FFFFFF0u);
		expectError(farTable, "outside the file");

		std::vector<uint8_t> noGlyphs = karla;
		WriteU16(noGlyphs, FindTable(noGlyphs, "maxp") + 4, 0);
		expectError(noGlyphs, "no glyphs");

		std::vector<uint8_t> noMetrics = karla;
		WriteU16(noMetrics, FindTable(noMetrics, "hhea") + 34, 0);
		expectError(noMetrics, "horizontal metrics");

		std::vector<uint8_t> shortMetrics = karla;
		WriteU32(shortMetrics, FindTableRecord(shortMetrics, "hmtx") + 12, 4);
		expectError(shortMetrics, "'hmtx' table is truncated");

		// Ascent at the descent: ImGui would divide by zero.
		std::vector<uint8_t> flat = karla;
		const size_t hhea = FindTable(flat, "hhea");
		WriteU16(flat, hhea + 4, 0);
		WriteU16(flat, hhea + 6, 0);
		expectError(flat, "ascent");

		std::vector<uint8_t> badLoca = karla;
		WriteU16(badLoca, FindTable(badLoca, "head") + 50, 2);
		expectError(badLoca, "'loca' format");

		// A glyph too short for the 10-byte header a repair would blank.
		std::vector<uint8_t> shortGlyph = karla;
		REQUIRE_FALSE(HasLongLoca(shortGlyph));
		const uint32_t glyph = FindGlyphWhere(shortGlyph, IsSimple, 20);
		WriteU16(shortGlyph, FindTable(shortGlyph, "loca") + 2 * (glyph + size_t{ 1 }), static_cast<uint16_t>((LocaOffset(shortGlyph, glyph) + 4) / 2));
		expectError(shortGlyph, "too short");

		// Glyphs declaring far more points than the file could hold (repeated flags, zero deltas): checking a
		// font made of tens of thousands of them would take billions of steps, so the font is refused.
		std::vector<uint8_t> pointHeavy = karla;
		size_t packed = 0;
		for (uint32_t heavy : FindGlyphsWhere(pointHeavy, IsSimple, 60))
		{
			const auto [heavyOffset, heavyLength] = FindGlyph(pointHeavy, heavy);
			// One contour; each (flag with repeat, count) pair adds 256 points with no coordinates.
			const size_t pairs = (heavyLength - 16) / 2;
			const uint16_t points = static_cast<uint16_t>(std::min<size_t>(pairs * 256, 65536) - 1);
			WriteU16(pointHeavy, heavyOffset, 1);
			WriteU16(pointHeavy, heavyOffset + 10, points); // last point index
			WriteU16(pointHeavy, heavyOffset + 12, 0);      // no instructions
			for (size_t pair = 0; pair < pairs; pair++)
			{
				pointHeavy[heavyOffset + 14 + 2 * pair] = 0x39; // on-curve, x and y unchanged, repeated
				pointHeavy[heavyOffset + 15 + 2 * pair] = 255;
			}
			packed += size_t{ points } + 1;
		}
		REQUIRE(packed > 4 * pointHeavy.size() + 65536);
		expectError(pointHeavy, "outline points, more than a");

		// Character maps: an unsupported format, none for Unicode, a search parameter past any valid font, and a
		// character mapped past the last glyph.
		const size_t format4 = FindCmapSubtable(karla, 4);
		REQUIRE(format4 != 0);
		std::vector<uint8_t> format2 = karla;
		WriteU16(format2, format4, 2);
		CHECK(Contains(ValidateTrueTypeFont(format2), "format 2 is not supported"));
		std::vector<uint8_t> noUnicode = karla;
		const size_t cmap = FindTable(noUnicode, "cmap");
		for (size_t i = 0; i < ReadU16(noUnicode, cmap + 2); i++)
			WriteU16(noUnicode, cmap + 4 + 8 * i, 2); // ISO platform: stb uses none of these
		expectError(noUnicode, "no Unicode character map");
		std::vector<uint8_t> selector = karla;
		WriteU16(selector, format4 + 10, 17);
		expectError(selector, "entrySelector 17");
		std::vector<uint8_t> badMap = karla;
		const size_t segmentsX2 = ReadU16(badMap, format4 + 6);
		const size_t startCodes = format4 + 16 + segmentsX2;
		const size_t deltas = startCodes + segmentsX2;
		REQUIRE(ReadU16(badMap, deltas + segmentsX2) == 0); // the first segment maps through its delta
		WriteU16(badMap, deltas, static_cast<uint16_t>(GlyphCount(badMap) - ReadU16(badMap, startCodes)));
		expectError(badMap, "but the font has");
	}

	TEST_CASE("Glyph problems break only their glyph")
	{
		const std::vector<uint8_t> karla = ReadKarla();
		const std::vector<uint32_t> tall = FindTallSimpleGlyphs(karla, 28);
		REQUIRE(tall.size() > 2);

		// A box that ends at the baseline while the outline rises above it makes stb assert while rasterizing
		// (an edge lies above the bitmap's first row). Elsewhere the rasterizer clips geometry outside the box.
		std::vector<uint8_t> overBaseline = karla;
		EndBoxAtBaseline(overBaseline, tall[0]);
		ExpectBroken(overBaseline, tall[0], "rises above the baseline");
		std::vector<uint8_t> lowTop = karla;
		WriteU16(lowTop, FindGlyph(lowTop, tall[0]).first + 8, 5);
		CHECK(ValidateTrueTypeFont(lowTop).empty());

		// An inverted box would give ImGui a negative bitmap size; an oversized one would not fit the atlas.
		const size_t offset = FindGlyph(karla, tall[0]).first;
		std::vector<uint8_t> inverted = karla;
		WriteU16(inverted, offset + 6, static_cast<uint16_t>(static_cast<int16_t>(ReadU16(inverted, offset + 2)) - 1));
		ExpectBroken(inverted, tall[0], "inverted");
		std::vector<uint8_t> huge = karla;
		WriteU16(huge, offset + 2, static_cast<uint16_t>(-30000));
		WriteU16(huge, offset + 6, 30000);
		ExpectBroken(huge, tall[0], "times the line height");

		std::vector<uint8_t> unordered = karla;
		const uint32_t multi = FindGlyphWhere(unordered, [](int16_t contours) { return contours > 1; });
		WriteU16(unordered, FindGlyph(unordered, multi).first + 12, 0);
		ExpectBroken(unordered, multi, "out of order");

		// stb reads the point after a contour's off-curve first point; for the last point there is none.
		std::vector<uint8_t> offCurveLast = karla;
		MakeTwoContourGlyph(offCurveLast, tall[1], 0x36);
		ExpectBroken(offCurveLast, tall[1], "one off-curve point");
		std::vector<uint8_t> onCurveLast = karla;
		MakeTwoContourGlyph(onCurveLast, tall[1], 0x37);
		CHECK(ValidateTrueTypeFont(onCurveLast).empty());

		// Outline data cut short inside the glyph.
		std::vector<uint8_t> cut = karla;
		WriteU16(cut, FindGlyph(cut, tall[2]).first + 10 + 2 * size_t{ static_cast<uint16_t>(GlyphContours(cut, tall[2])) }, 60000); // instruction length
		ExpectBroken(cut, tall[2], "truncated");

		// A composite that contains itself would recurse until the stack overflows; point matching asserts.
		std::vector<uint8_t> cycle = karla;
		MakeComposite(cycle, tall[0], { tall[0] });
		ExpectBroken(cycle, tall[0], "form a cycle");
		std::vector<uint8_t> matching = cycle;
		WriteU16(matching, offset + 10, 0x0000);
		ExpectBroken(matching, tall[0], "point matching");
	}

	TEST_CASE("Composite glyphs: real ones pass, and only the glyph that misbehaves breaks")
	{
		const std::vector<uint8_t> cousine = ReadCousine();
		CHECK(ValidateTrueTypeFont(cousine).empty());
		const std::vector<uint32_t> composites = FindGlyphsWhere(cousine, IsComposite, 20);
		REQUIRE_FALSE(composites.empty());
		uint32_t composite = composites.front();
		for (uint32_t candidate : composites)
		{
			if (GlyphTop(cousine, candidate) > 0 && GlyphContours(cousine, ReadU16(cousine, FindGlyph(cousine, candidate).first + 12)) > 0)
			{
				composite = candidate;
				break;
			}
		}
		const size_t offset = FindGlyph(cousine, composite).first;
		const uint32_t base = ReadU16(cousine, offset + 12);
		REQUIRE((ReadU16(cousine, offset + 10) & 2) != 0); // positioned by offsets

		// A composite rising above a box that ends at the baseline (the same stb assertion as a simple glyph).
		std::vector<uint8_t> overBaseline = cousine;
		EndBoxAtBaseline(overBaseline, composite);
		ExpectBroken(overBaseline, composite, "rises above the baseline");

		// A broken component is blanked; the composites using it keep their other parts.
		std::vector<uint8_t> brokenBase = cousine;
		const size_t baseOffset = FindGlyph(brokenBase, base).first;
		WriteU16(brokenBase, baseOffset + 6, static_cast<uint16_t>(static_cast<int16_t>(ReadU16(brokenBase, baseOffset + 2)) - 1));
		ExpectBroken(brokenBase, base, "inverted");

		// A component moved far away and scaled up would overflow stb's 16-bit vertex coordinates. Rewrite the
		// first component as: 16-bit offsets (bit 0), positioned by offsets (bit 1), one scale (bit 3), last.
		std::vector<uint8_t> far = cousine;
		WriteU16(far, offset + 10, (1 << 0) | (1 << 1) | (1 << 3));
		WriteU16(far, offset + 14, 32000);  // x offset
		WriteU16(far, offset + 16, 0);      // y offset
		WriteU16(far, offset + 18, 0x7FFF); // scale ~2 (F2Dot14)
		ExpectBroken(far, composite, "beyond 16-bit coordinates");

		std::vector<uint8_t> cycle = cousine;
		WriteU16(cycle, offset + 12, static_cast<uint16_t>(composite));
		ExpectBroken(cycle, composite, "form a cycle");
		CHECK(RepairTrueTypeFont(cycle).empty());
		CHECK(ValidateTrueTypeFont(cycle).empty());
	}

	TEST_CASE("Composite nesting and expansion are bounded, and only the offending glyph is blamed")
	{
		const std::vector<uint8_t> karla = ReadKarla();
		const std::vector<uint32_t> victims = FindTallSimpleGlyphs(karla, 28);
		REQUIRE(victims.size() >= 10);

		// A chain: each glyph a composite of the one before. Eight composite levels are allowed, nine are not.
		std::vector<uint8_t> chain = karla;
		for (size_t level = 1; level <= 9; level++)
			MakeComposite(chain, victims[level], { victims[level - 1] });
		ExpectBroken(chain, victims[9], "nest more than 8 levels");

		// The same chain the other way round (the root has the lowest index, so it is resolved first): still
		// only the root is blamed, not the glyph where the depth limit is reached.
		std::vector<uint8_t> reversed = karla;
		for (size_t level = 1; level <= 9; level++)
			MakeComposite(reversed, victims[level - 1], { victims[level] });
		ExpectBroken(reversed, victims[0], "nest more than 8 levels");

		// A cycle breaks one glyph, whatever the glyph order: A -> B -> A breaks A (the glyph that comes back
		// around) and leaves B, and C using B, intact; a composite D using a self-cycle C' keeps its other part.
		std::vector<uint8_t> loop = karla;
		MakeComposite(loop, victims[0], { victims[1] });
		MakeComposite(loop, victims[1], { victims[0] });
		MakeComposite(loop, victims[2], { victims[1] });
		ExpectBroken(loop, victims[0], "form a cycle");
		std::vector<uint8_t> innocent = karla;
		MakeComposite(innocent, victims[0], { victims[2], victims[1] });
		MakeComposite(innocent, victims[1], { victims[1] });
		ExpectBroken(innocent, victims[1], "form a cycle");

		// Fan-out: each glyph uses the one before three times. stb expands composites without caching, so
		// level 7 would take 6559 shape calls; the limit is MaxShapeCalls.
		std::vector<uint8_t> fan = karla;
		for (size_t level = 1; level <= 7; level++)
			MakeComposite(fan, victims[level], { victims[level - 1], victims[level - 1], victims[level - 1] });
		ExpectBroken(fan, victims[7], "too many components");
		CHECK(RepairTrueTypeFont(fan).empty());
	}

	TEST_CASE("Long loca offsets are read like short ones")
	{
		std::vector<uint8_t> font = ReadKarla();
		REQUIRE_FALSE(HasLongLoca(font));
		std::vector<uint8_t> loca;
		for (size_t glyph = 0; glyph <= GlyphCount(font); glyph++)
			AppendU32(loca, static_cast<uint32_t>(LocaOffset(font, glyph)));
		ReplaceTable(font, "loca", loca);
		WriteU16(font, FindTable(font, "head") + 50, 1);
		REQUIRE(HasLongLoca(font));
		CHECK(ValidateTrueTypeFont(font).empty());
		{
			ImGuiTestContext imgui;
			LoadAndRasterize(font);
		}

		std::vector<uint8_t> decreasing = font;
		WriteU32(decreasing, FindTable(decreasing, "loca") + 24, static_cast<uint32_t>(LocaOffset(decreasing, 7) + 2));
		CHECK(Contains(ValidateTrueTypeFont(decreasing), "offsets decrease at glyph 6"));
	}

	TEST_CASE("Character maps of formats 0, 6, 12 and 13 are checked like format 4, and stb's choice of map is mirrored")
	{
		const uint32_t glyphs = GlyphCount(ReadKarla());
		REQUIRE(glyphs > 30);
		const auto accepted = [](const std::vector<uint8_t>& font) {
			const std::string error = ValidateTrueTypeFont(font);
			CHECK_MESSAGE(error.empty(), error);
		};
		const auto rejected = [](const std::vector<uint8_t>& font, const char* fragment) {
			const std::string error = ValidateTrueTypeFont(font);
			CHECK_MESSAGE(Contains(error, fragment), error);
		};
		const uint16_t last = static_cast<uint16_t>(glyphs - 1);

		// Format 0 (bytes), 6 (a trimmed array), 12 (groups of consecutive glyphs), 13 (one glyph per group).
		accepted(KarlaWithCmap({ { 3, 1, CmapFormat0({ { 'A', 1 }, { 'B', 2 } }) } }));
		rejected(KarlaWithCmap({ { 3, 1, CmapFormat0({ { 'A', static_cast<uint8_t>(std::min<uint32_t>(glyphs, 255)) } }) } }), "but the font has");
		accepted(KarlaWithCmap({ { 3, 1, CmapFormat6('A', { 1, 2, last }) } }));
		rejected(KarlaWithCmap({ { 3, 1, CmapFormat6('A', { 1, 2, static_cast<uint16_t>(glyphs) }) } }), "but the font has");
		std::vector<uint8_t> format12 = KarlaWithCmap({ { 3, 10, CmapFormat12(12, { { 'A', 'Z', 1 }, { 0x20AC, 0x20AC, 27 } }) } });
		accepted(format12);
		rejected(KarlaWithCmap({ { 3, 10, CmapFormat12(12, { { 'A', 'Z', glyphs - 10 } }) } }), "but the font has");
		rejected(KarlaWithCmap({ { 3, 10, CmapFormat12(12, { { 'A', 'Z', 1 } }, 1000) } }), "truncated");
		accepted(KarlaWithCmap({ { 3, 10, CmapFormat12(12, { { 'Z', 'A', 60000 } }) } })); // never matches
		accepted(KarlaWithCmap({ { 0, 4, CmapFormat12(13, { { 0x20, 0x10FFFF, 3 } }) } }));
		rejected(KarlaWithCmap({ { 0, 4, CmapFormat12(13, { { 0x20, 0x7E, glyphs } }) } }), "but the font has");
		{
			ImGuiTestContext imgui;
			ImFont* font = LoadAndRasterize(format12);
			CHECK(font->IsGlyphInFont('A'));
			CHECK(font->IsGlyphInFont(0x20AC));
			CHECK_FALSE(font->IsGlyphInFont('a'));
		}

		// stb uses the last Unicode subtable (Microsoft BMP or full repertoire, or any Unicode-platform one) and
		// ignores Macintosh ones: only that subtable matters.
		const std::vector<uint8_t> good = CmapFormat6('A', { 1, 2, 3 });
		const std::vector<uint8_t> bad = CmapFormat6('A', { 1, 2, static_cast<uint16_t>(glyphs) });
		accepted(KarlaWithCmap({ { 0, 3, bad }, { 3, 1, good } }));
		rejected(KarlaWithCmap({ { 3, 1, good }, { 0, 3, bad } }), "but the font has");
		accepted(KarlaWithCmap({ { 3, 1, good }, { 1, 0, bad } }));
		rejected(KarlaWithCmap({ { 1, 0, good } }), "no Unicode character map");
	}

	TEST_CASE("stb only finds glyphs that exist in fonts the check accepts")
	{
		// Random changes to Karla's format 4 character map (its search parameters, segment ends and starts,
		// deltas and range offsets). For every changed font the check accepts, stb's own lookup must give an
		// existing glyph for every 16-bit codepoint, and ImGui must load it; under ASan, any read outside the
		// file fails the test too.
		ImGuiTestContext imgui;
		const std::vector<uint8_t> karla = ReadKarla();
		const size_t format4 = FindCmapSubtable(karla, 4);
		REQUIRE(format4 != 0);
		const size_t length = 16 + 4 * size_t{ ReadU16(karla, format4 + 6) };

		std::mt19937 random(1234);
		int accepted = 0;
		int rejected = 0;
		for (int mutant = 0; mutant < 60; mutant++)
		{
			std::vector<uint8_t> font = karla;
			const int changes = 1 + static_cast<int>(random() % 3);
			for (int change = 0; change < changes; change++)
			{
				const size_t at = format4 + 6 + random() % (length - 6);
				font[at] = static_cast<uint8_t>(random() % 4 == 0 ? random() : font[at] ^ (1u << (random() % 8)));
			}
			std::string error;
			ImFont* loaded = UIFonts::LoadFromMemory(font, "Mutant", error);
			if (!loaded)
			{
				rejected++;
				continue;
			}
			accepted++;
			CHECK(CountGlyphsPastTheEnd(font) == 0);
			ImFontBaked* baked = loaded->GetFontBaked(20.0f);
			for (ImWchar c = 32; c < 127; c++)
				baked->FindGlyph(c);
		}
		INFO("accepted ", accepted, ", rejected ", rejected);
		CHECK(accepted > 5);
		CHECK(rejected > 5);
	}

	TEST_CASE("RepairTrueTypeFont blanks broken glyphs so ImGui can load and draw the rest of the font")
	{
		ImGuiTestContext imgui;
		const std::vector<uint8_t> karla = ReadKarla();
		std::vector<TrueTypeFontCheck::BrokenGlyph> repaired;
		std::vector<uint8_t> clean = karla;
		CHECK(RepairTrueTypeFont(clean, &repaired).empty());
		CHECK(repaired.empty());
		CHECK(clean == karla);

		// Break every glyph that rises above the baseline (so every letter is broken): one becomes a composite
		// of itself, the others get a box that ends at the baseline.
		std::vector<uint8_t> broken = karla;
		const std::vector<uint32_t> victims = FindTallSimpleGlyphs(broken, 16);
		REQUIRE(victims.size() > 50);
		MakeComposite(broken, victims[0], { victims[0] });
		for (size_t i = 1; i < victims.size(); i++)
			EndBoxAtBaseline(broken, victims[i]);

		const std::vector<uint8_t> unrepaired = broken;
		CHECK(RepairTrueTypeFont(broken, &repaired).empty());
		REQUIRE(repaired.size() == victims.size());
		CHECK(Contains(repaired[0].Problem, "form a cycle"));
		CHECK(Contains(repaired[1].Problem, "rises above the baseline"));
		CHECK(ValidateTrueTypeFont(broken).empty());
		// A blanked glyph has no contours and an empty box.
		const size_t blankedOffset = FindGlyph(broken, victims[1]).first;
		for (size_t i = 0; i < 10; i++)
			CHECK(broken[blankedOffset + i] == 0);

		// The repaired font rasterizes every printable ASCII character at several sizes (stb would assert on
		// the unrepaired glyphs). The warning lists the first five blanked glyphs.
		const uint64_t since = Log::GetHistory().GetTotalCount();
		ImFont* font = LoadAndRasterize(unrepaired);
		CHECK(CountLogMessages(since, fmt::format("{} glyph(s) cannot be drawn safely", victims.size())) == 1);
		CHECK(CountLogMessages(since, "; ...)") == 1);
		CHECK(font->CalcTextSizeA(32.0f, FLT_MAX, 0.0f, "ABC xyz").x > 0.0f);

		// Font-wide problems cannot be repaired.
		std::vector<uint8_t> noMetrics = karla;
		WriteU16(noMetrics, FindTable(noMetrics, "hhea") + 34, 0);
		CHECK(Contains(RepairTrueTypeFont(noMetrics, &repaired), "horizontal metrics"));
		CHECK(repaired.empty());
	}

	// Regression (FuzzFont): without U+2026 or U+0085, ImGui builds an ellipsis from the '.' glyph's bitmap
	// and asserted when '.' had none.
	TEST_CASE("A font whose period draws nothing loads, and asking for an ellipsis does not assert")
	{
		ImGuiTestContext imgui;
		const std::vector<uint8_t> font = ReadFont("../Fuzz/Corpus/FuzzFont/regression-blank-period.ttf");
		std::string error;
		ImFont* loaded = UIFonts::LoadFromMemory(font, "BlankPeriod", error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK_FALSE(loaded->EllipsisAutoBake);
		for (float size : { 9.0f, 40.0f })
		{
			ImFontBaked* baked = loaded->GetFontBaked(size);
			for (ImWchar c : { ImWchar('.'), ImWchar(0x2026), ImWchar(0x0085), loaded->EllipsisChar })
				baked->FindGlyph(c);
		}
	}

	TEST_CASE("UIFonts loads project fonts once, falls back to the built-in font and logs each failure once")
	{
		ImGuiTestContext imgui;
		// Loading a game font first still leaves the built-in font as ImGui's default (the atlas's first).
		std::string error;
		ImFont* karla = UIFonts::LoadFromMemory(ReadKarla(), "Karla", error);
		REQUIRE_MESSAGE(karla, error);
		CHECK(ImGui::GetIO().Fonts->Fonts[0] != karla);
		ImFont* builtIn = UIFonts::GetDefault();
		CHECK(builtIn == ImGui::GetIO().Fonts->Fonts[0]);
		CHECK(UIFonts::Get("") == builtIn);
		const float karlaWidth = karla->CalcTextSizeA(40.0f, FLT_MAX, 0.0f, "Hello").x;
		CHECK(karlaWidth > 0.0f);
		CHECK(karlaWidth != builtIn->CalcTextSizeA(40.0f, FLT_MAX, 0.0f, "Hello").x);
		CHECK_FALSE(UIFonts::LoadFromMemory(std::vector<uint8_t>(200, 0), "Junk", error));
		CHECK(Contains(error, "not a TrueType font"));

		BasaltTest::TempProject project("UIFonts");
		const std::vector<uint8_t> data = ReadKarla();
		const std::filesystem::path fonts = project.GetDirectory() / "Assets/Fonts";
		REQUIRE(FileSystem::WriteBinaryFile(fonts / "Karla.ttf", data.data(), data.size()));
		ImFont* loaded = UIFonts::Get("Assets/Fonts/Karla.ttf");
		CHECK(loaded != builtIn);
		CHECK(UIFonts::Get("Assets/Fonts/../Fonts/Karla.ttf") == loaded); // one entry per file
		CHECK(UIFonts::GetError("Assets/Fonts/Karla.ttf").empty());
		CHECK(UIFonts::CheckFile("Assets/Fonts/Karla.ttf").empty());

		// A missing file is logged once, and loaded once it appears.
		const uint64_t since = Log::GetHistory().GetTotalCount();
		CHECK(UIFonts::Get("Assets/Fonts/Missing.ttf") == builtIn);
		CHECK(UIFonts::Get("Assets/Fonts/Missing.ttf") == builtIn);
		CHECK(CountLogMessages(since, "Missing.ttf") == 1);
		CHECK(Contains(UIFonts::GetError("Assets/Fonts/Missing.ttf"), "does not exist or is not a file"));
		REQUIRE(FileSystem::WriteBinaryFile(fonts / "Missing.ttf", data.data(), data.size()));
		CHECK(UIFonts::Get("Assets/Fonts/Missing.ttf") != builtIn);
		CHECK(UIFonts::GetError("Assets/Fonts/Missing.ttf").empty());

		// An invalid file, fixed on disk, loads on the next use.
		const std::filesystem::path fake = fonts / "Fake.ttf";
		REQUIRE(FileSystem::WriteTextFile(fake, std::string(500, 'x')));
		const uint64_t fakeSince = Log::GetHistory().GetTotalCount();
		CHECK(UIFonts::Get("Assets/Fonts/Fake.ttf") == builtIn);
		CHECK(UIFonts::Get("Assets/Fonts/Fake.ttf") == builtIn); // unchanged: not read again
		CHECK(CountLogMessages(fakeSince, "Fake.ttf") == 1);
		CHECK(Contains(UIFonts::GetError("Assets/Fonts/Fake.ttf"), "not a TrueType font"));
		CHECK(Contains(UIFonts::CheckFile("Assets/Fonts/Fake.ttf"), "not a TrueType font"));
		REQUIRE(FileSystem::WriteBinaryFile(fake, data.data(), data.size()));
		std::filesystem::last_write_time(fake, std::filesystem::last_write_time(fake) + std::chrono::hours(1));
		CHECK(UIFonts::GetError("Assets/Fonts/Fake.ttf").empty()); // changed since: no longer the error
		CHECK(UIFonts::Get("Assets/Fonts/Fake.ttf") != builtIn);

		// Directories and oversized files are refused before they are read.
		std::filesystem::create_directories(fonts / "Folder.ttf");
		CHECK(UIFonts::Get("Assets/Fonts/Folder.ttf") == builtIn);
		CHECK(Contains(UIFonts::GetError("Assets/Fonts/Folder.ttf"), "is not a file"));
		REQUIRE(FileSystem::WriteTextFile(fonts / "Huge.ttf", "x"));
		std::filesystem::resize_file(fonts / "Huge.ttf", MaxTrueTypeFontSize + 1);
		CHECK(UIFonts::Get("Assets/Fonts/Huge.ttf") == builtIn);
		CHECK(Contains(UIFonts::GetError("Assets/Fonts/Huge.ttf"), "larger than 32 MB"));
		// Exactly the limit is read (and then checked).
		REQUIRE(FileSystem::WriteTextFile(fonts / "Limit.ttf", "x"));
		std::filesystem::resize_file(fonts / "Limit.ttf", MaxTrueTypeFontSize);
		CHECK(Contains(UIFonts::CheckFile("Assets/Fonts/Limit.ttf"), "not a TrueType font"));

		// A font with blanked glyphs loads, with a warning that the inspector and ui.layout show.
		std::vector<uint8_t> repairable = data;
		EndBoxAtBaseline(repairable, FindTallSimpleGlyphs(repairable, 10).front());
		REQUIRE(FileSystem::WriteBinaryFile(fonts / "Repairable.ttf", repairable.data(), repairable.size()));
		CHECK(UIFonts::Get("Assets/Fonts/Repairable.ttf") != builtIn);
		CHECK(Contains(UIFonts::GetWarning("Assets/Fonts/Repairable.ttf"), "1 glyph(s) cannot be drawn safely"));
		CHECK(Contains(UIFonts::CheckFile("Assets/Fonts/Repairable.ttf"), "1 glyph(s) cannot be drawn safely"));
		CHECK(UIFonts::GetWarning("Assets/Fonts/Karla.ttf").empty());

		// Clearing forgets failures.
		UIFonts::Clear();
		CHECK(UIFonts::GetError("Assets/Fonts/Huge.ttf").empty());
	}

	TEST_CASE("UIFonts forgets a destroyed context's fonts, even without Clear")
	{
		BasaltTest::TempProject project("UIFontsContexts");
		const std::vector<uint8_t> data = ReadKarla();
		REQUIRE(FileSystem::WriteBinaryFile(project.GetDirectory() / "Assets/Fonts/Karla.ttf", data.data(), data.size()));
		{
			ImGuiTestContext first;
			REQUIRE(UIFonts::Get("Assets/Fonts/Karla.ttf") != UIFonts::GetDefault());
		}
		// The new context may reuse the old one's address; its atlas must still get its own font.
		ImGuiTestContext second;
		ImFont* font = UIFonts::Get("Assets/Fonts/Karla.ttf");
		CHECK(ImGui::GetIO().Fonts->Fonts.contains(font));
		CHECK(font != UIFonts::GetDefault());
	}
}
