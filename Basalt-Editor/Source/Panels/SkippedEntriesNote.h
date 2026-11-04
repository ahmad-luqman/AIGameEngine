#pragma once

#include "EditorColors.h"

#include "Basalt/Core/FileSystem.h"

#include <imgui.h>

#include <set>
#include <string>

namespace Basalt {

	// A line saying how many entries a list leaves out for names no key can hold, naming them on hover.
	inline void DrawSkippedEntriesNote(const std::set<std::string>& skipped)
	{
		if (skipped.empty())
			return;
		std::string details = FileSystem::NonUtf8NameProblem;
		for (const std::string& entry : skipped)
			details.append("\n").append(entry);
		ImGui::TextColored(EditorColors::Warning(), "%zu hidden: names that are not valid Unicode", skipped.size());
		ImGui::SetItemTooltip("%s", details.c_str());
	}

}
