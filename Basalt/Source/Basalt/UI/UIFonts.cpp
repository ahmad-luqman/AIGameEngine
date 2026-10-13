#include "Basalt/UI/UIFonts.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Scene/Components.h"
#include "Basalt/UI/TrueTypeValidator.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <unordered_map>

namespace Basalt {

	namespace {

		struct FontEntry
		{
			ImFont* Font = nullptr;
			std::string Error;
			// For a failure: the file's modification time then (nullopt: it did not exist). A change makes
			// the next Get try again, so fixing or adding the file needs no restart.
			std::optional<std::filesystem::file_time_type> FailedStamp;
		};

		// Keyed by the resolved file path, so the same file named two ways loads once and another project's
		// file with the same relative path is a different font.
		std::unordered_map<std::string, FontEntry> s_Fonts;
		ImGuiContext* s_Context = nullptr;

		// Fonts belong to a context's atlas. The cache follows the current context, and a shutdown hook clears
		// it when that context is destroyed, so it never holds fonts of a dead atlas (even if a new context
		// reuses the old one's address).
		void SyncContext()
		{
			ImGuiContext* context = ImGui::GetCurrentContext();
			if (context == s_Context)
				return;
			s_Fonts.clear();
			s_Context = context;
			if (!context)
				return;
			ImGuiContextHook hook;
			hook.Type = ImGuiContextHookType_Shutdown;
			hook.Callback = [](ImGuiContext* shutdown, ImGuiContextHook*) {
				if (shutdown == s_Context)
					UIFonts::Clear();
			};
			ImGui::AddContextHook(context, &hook);
		}

		std::filesystem::path ResolveFile(const std::string& path)
		{
			return Project::ResolvePath(path).lexically_normal();
		}

		std::optional<std::filesystem::file_time_type> GetStamp(const std::filesystem::path& file)
		{
			std::error_code error;
			const std::filesystem::file_time_type time = std::filesystem::last_write_time(file, error);
			return error ? std::nullopt : std::optional(time);
		}

		// Reads a font file, refusing anything that is not a regular file of at most MaxTrueTypeFontSize
		// before allocating for it.
		std::optional<std::vector<uint8_t>> ReadFontFile(const std::filesystem::path& file, std::string& outError)
		{
			std::error_code error;
			if (!std::filesystem::is_regular_file(file, error))
			{
				outError = fmt::format("'{}' does not exist or is not a file", FileSystem::PathToUtf8(file));
				return std::nullopt;
			}
			const uintmax_t size = std::filesystem::file_size(file, error);
			if (error || size > MaxTrueTypeFontSize)
			{
				outError = fmt::format("'{}' is larger than {} MB", FileSystem::PathToUtf8(file), MaxTrueTypeFontSize >> 20);
				return std::nullopt;
			}
			std::optional<std::vector<uint8_t>> data = FileSystem::ReadBinaryFile(file);
			if (!data)
				outError = fmt::format("'{}' could not be read", FileSystem::PathToUtf8(file));
			return data;
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

	ImFont* UIFonts::LoadFromMemory(std::vector<uint8_t> data, const std::string& name, std::string& outError)
	{
		std::vector<TrueTypeFontCheck::BrokenGlyph> repaired;
		if (std::string problem = RepairTrueTypeFont(data, &repaired); !problem.empty())
		{
			outError = std::move(problem);
			return nullptr;
		}
		if (!repaired.empty())
		{
			std::string glyphs;
			for (size_t i = 0; i < repaired.size() && i < 5; i++)
				glyphs += fmt::format("{}glyph {}: {}", i > 0 ? "; " : "", repaired[i].Index, repaired[i].Problem);
			BS_CORE_WARN("UI font '{}': {} glyph(s) cannot be drawn safely and draw blank ({}{})", name, repaired.size(), glyphs, repaired.size() > 5 ? "; ..." : "");
		}

		// ImGui's default font is the atlas's first one: make sure that is the built-in font, not a game font
		// loaded before ImGui's first frame (the editor's own UI would switch to it).
		GetDefault();

		// The atlas reads the data whenever it rasterizes new glyphs, so it takes ownership of a copy.
		void* copy = IM_ALLOC(data.size());
		std::memcpy(copy, data.data(), data.size());
		ImFontConfig config;
		config.FontDataOwnedByAtlas = true;
		std::snprintf(config.Name, sizeof(config.Name), "%s", name.c_str());
		// The size only matters to code that draws with the font's default size; the UI always passes one.
		ImFont* font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(copy, static_cast<int>(data.size()), UITextComponent::DefaultFontSize, &config);
		if (!font)
		{
			// Unreachable while ImGui asserts on fonts stb rejects; the check should have rejected it first.
			outError = "stb_truetype rejected a font that passed the font check (please report this as a bug)";
			return nullptr;
		}
		// Without U+2026 or U+0085, ImGui builds an ellipsis from the '.' glyph's bitmap and asserts when that
		// glyph has none (a blank or repaired '.'). The game UI never elides text, so it needs no ellipsis:
		// U+0085 then falls back like any other missing character.
		font->EllipsisAutoBake = false;
		return font;
	}

	ImFont* UIFonts::Get(const std::string& path)
	{
		if (path.empty())
			return GetDefault();
		SyncContext();
		const std::filesystem::path file = ResolveFile(path);
		const std::string key = FileSystem::PathToUtf8(file);
		auto it = s_Fonts.find(key);
		if (it != s_Fonts.end() && !it->second.Font && it->second.FailedStamp != GetStamp(file))
		{
			s_Fonts.erase(it);
			it = s_Fonts.end();
		}
		if (it == s_Fonts.end())
		{
			FontEntry entry;
			if (std::optional<std::vector<uint8_t>> data = ReadFontFile(file, entry.Error))
				entry.Font = LoadFromMemory(std::move(*data), path, entry.Error);
			if (!entry.Font)
			{
				entry.FailedStamp = GetStamp(file);
				BS_CORE_ERROR("UI font '{}': {}; using the built-in font", path, entry.Error);
			}
			it = s_Fonts.emplace(key, std::move(entry)).first;
		}
		return it->second.Font ? it->second.Font : GetDefault();
	}

	std::string UIFonts::CheckFile(const std::string& path)
	{
		std::string error;
		if (std::optional<std::vector<uint8_t>> data = ReadFontFile(ResolveFile(path), error))
			error = RepairTrueTypeFont(*data);
		return error;
	}

	std::string UIFonts::GetError(const std::string& path)
	{
		if (path.empty())
			return {};
		SyncContext();
		auto it = s_Fonts.find(FileSystem::PathToUtf8(ResolveFile(path)));
		return it != s_Fonts.end() ? it->second.Error : std::string();
	}

	void UIFonts::Clear()
	{
		s_Fonts.clear();
		s_Context = nullptr;
	}

}
