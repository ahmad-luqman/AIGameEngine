#pragma once

// Helpers shared by the UI tests (UITests.cpp) and the font tests (UIFontTests.cpp): an ImGui context without
// a GPU, the test fonts, and big-endian access into font files for tests that corrupt or rebuild one table.

#include <Basalt/Core/FileSystem.h>
#include <Basalt/Core/Log.h>
#include <Basalt/UI/GameUI.h>
#include <Basalt/UI/UIFonts.h>

#include <doctest/doctest.h>
#include <imgui.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace BasaltTest {

	inline std::vector<uint8_t> ReadFont(const std::filesystem::path& relativeToTestData)
	{
		std::optional<std::vector<uint8_t>> data = Basalt::FileSystem::ReadBinaryFile(std::filesystem::path(BASALT_TEST_DATA_DIR) / relativeToTestData);
		REQUIRE_MESSAGE(data, relativeToTestData.string());
		return *data;
	}

	// Simple glyphs only.
	inline std::vector<uint8_t> ReadKarla()
	{
		return ReadFont("FeatureTest/Assets/Fonts/Karla-Regular.ttf");
	}

	// The FuzzFont seed with composite glyphs.
	inline std::vector<uint8_t> ReadCousine()
	{
		return ReadFont("../Fuzz/Corpus/FuzzFont/Cousine-Regular.ttf");
	}

	// An ImGui context for font and draw tests: no window or GPU, but the dynamic font atlas works on the CPU
	// once a renderer claims to handle textures. Destroying it clears UIFonts (through UIFonts' shutdown hook).
	class ImGuiTestContext
	{
	public:
		ImGuiTestContext()
		{
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
			io.IniFilename = nullptr;
			io.DisplaySize = ImVec2(1920.0f, 1080.0f);
			io.DeltaTime = 1.0f / 60.0f;
		}
		~ImGuiTestContext() { ImGui::DestroyContext(); }

		ImGuiTestContext(const ImGuiTestContext&) = delete;
		ImGuiTestContext& operator=(const ImGuiTestContext&) = delete;

		// Pretends a renderer uploaded the font atlas, so ImDrawCmd::GetTexID() works (call inside a frame).
		static void MarkTexturesUploaded()
		{
			// The atlas's own list: textures it created since the frame began are not in the platform list yet.
			for (ImTextureData* texture : ImGui::GetIO().Fonts->TexList)
			{
				if (texture->TexID == ImTextureID_Invalid)
					texture->SetTexID(static_cast<ImTextureID>(1));
				texture->SetStatus(ImTextureStatus_OK);
			}
		}
	};

	inline uint16_t ReadU16(const std::vector<uint8_t>& data, size_t at)
	{
		return static_cast<uint16_t>((data[at] << 8) | data[at + 1]);
	}

	inline uint32_t ReadU32(const std::vector<uint8_t>& data, size_t at)
	{
		return (static_cast<uint32_t>(ReadU16(data, at)) << 16) | ReadU16(data, at + 2);
	}

	inline void WriteU16(std::vector<uint8_t>& data, size_t at, uint16_t value)
	{
		data[at] = static_cast<uint8_t>(value >> 8);
		data[at + 1] = static_cast<uint8_t>(value & 0xFF);
	}

	inline void WriteU32(std::vector<uint8_t>& data, size_t at, uint32_t value)
	{
		WriteU16(data, at, static_cast<uint16_t>(value >> 16));
		WriteU16(data, at + 2, static_cast<uint16_t>(value & 0xFFFF));
	}

	inline void AppendU16(std::vector<uint8_t>& data, uint16_t value)
	{
		data.push_back(static_cast<uint8_t>(value >> 8));
		data.push_back(static_cast<uint8_t>(value & 0xFF));
	}

	inline void AppendU32(std::vector<uint8_t>& data, uint32_t value)
	{
		AppendU16(data, static_cast<uint16_t>(value >> 16));
		AppendU16(data, static_cast<uint16_t>(value & 0xFFFF));
	}

	// Offset of a table's directory record (0 when the font has none).
	inline size_t FindTableRecord(const std::vector<uint8_t>& data, const char* tag)
	{
		const uint16_t tables = ReadU16(data, 4);
		for (size_t i = 0; i < tables; i++)
		{
			const size_t record = 12 + 16 * i;
			if (std::memcmp(&data[record], tag, 4) == 0)
				return record;
		}
		return 0;
	}

	// Offset of a table in the file (0 when the font has none).
	inline size_t FindTable(const std::vector<uint8_t>& data, const char* tag)
	{
		const size_t record = FindTableRecord(data, tag);
		return record ? ReadU32(data, record + 8) : 0;
	}

	// Appends `table` (4-byte aligned) and points the font's `tag` record at it.
	inline void ReplaceTable(std::vector<uint8_t>& data, const char* tag, const std::vector<uint8_t>& table)
	{
		const size_t record = FindTableRecord(data, tag);
		REQUIRE(record != 0);
		while (data.size() % 4 != 0)
			data.push_back(0);
		const size_t offset = data.size();
		data.insert(data.end(), table.begin(), table.end());
		WriteU32(data, record + 8, static_cast<uint32_t>(offset));
		WriteU32(data, record + 12, static_cast<uint32_t>(table.size()));
	}

	inline uint32_t GlyphCount(const std::vector<uint8_t>& data)
	{
		return ReadU16(data, FindTable(data, "maxp") + 4);
	}

	inline bool HasLongLoca(const std::vector<uint8_t>& data)
	{
		return ReadU16(data, FindTable(data, "head") + 50) == 1;
	}

	// A glyph's data offset in the loca table's units resolved to bytes (relative to 'glyf').
	inline size_t LocaOffset(const std::vector<uint8_t>& data, size_t index)
	{
		const size_t loca = FindTable(data, "loca");
		return HasLongLoca(data) ? ReadU32(data, loca + 4 * index) : size_t{ ReadU16(data, loca + 2 * index) } * 2;
	}

	// Offset of a glyph's data (in the file) and its length.
	inline std::pair<size_t, size_t> FindGlyph(const std::vector<uint8_t>& data, uint32_t glyph)
	{
		return { FindTable(data, "glyf") + LocaOffset(data, glyph), LocaOffset(data, glyph + size_t{ 1 }) - LocaOffset(data, glyph) };
	}

	inline int16_t GlyphContours(const std::vector<uint8_t>& data, uint32_t glyph)
	{
		return static_cast<int16_t>(ReadU16(data, FindGlyph(data, glyph).first));
	}

	inline int16_t GlyphTop(const std::vector<uint8_t>& data, uint32_t glyph)
	{
		return static_cast<int16_t>(ReadU16(data, FindGlyph(data, glyph).first + 8));
	}

	// Every glyph with at least `minLength` bytes whose contour count passes `predicate` (simple: > 0,
	// composite: < 0), in glyph order.
	template<typename Predicate>
	std::vector<uint32_t> FindGlyphsWhere(const std::vector<uint8_t>& data, Predicate predicate, size_t minLength = 10)
	{
		std::vector<uint32_t> glyphs;
		for (uint32_t glyph = 0; glyph < GlyphCount(data); glyph++)
		{
			if (FindGlyph(data, glyph).second >= minLength && predicate(GlyphContours(data, glyph)))
				glyphs.push_back(glyph);
		}
		return glyphs;
	}

	template<typename Predicate>
	uint32_t FindGlyphWhere(const std::vector<uint8_t>& data, Predicate predicate, size_t minLength = 10)
	{
		const std::vector<uint32_t> glyphs = FindGlyphsWhere(data, predicate, minLength);
		REQUIRE_MESSAGE(!glyphs.empty(), "no such glyph in the font");
		return glyphs.front();
	}

	// The first subtable of the given format in the font's cmap (file offset; 0 when there is none).
	inline size_t FindCmapSubtable(const std::vector<uint8_t>& data, uint16_t format)
	{
		const size_t cmap = FindTable(data, "cmap");
		for (size_t i = 0; i < ReadU16(data, cmap + 2); i++)
		{
			const size_t subtable = cmap + ReadU32(data, cmap + 4 + 8 * i + 4);
			if (ReadU16(data, subtable) == format)
				return subtable;
		}
		return 0;
	}

	// Rewrites a glyph's data in place (keeping its bounding box) as a composite of `components`, each placed
	// at offset (0, 0). The glyph needs 10 + 6 bytes per component.
	inline void MakeComposite(std::vector<uint8_t>& data, uint32_t glyph, const std::vector<uint32_t>& components)
	{
		const auto [offset, length] = FindGlyph(data, glyph);
		REQUIRE(length >= 10 + 6 * components.size());
		WriteU16(data, offset, 0xFFFF); // numberOfContours = -1
		size_t at = offset + 10;
		for (size_t i = 0; i < components.size(); i++)
		{
			// ARGS_ARE_XY_VALUES with byte offsets, MORE_COMPONENTS on all but the last.
			WriteU16(data, at, static_cast<uint16_t>(0x0002 | (i + 1 < components.size() ? 0x0020 : 0)));
			WriteU16(data, at + 2, static_cast<uint16_t>(components[i]));
			WriteU16(data, at + 4, 0);
			at += 6;
		}
	}

	// Moves a glyph's box to end at the baseline (yMax = 0, yMin = -300).
	inline void EndBoxAtBaseline(std::vector<uint8_t>& data, uint32_t glyph)
	{
		const size_t offset = FindGlyph(data, glyph).first;
		WriteU16(data, offset + 4, static_cast<uint16_t>(-300));
		WriteU16(data, offset + 8, 0);
	}

	inline bool Contains(const std::string& text, const char* fragment)
	{
		return text.find(fragment) != std::string::npos;
	}

	// Number of log messages containing `fragment`.
	inline size_t CountLogMessages(const char* fragment)
	{
		size_t count = 0;
		for (const Basalt::LogMessage& message : Basalt::Log::GetHistory().GetAll())
		{
			if (Contains(message.Text, fragment))
				count++;
		}
		return count;
	}

	inline std::vector<Basalt::UIDrawCommand> GetImmediateCommands()
	{
		std::vector<Basalt::UIDrawCommand> commands;
		Basalt::GameUI::AppendCommands(commands);
		return commands;
	}

}
