#pragma once

#include "EditorContext.h"

namespace Basalt {
	// Captures keys and scrubs selected-entity numeric timelines through the automation API.
	class PropertyAnimationPanel
	{
	public:
		explicit PropertyAnimationPanel(EditorContext& context)
			: m_Context(context)
		{}
		void OnImGuiRender();

	private:
		EditorContext& m_Context;
		std::string m_Component = "Transform";
		std::string m_Field = "Translation";
		float m_Time = 0;
	};
}
