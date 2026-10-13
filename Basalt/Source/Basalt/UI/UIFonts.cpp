#include "Basalt/UI/UIFonts.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Project/Project.h"
#include "Basalt/UI/TrueTypeValidator.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <optional>
#include <unordered_map>

namespace Basalt {

	namespace {

		struct FontEntry
		{
			ImFont* Font = nullptr;
			std::string Error;
		};

		// Keyed by the resolved file path, so the same file named two ways loads once and another project's
		// file with the same relative path is a different font.
		std::unordered_map<std::string, FontEntry> s_Fonts;
		ImGuiContext* s_Context = nullptr;

		// Fonts belong to a context's atlas: a cache filled for another context is stale.
		void SyncContext()
		{
			ImGuiContext* context = ImGui::GetCurrentContext();
			if (context == s_Context)
				return;
			s_Fonts.clear();
			s_Context = context;
		}

		std::string ResolveKey(const std::string& path)
		{
			return FileSystem::PathToUtf8(Project::ResolvePath(path).lexically_normal());
		}

	}

	ImFont* UIFonts::GetDefault()
	{
		ImGuiIO& io = ImGui::GetIO();
		if (io.FontDefault)
			return io.FontDefault;
		if (!io.Fonts->Fonts.empty())
			return io.Fonts->Fonts[0];
		return io.Fonts->AddFontDefault();
	}

	ImFont* UIFonts::LoadFromMemory(const std::vector<uint8_t>& data, const std::string& name, std::string& outError)
	{
		std::vector<uint8_t> font = data;
		std::vector<TrueTypeFontCheck::BrokenGlyph> repaired;
		if (std::string problem = RepairTrueTypeFont(font, &repaired); !problem.empty())
		{
			outError = std::move(problem);
			return nullptr;
		}
		if (!repaired.empty())
			BS_CORE_WARN("UI font '{}': {} glyph(s) cannot be drawn safely and draw blank (glyph {}: {})", name, repaired.size(), repaired.front().Index, repaired.front().Problem);

		// ImGui's default font is the atlas's first one: make sure that is the built-in font, not a game font
		// loaded before ImGui's first frame (the editor's own UI would switch to it).
		GetDefault();

		// The atlas reads the data whenever it rasterizes new glyphs, so it takes ownership of a copy.
		void* copy = IM_ALLOC(font.size());
		std::memcpy(copy, font.data(), font.size());
		ImFontConfig config;
		config.FontDataOwnedByAtlas = true;
		std::snprintf(config.Name, sizeof(config.Name), "%s", name.c_str());
		// The size only matters to code that draws with the font's default size; the UI always passes one.
		ImFont* loaded = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(copy, static_cast<int>(font.size()), 32.0f, &config);
		if (!loaded)
			outError = "ImGui could not load the font";
		return loaded;
	}

	ImFont* UIFonts::Get(const std::string& path)
	{
		if (path.empty())
			return GetDefault();
		SyncContext();
		const std::string key = ResolveKey(path);
		auto it = s_Fonts.find(key);
		if (it == s_Fonts.end())
		{
			FontEntry entry;
			std::optional<std::vector<uint8_t>> data = FileSystem::ReadBinaryFile(key);
			if (!data)
				entry.Error = "the file could not be read";
			else
				entry.Font = LoadFromMemory(*data, path, entry.Error);
			if (!entry.Font)
				BS_CORE_ERROR("UI font '{}': {}; using the built-in font", path, entry.Error);
			it = s_Fonts.emplace(key, std::move(entry)).first;
		}
		return it->second.Font ? it->second.Font : GetDefault();
	}

	std::string UIFonts::GetError(const std::string& path)
	{
		if (path.empty())
			return {};
		SyncContext();
		auto it = s_Fonts.find(ResolveKey(path));
		return it != s_Fonts.end() ? it->second.Error : std::string();
	}

	void UIFonts::Clear()
	{
		s_Fonts.clear();
		s_Context = nullptr;
	}

}
