#pragma once

#include "EditorContext.h"

namespace Basalt {
	// Captures keys of the selected entity's fields, or another's, and scrubs its timeline through the automation API.
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
		// The entity Capture Key reads: 0 is the selected one.
		uint64_t m_Target = 0;
		float m_Time = 0;
	};
}
