#pragma once

#include "AnimationGraphDraft.h"
#include "EditorContext.h"

#include "Basalt/Core/FileWatcher.h"
#include "Basalt/Automation/EditorCommandSchemas.h"

#include <memory>
#include <map>
#include <cstdint>

namespace Basalt {
	class Project;
	// Structured .banimgraph authoring with a state/edge view and validated automation saves.
	class AnimationGraphPanel
	{
	public:
		explicit AnimationGraphPanel(EditorContext& context)
			: m_Context(context)
		{}
		bool Open(const std::string& path, std::string& error);
		void Reset();
		void OnImGuiRender();
		void OnFilesChanged(const std::vector<FileChange>& changes);
		void OnAssetMoved(const std::string& from, const std::string& to);
		const AnimationGraphDraft& GetDraft() const { return m_Draft; }
		bool IsDirty() const;
		std::string GetError() const;
		bool Validate(std::string& error) const;
		static constexpr const char* Title = EditorCommandSchemas::AnimationGraphTitle;

	private:
		void DrawGraph();
		void DrawFields(bool discardNameEvents);
		bool Save();
		EditorContext& m_Context;
		AnimationGraphDraft m_Draft;
		std::weak_ptr<Project> m_Project;
		std::string m_Error;
		int m_SelectedState = 0;
		std::map<uint32_t, AnimationGraphNameEdit> m_NameEdits;
		bool HasPendingRename() const;
		bool m_DiscardNameEvents = true;
		bool m_ReloadAfterRename = false;
		void ResetNameEdits();
		uint32_t m_StateNameInputId = 0;
		bool CommitStateRename();
		std::string m_PathInput = "Assets/Animations/New.banimgraph";
	};
}
