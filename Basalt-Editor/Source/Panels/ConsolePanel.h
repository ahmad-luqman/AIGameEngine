#pragma once

#include "EditorContext.h"

#include <string>

namespace Basalt {

	// Engine log viewer with level filters, plus a Lua prompt that runs in the playing scene.
	class ConsolePanel
	{
	public:
		explicit ConsolePanel(EditorContext& context)
			: m_Context(context)
		{
		}

		void OnImGuiRender();

	private:
		EditorContext& m_Context;
		std::string m_Input;
		bool m_ShowTrace = false;
		bool m_ShowInfo = true;
		bool m_ShowWarnings = true;
		bool m_ShowErrors = true;
		bool m_AutoScroll = true;
		uint64_t m_ClearedAt = 0;
	};

}
