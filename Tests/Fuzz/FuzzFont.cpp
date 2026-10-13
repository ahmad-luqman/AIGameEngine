// TrueType fonts for the game UI: the check and repair (TrueTypeValidator), then Dear ImGui's font loader
// (stb_truetype) measuring and rasterizing glyphs from the repaired font, as UIFonts does. An ImGui assertion
// counts as a crash: stb asserts on input it cannot handle, so any assertion is input the check let through.
#include "FuzzCommon.h"

#include <Basalt/UI/TrueTypeValidator.h>
#include <Basalt/UI/UIFonts.h>

#include <imgui.h>

#include <cfloat>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BasaltFuzz::Init();
	if (size > Basalt::MaxTrueTypeFontSize)
		return 0;

	const std::vector<uint8_t> font(data, data + size);
	// Fonts accumulate in an atlas, so every input gets a fresh context.
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	io.IniFilename = nullptr;

	std::string error;
	if (ImFont* loaded = Basalt::UIFonts::LoadFromMemory(font, "fuzz", error))
	{
		// Letters, digits and punctuation, plus a few characters chosen by the input, at a size ImGui
		// oversamples and one it does not. (More glyphs and sizes mostly repeat the same rasterizer work.)
		std::vector<ImWchar> codepoints;
		for (const char* c = "Ag0!@|y"; *c; c++)
			codepoints.push_back(static_cast<ImWchar>(*c));
		// (From the end of the input: its first bytes are the font header, where a change breaks the font.)
		for (size_t i = 2; i <= size && i <= 16; i += 2)
			codepoints.push_back(static_cast<ImWchar>((data[size - i] << 8) | data[size - i + 1]));
		for (float pixels : { 9.0f, 40.0f })
		{
			ImFontBaked* baked = loaded->GetFontBaked(pixels);
			for (ImWchar c : codepoints)
				baked->FindGlyph(c);
		}
		const char* text = "The quick brown fox\njumps over the lazy dog";
		loaded->CalcTextSizeA(24.0f, FLT_MAX, 120.0f, text);
	}
	Basalt::UIFonts::Clear();
	ImGui::DestroyContext();
	return 0;
}
