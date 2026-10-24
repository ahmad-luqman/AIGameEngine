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
			// Loaded, but some glyphs draw blank (RepairTrueTypeFont).
			std::string Warning;
			// For a failure: the file's modification time before it was read (nullopt: it did not exist). A
			// change makes the next Get try again, so fixing or adding the file needs no restart.
			std::optional<std::filesystem::file_time_type> FailedStamp;

			bool Failed() const { return Font == nullptr; }
		};

		// Keyed by the resolved file path, so the same file named two ways loads once and another project's
		// file with the same relative path is a different font.
		std::unordered_map<std::string, FontEntry> s_Fonts;
		ImGuiContext* s_Context = nullptr;

		// Repaired glyphs listed in a warning.
		constexpr size_t MaxListedGlyphs = 5;

		void OnContextShutdown(ImGuiContext* context, ImGuiContextHook*)
		{
			if (context != s_Context)
				return;
			s_Fonts.clear();
			s_Context = nullptr;
		}

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
			// One hook per context, however often the current context changes.
			for (const ImGuiContextHook& hook : context->Hooks)
			{
				if (hook.Type == ImGuiContextHookType_Shutdown && hook.Callback == &OnContextShutdown)
					return;
			}
			ImGuiContextHook hook;
			hook.Type = ImGuiContextHookType_Shutdown;
			hook.Callback = &OnContextShutdown;
			ImGui::AddContextHook(context, &hook);
		}

		std::filesystem::path ResolveFile(const std::string& path)
		{
			return Project::ResolvePath(path).lexically_normal();
		}

		std::optional<std::filesystem::file_time_type> GetStamp(const std::filesystem::path& file)
		{
			// Archive files never change.
			if (FileSystem::FindMounted(file))
				return std::filesystem::file_time_type{};
			std::error_code error;
			const std::filesystem::file_time_type time = std::filesystem::last_write_time(file, error);
			return error ? std::nullopt : std::optional(time);
		}

		// Reads a font file, refusing anything that is not a regular file of at most MaxTrueTypeFontSize
		// before allocating for it.
		std::optional<std::vector<uint8_t>> ReadFontFile(const std::filesystem::path& file, std::string& outError)
		{
			const std::string name = FileSystem::PathToUtf8(file);
			std::error_code error;
			const std::optional<uint64_t> size = FileSystem::GetRegularFileSize(file, error);
			if (error)
			{
				outError = fmt::format("'{}' could not be read: {}", name, error.message());
				return std::nullopt;
			}
			if (!size)
			{
				outError = fmt::format("'{}' does not exist or is not a file", name);
				return std::nullopt;
			}
			if (*size > MaxTrueTypeFontSize)
			{
				outError = fmt::format("'{}' is larger than {} MB", name, MaxTrueTypeFontSize / (size_t{ 1024 } * 1024));
				return std::nullopt;
			}
			std::string readError;
			std::optional<std::vector<uint8_t>> data = FileSystem::ReadBinaryFile(file, &readError);
			if (!data)
				outError = fmt::format("'{}' could not be read ({})", name, readError);
			return data;
		}

		// The warning for a font whose broken glyphs were blanked; empty when none were.
		std::string DescribeRepairs(const std::vector<TrueTypeFontCheck::BrokenGlyph>& repaired)
		{
			if (repaired.empty())
				return {};
			std::string glyphs;
			for (size_t i = 0; i < repaired.size() && i < MaxListedGlyphs; i++)
				glyphs += fmt::format("{}glyph {}: {}", i > 0 ? "; " : "", repaired[i].Index, repaired[i].Problem);
			return fmt::format("{} glyph(s) cannot be drawn safely and draw blank ({}{})", repaired.size(), glyphs, repaired.size() > MaxListedGlyphs ? "; ..." : "");
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

	ImFont* UIFonts::LoadFromMemory(std::vector<uint8_t> data, const std::string& name, std::string& outError, std::string* outWarning)
	{
		std::vector<TrueTypeFontCheck::BrokenGlyph> repaired;
		if (std::string problem = RepairTrueTypeFont(data, &repaired); !problem.empty())
		{
			outError = std::move(problem);
			return nullptr;
		}
		if (std::string warning = DescribeRepairs(repaired); !warning.empty())
		{
			BS_CORE_WARN("UI font '{}': {}", name, warning);
			if (outWarning)
				*outWarning = std::move(warning);
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
		const bool retry = it != s_Fonts.end() && it->second.Failed() && it->second.FailedStamp != GetStamp(file);
		if (it == s_Fonts.end() || retry)
		{
			FontEntry entry;
			// Taken before reading, so a file saved while it is read is read again next time.
			const std::optional<std::filesystem::file_time_type> stamp = GetStamp(file);
			if (std::optional<std::vector<uint8_t>> data = ReadFontFile(file, entry.Error))
				entry.Font = LoadFromMemory(std::move(*data), path, entry.Error, &entry.Warning);
			if (entry.Failed())
			{
				entry.FailedStamp = stamp;
				BS_CORE_ERROR("UI font '{}': {}; using the built-in font", path, entry.Error);
			}
			it = s_Fonts.insert_or_assign(key, std::move(entry)).first;
		}
		return it->second.Failed() ? GetDefault() : it->second.Font;
	}

	std::string UIFonts::CheckFile(const std::string& path)
	{
		std::string problem;
		std::optional<std::vector<uint8_t>> data = ReadFontFile(ResolveFile(path), problem);
		if (!data)
			return problem;
		std::vector<TrueTypeFontCheck::BrokenGlyph> repaired;
		problem = RepairTrueTypeFont(*data, &repaired);
		return problem.empty() ? DescribeRepairs(repaired) : problem;
	}

	std::string UIFonts::GetError(const std::string& path)
	{
		if (path.empty())
			return {};
		SyncContext();
		const std::filesystem::path file = ResolveFile(path);
		auto it = s_Fonts.find(FileSystem::PathToUtf8(file));
		// A failure whose file changed since is tried again on the next Get: it is no longer the error.
		if (it == s_Fonts.end() || (it->second.Failed() && it->second.FailedStamp != GetStamp(file)))
			return {};
		return it->second.Error;
	}

	std::string UIFonts::GetWarning(const std::string& path)
	{
		if (path.empty())
			return {};
		SyncContext();
		auto it = s_Fonts.find(FileSystem::PathToUtf8(ResolveFile(path)));
		return it != s_Fonts.end() ? it->second.Warning : std::string();
	}

	void UIFonts::Clear()
	{
		s_Fonts.clear();
	}

}
