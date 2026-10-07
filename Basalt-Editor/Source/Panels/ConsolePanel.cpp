#include "Panels/ConsolePanel.h"

#include "Basalt/Core/Log.h"

#include <imgui.h>
#include <imgui_stdlib.h>

namespace Basalt {

	void ConsolePanel::OnImGuiRender()
	{
		ImGui::Begin("Console");
		if (ImGui::Button("Clear"))
			m_ClearedAt = Log::GetHistory().GetTotalCount();
		ImGui::SameLine();
		ImGui::Checkbox("Trace", &m_ShowTrace);
		ImGui::SameLine();
		ImGui::Checkbox("Info", &m_ShowInfo);
		ImGui::SameLine();
		ImGui::Checkbox("Warnings", &m_ShowWarnings);
		ImGui::SameLine();
		ImGui::Checkbox("Errors", &m_ShowErrors);
		ImGui::SameLine();
		ImGui::Checkbox("Auto-scroll", &m_AutoScroll);
		ImGui::Separator();

		const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
		ImGui::BeginChild("Messages", ImVec2(0.0f, -footer), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
		uint64_t next = 0;
		for (const LogMessage& message : Log::GetHistory().GetMessagesSince(m_ClearedAt, next))
		{
			ImVec4 color(0.85f, 0.85f, 0.85f, 1.0f);
			switch (message.Level)
			{
				case LogLevel::Trace:
					if (!m_ShowTrace)
						continue;
					color = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
					break;
				case LogLevel::Info:
					if (!m_ShowInfo)
						continue;
					break;
				case LogLevel::Warn:
					if (!m_ShowWarnings)
						continue;
					color = ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
					break;
				case LogLevel::Error:
				case LogLevel::Critical:
					if (!m_ShowErrors)
						continue;
					color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
					break;
			}
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			ImGui::TextUnformatted(("[" + message.LoggerName + "] " + message.Text).c_str());
			ImGui::PopStyleColor();
		}
		if (m_AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 5.0f)
			ImGui::SetScrollHereY(1.0f);
		ImGui::EndChild();

		ImGui::SetNextItemWidth(-1.0f);
		const char* hint = m_Context.Session.IsPlaying() ? "Lua (runs in the playing scene), e.g. Scene.GetEntityCount()" : "Lua console is available while playing";
		if (ImGui::InputTextWithHint("##Lua", hint, &m_Input, ImGuiInputTextFlags_EnterReturnsTrue) && !m_Input.empty())
		{
			BS_INFO("> {}", m_Input);
			if (auto result = m_Context.Execute("lua.exec", { { "code", m_Input } }))
			{
				const std::string text = (*result)["result"].get<std::string>();
				if (!text.empty())
					BS_INFO("{}", text);
			}
			m_Input.clear();
			ImGui::SetKeyboardFocusHere(-1);
		}
		ImGui::End();
	}

}
