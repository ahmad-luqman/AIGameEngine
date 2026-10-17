#pragma once

#include <imgui.h>

namespace Basalt::EditorColors {

	// Text colours the editor's panels share for problems and results, readable on the current theme's background (the
	// dark theme's light tints would wash out on the light theme).
	struct Palette
	{
		ImVec4 Error;
		ImVec4 Warning;
		ImVec4 Success;
		// Less important lines (trace and debug log messages).
		ImVec4 Muted;
		// Highlighted values (per-entity script property overrides).
		ImVec4 Highlight;
	};

	inline constexpr Palette DarkPalette{ { 1.0f, 0.45f, 0.45f, 1.0f }, { 1.0f, 0.75f, 0.3f, 1.0f }, { 0.4f, 0.9f, 0.4f, 1.0f }, { 0.55f, 0.55f, 0.55f, 1.0f }, { 0.55f, 0.85f, 1.0f, 1.0f } };
	inline constexpr Palette LightPalette{ { 0.75f, 0.1f, 0.1f, 1.0f }, { 0.65f, 0.4f, 0.0f, 1.0f }, { 0.1f, 0.5f, 0.1f, 1.0f }, { 0.45f, 0.45f, 0.45f, 1.0f }, { 0.1f, 0.35f, 0.7f, 1.0f } };

	// Set by EditorLayer::ApplyPreferences through SetLightTheme.
	inline bool s_LightTheme = false;

	inline void SetLightTheme(bool light)
	{
		s_LightTheme = light;
	}

	inline const Palette& Current()
	{
		return s_LightTheme ? LightPalette : DarkPalette;
	}

	inline ImVec4 Error()
	{
		return Current().Error;
	}

	inline ImVec4 Warning()
	{
		return Current().Warning;
	}

	inline ImVec4 Success()
	{
		return Current().Success;
	}

	inline ImVec4 Muted()
	{
		return Current().Muted;
	}

	inline ImVec4 Highlight()
	{
		return Current().Highlight;
	}

}
