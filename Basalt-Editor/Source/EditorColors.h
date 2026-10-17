#pragma once

#include <imgui.h>

namespace Basalt::EditorColors {

	// Text colours the editor's panels share for problems and results, readable on the current theme's background (the
	// dark theme's light tints would wash out on the light theme).
	inline bool s_LightTheme = false;

	inline void SetLightTheme(bool light)
	{
		s_LightTheme = light;
	}

	inline ImVec4 Error()
	{
		return s_LightTheme ? ImVec4(0.75f, 0.1f, 0.1f, 1.0f) : ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
	}

	inline ImVec4 Warning()
	{
		return s_LightTheme ? ImVec4(0.65f, 0.4f, 0.0f, 1.0f) : ImVec4(1.0f, 0.75f, 0.3f, 1.0f);
	}

	inline ImVec4 Success()
	{
		return s_LightTheme ? ImVec4(0.1f, 0.5f, 0.1f, 1.0f) : ImVec4(0.4f, 0.9f, 0.4f, 1.0f);
	}

	// Less important lines (trace and debug log messages).
	inline ImVec4 Muted()
	{
		return s_LightTheme ? ImVec4(0.45f, 0.45f, 0.45f, 1.0f) : ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
	}

	// Highlighted values (per-entity script property overrides).
	inline ImVec4 Highlight()
	{
		return s_LightTheme ? ImVec4(0.1f, 0.35f, 0.7f, 1.0f) : ImVec4(0.55f, 0.85f, 1.0f, 1.0f);
	}

}
