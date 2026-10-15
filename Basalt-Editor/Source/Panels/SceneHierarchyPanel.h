#pragma once

#include "EditorContext.h"

#include "Basalt/Scene/Entity.h"

namespace Basalt {

	// Tree of scene entities: selection, drag-and-drop re-parenting, create/duplicate/delete.
	class SceneHierarchyPanel
	{
	public:
		explicit SceneHierarchyPanel(EditorContext& context)
			: m_Context(context)
		{
		}

		void OnImGuiRender();

	private:
		void DrawEntityNode(Entity entity);
		void DrawCreateMenu(Entity parent);

	private:
		EditorContext& m_Context;
		bool m_PendingDelete = false;
		UUID m_Anchor = 0;
		std::vector<UUID> m_Visible, m_PreviousVisible;
	};

}
