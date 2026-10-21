#pragma once

#include "AnimationGraphDraft.h"
#include "EditorContext.h"

#include "Basalt/Core/FileWatcher.h"

#include <memory>

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
		static constexpr const char* Title = "Animation Graph";

	private:
		void DrawGraph();
		void DrawFields();
		bool Save();
		EditorContext& m_Context;
		AnimationGraphDraft m_Draft;
		std::weak_ptr<Project> m_Project;
		std::string m_Error;
		int m_SelectedState = 0;
	};
}
