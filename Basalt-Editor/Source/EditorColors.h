#pragma once

#include "Basalt/Automation/EditorPreferences.h"

#include <imgui.h>

namespace Basalt::EditorColors {

	// Text colours the editor's panels share for problems and results, readable on the current theme's background (the
	// dark theme's light tints would wash out on the light theme).
	struct Palette
	{
		// Ordinary log lines (slightly softer than the theme's text).
		ImVec4 Info;
		ImVec4 Error;
		ImVec4 Warning;
		ImVec4 Success;
		// Less important lines (trace and debug log messages).
		ImVec4 Muted;
		// Highlighted values (per-entity script property overrides).
		ImVec4 Highlight;
	};

	inline constexpr Palette DarkPalette{
		.Info = { 0.85f, 0.85f, 0.85f, 1.0f },
		.Error = { 1.0f, 0.45f, 0.45f, 1.0f },
		.Warning = { 1.0f, 0.75f, 0.3f, 1.0f },
		.Success = { 0.4f, 0.9f, 0.4f, 1.0f },
		.Muted = { 0.55f, 0.55f, 0.55f, 1.0f },
		.Highlight = { 0.55f, 0.85f, 1.0f, 1.0f },
	};
	inline constexpr Palette LightPalette{
		.Info = { 0.12f, 0.12f, 0.12f, 1.0f },
		.Error = { 0.75f, 0.1f, 0.1f, 1.0f },
		.Warning = { 0.65f, 0.4f, 0.0f, 1.0f },
		.Success = { 0.1f, 0.5f, 0.1f, 1.0f },
		.Muted = { 0.45f, 0.45f, 0.45f, 1.0f },
		.Highlight = { 0.1f, 0.35f, 0.7f, 1.0f },
	};

	namespace Detail {

		// The palette in use; EditorLayer::ApplyPreferences sets it with the ImGui theme (SetTheme).
		inline const Palette* s_Current = &DarkPalette;

	}

	inline void SetTheme(EditorTheme theme)
	{
		Detail::s_Current = theme == EditorTheme::Light ? &LightPalette : &DarkPalette;
	}

	inline const Palette& Current()
	{
		return *Detail::s_Current;
	}

	inline ImVec4 Info()
	{
		return Current().Info;
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
