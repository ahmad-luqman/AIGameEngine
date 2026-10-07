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
		UUID m_PendingDelete = 0;
	};

}
