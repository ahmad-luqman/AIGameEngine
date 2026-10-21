#include "Panels/AnimationGraphPanel.h"

#include "EditorColors.h"
#include "Panels/JsonWidgets.h"

#include "Basalt/Animation/PropertyAnimation.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Scene/Entity.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>

namespace Basalt {
	namespace {
		using json = nlohmann::json;
		std::string UniqueName(const json& list, const std::string& prefix)
		{
			for (size_t i = 1;; i++)
			{
				const auto name = prefix + " " + std::to_string(i);
				if (std::none_of(list.begin(), list.end(), [&](const auto& item) { return item.at("Name") == name; }))
					return name;
			}
		}
		void Choice(const char* label, json& value, const std::vector<std::string>& choices)
		{
			const auto current = value.get<std::string>();
			if (ImGui::BeginCombo(label, current.empty() ? "(none / any)" : current.c_str()))
			{
				for (const auto& choice : choices)
					if (ImGui::Selectable(choice.empty() ? "(none / any)" : choice.c_str(), current == choice))
						value = choice;
				ImGui::EndCombo();
			}
		}
	}
	bool AnimationGraphPanel::Open(const std::string& requested, std::string& error)
	{
		const auto path = FileSystem::NormalizeGeneric(requested);
		if (!Project::GetActive() || !path.starts_with("Assets/") || FileSystem::GetLowercaseExtension(FileSystem::PathFromUtf8(path)) != ".banimgraph" || path.find("..") != std::string::npos)
		{
			error = "open a .banimgraph under the active project's Assets/";
			return false;
		}
		if (m_Draft.Path == path)
			return true;
		if (m_Draft.IsDirty())
		{
			error = "Save or Revert the current graph before opening another";
			return false;
		}
		if (!m_Draft.Load(path, error))
			return false;
		m_Project = Project::GetActive();
		m_Error.clear();
		m_SelectedState = 0;
		return true;
	}
	void AnimationGraphPanel::Reset()
	{
		m_Draft = {};
		m_Project.reset();
		m_Error.clear();
		m_SelectedState = 0;
	}
	void AnimationGraphPanel::OnFilesChanged(const std::vector<FileChange>& changes)
	{
		for (const auto& change : changes)
			if (!m_Draft.Path.empty() && change.Path == m_Draft.Path)
			{
				m_Draft.OnFileChanged(m_Error);
			}
	}
	void AnimationGraphPanel::OnAssetMoved(const std::string& from, const std::string& to)
	{
		if (m_Draft.Path == from)
			m_Draft.Path = to;
	}
	bool AnimationGraphPanel::Save()
	{
		if (!m_Draft.Validate(m_Error))
			return false;
		if (!m_Context.Execute("asset.write", { { "path", m_Draft.Path }, { "content", m_Draft.Data.dump(2) } }))
		{
			m_Error = m_Context.StatusMessage;
			return false;
		}
		m_Draft.MarkSaved();
		m_Error.clear();
		return true;
	}
	void AnimationGraphPanel::DrawGraph()
	{
		const auto& states = m_Draft.Data["States"];
		const auto origin = ImGui::GetCursorScreenPos();
		auto center = [&](size_t i) { return ImVec2(origin.x + 80 + static_cast<float>(i % 3) * 180, origin.y + 30 + std::floor(static_cast<float>(i) / 3.0f) * 80); };
		auto* draw = ImGui::GetWindowDrawList();
		for (const auto& t : m_Draft.Data["Transitions"])
		{
			auto index = [&](const json& name) { return std::find_if(states.begin(), states.end(), [&](const auto& s) { return s.at("Name") == name; }); };
			const auto from = index(t["From"]), to = index(t["To"]);
			if (to == states.end())
				continue;
			const auto target = center(static_cast<size_t>(to - states.begin()));
			const auto source = from == states.end() ? ImVec2(target.x, target.y - 45) : center(static_cast<size_t>(from - states.begin()));
			const float dx = target.x - source.x, dy = target.y - source.y;
			const float length = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
			const float border = std::min(std::abs(dx) > 0 ? 76.0f / std::abs(dx) : 1.0f, std::abs(dy) > 0 ? 23.0f / std::abs(dy) : 1.0f);
			const auto start = from == states.end() ? source : ImVec2(source.x + dx * border, source.y + dy * border);
			const ImVec2 end(target.x - dx * border, target.y - dy * border);
			draw->AddLine(start, end, IM_COL32(130, 150, 180, 255), 2);
			draw->AddTriangleFilled(end, ImVec2(end.x - 8 * dx / length + 4 * dy / length, end.y - 8 * dy / length - 4 * dx / length), ImVec2(end.x - 8 * dx / length - 4 * dy / length, end.y - 8 * dy / length + 4 * dx / length), IM_COL32(200, 210, 230, 255));
		}
		for (size_t i = 0; i < states.size(); i++)
		{
			ImGui::PushID(static_cast<int>(i));
			const auto c = center(i);
			draw->AddRectFilled(ImVec2(c.x - 75, c.y - 22), ImVec2(c.x + 75, c.y + 22), IM_COL32(42, 46, 52, 255), 4);
			draw->AddRect(ImVec2(c.x - 75, c.y - 22), ImVec2(c.x + 75, c.y + 22), IM_COL32(100, 120, 150, 255), 4);
			ImGui::SetCursorScreenPos(ImVec2(c.x - 75, c.y - 22));
			const auto name = states[i]["Name"].get<std::string>();
			const auto label = (states[i]["Name"] == m_Draft.Data["InitialState"] ? "> " : "") + name;
			if (ImGui::Selectable(label.c_str(), m_SelectedState == static_cast<int>(i), 0, ImVec2(150, 44)))
				m_SelectedState = static_cast<int>(i);
			ImGui::PopID();
		}
		ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + std::ceil(static_cast<float>(states.size()) / 3.0f) * 80));
		ImGui::Dummy(ImVec2(540, 1));
	}
	void AnimationGraphPanel::DrawFields()
	{
		auto& data = m_Draft.Data;
		auto& states = data["States"];
		auto& parameters = data["Parameters"];
		auto& transitions = data["Transitions"];
		std::vector<std::string> names, axes{ "" }, parameterNames;
		for (const auto& s : states)
			names.push_back(s["Name"]);
		for (const auto& p : parameters)
		{
			parameterNames.push_back(p["Name"]);
			if (p["Type"] == "Float")
				axes.push_back(p["Name"]);
		}
		Choice("Initial State", data["InitialState"], names);
		if (ImGui::Button("Add State") && states.size() < 128)
		{
			states.push_back({ { "Name", UniqueName(states, "State") }, { "Clip", "" }, { "Looping", true }, { "RootMotion", false } });
			m_SelectedState = static_cast<int>(states.size() - 1);
		}
		m_SelectedState = std::clamp(m_SelectedState, 0, static_cast<int>(states.size()) - 1);
		auto& state = states[static_cast<size_t>(m_SelectedState)];
		std::string stateName = state["Name"];
		if (ImGui::InputText("State Name", &stateName))
			m_Draft.RenameState(static_cast<size_t>(m_SelectedState), stateName);
		bool blend = state.contains("BlendSpace");
		if (ImGui::Checkbox("Blend Space State", &blend))
		{
			if (blend)
			{
				state.erase("Clip");
				state["BlendSpace"] = "";
				state["ParameterX"] = "";
				state["ParameterY"] = "";
			}
			else
			{
				state.erase("BlendSpace");
				state.erase("ParameterX");
				state.erase("ParameterY");
				state["Clip"] = "";
			}
		}
		DrawJsonField(blend ? "BlendSpace" : "Clip", state[blend ? "BlendSpace" : "Clip"]);
		if (blend)
		{
			Choice("Parameter X", state["ParameterX"], axes);
			Choice("Parameter Y", state["ParameterY"], axes);
		}
		DrawJsonField("Looping", state["Looping"]);
		DrawJsonField("RootMotion", state["RootMotion"]);
		ImGui::BeginDisabled(states.size() <= 1);
		if (ImGui::Button("Remove State and its Edges"))
			m_Draft.RemoveState(static_cast<size_t>(m_SelectedState));
		ImGui::EndDisabled();
		ImGui::SeparatorText("Parameters");
		if (ImGui::Button("Add Parameter") && parameters.size() < 64)
			parameters.push_back({ { "Name", UniqueName(parameters, "Parameter") }, { "Type", "Float" }, { "Default", 0.0f } });
		for (size_t i = 0; i < parameters.size(); i++)
		{
			ImGui::PushID(static_cast<int>(i));
			auto& p = parameters[i];
			std::string name = p["Name"];
			if (ImGui::InputText("Name", &name))
				m_Draft.RenameParameter(i, name);
			const auto oldType = p["Type"];
			Choice("Type", p["Type"], { "Float", "Bool", "Trigger" });
			if (oldType != p["Type"])
				m_Draft.SetParameterType(i, p["Type"].get<std::string>());
			if (p["Type"] != "Trigger")
				DrawJsonField("Default", p["Default"]);
			if (ImGui::Button("Remove Parameter and dependent Edges"))
			{
				m_Draft.RemoveParameter(i);
				ImGui::PopID();
				break;
			}
			ImGui::PopID();
		}
		ImGui::SeparatorText("Transitions (topmost has priority)");
		if (ImGui::Button("Add Transition") && transitions.size() < 512)
			transitions.push_back({ { "From", "" }, { "To", states[0]["Name"] }, { "Duration", 0.15f }, { "Conditions", json::array() } });
		for (size_t i = 0; i < transitions.size(); i++)
		{
			ImGui::PushID(static_cast<int>(i) + 1000);
			auto& t = transitions[i];
			if (ImGui::TreeNode("Edge", "%zu: %s -> %s", i, t["From"].get_ref<const std::string&>().c_str(), t["To"].get_ref<const std::string&>().c_str()))
			{
				auto sources = names;
				sources.insert(sources.begin(), "");
				Choice("From", t["From"], sources);
				Choice("To", t["To"], names);
				DrawJsonField("Duration", t["Duration"]);
				bool exit = t.contains("ExitTime");
				if (ImGui::Checkbox("Use Exit Time", &exit))
				{
					if (exit)
						t["ExitTime"] = 1.0f;
					else
						t.erase("ExitTime");
				}
				if (exit)
					DrawJsonField("ExitTime", t["ExitTime"]);
				if (ImGui::Button("Add Condition") && !parameters.empty() && t["Conditions"].size() < 64)
					t["Conditions"].push_back({ { "Parameter", parameters[0]["Name"] }, { "Operator", "Equal" }, { "Value", parameters[0]["Type"] == "Float" ? json(0.0f) : json(true) } });
				auto& conditions = t["Conditions"];
				for (size_t j = 0; j < conditions.size(); j++)
				{
					ImGui::PushID(static_cast<int>(j));
					auto& c = conditions[j];
					const auto old = c["Parameter"];
					Choice("Parameter", c["Parameter"], parameterNames);
					const auto p = std::find_if(parameters.begin(), parameters.end(), [&](const auto& v) { return v.at("Name") == c["Parameter"]; });
					const bool numeric = p != parameters.end() && (*p)["Type"] == "Float";
					const bool trigger = p != parameters.end() && (*p)["Type"] == "Trigger";
					if (old != c["Parameter"])
					{
						c["Value"] = numeric ? json(0.0f) : json(true);
						c["Operator"] = "Equal";
					}
					Choice("Operator", c["Operator"], trigger ? std::vector<std::string>{ "Equal" } : numeric ? std::vector<std::string>{ "Greater", "Less", "Equal", "NotEqual" }
																											  : std::vector<std::string>{ "Equal", "NotEqual" });
					if (!trigger)
						DrawJsonField("Value", c["Value"]);
					if (ImGui::Button("Remove Condition"))
					{
						conditions.erase(conditions.begin() + static_cast<ptrdiff_t>(j));
						ImGui::PopID();
						break;
					}
					ImGui::PopID();
				}
				bool remove = ImGui::Button("Remove Edge");
				ImGui::SameLine();
				bool up = ImGui::Button("Higher Priority") && i > 0;
				ImGui::SameLine();
				bool down = ImGui::Button("Lower Priority") && i + 1 < transitions.size();
				ImGui::TreePop();
				if (remove)
				{
					transitions.erase(transitions.begin() + static_cast<ptrdiff_t>(i));
					ImGui::PopID();
					break;
				}
				if (up)
					std::swap(transitions[i], transitions[i - 1]);
				if (down)
					std::swap(transitions[i], transitions[i + 1]);
			}
			ImGui::PopID();
		}
	}
	void AnimationGraphPanel::OnImGuiRender()
	{
		if (!m_Draft.Path.empty() && m_Project.lock() != Project::GetActive())
			Reset();
		ImGui::SetNextWindowSize(ImVec2(600, 650), ImGuiCond_FirstUseEver);
		if (ImGui::Begin(Title, nullptr, m_Draft.IsDirty() ? ImGuiWindowFlags_UnsavedDocument : 0))
		{
			static std::string s_Path = "Assets/Animations/New.banimgraph";
			ImGui::InputText("Graph Path", &s_Path);
			if (ImGui::Button("Open"))
			{
				if (!Open(s_Path, m_Error))
					m_Context.SetStatus(m_Error, true);
			}
			ImGui::SameLine();
			if (ImGui::Button("Create") && Project::GetActive())
			{
				if (m_Draft.IsDirty())
					m_Error = "Save or Revert the current graph first";
				else if (!s_Path.starts_with("Assets/") || s_Path.find("..") != std::string::npos || FileSystem::GetLowercaseExtension(FileSystem::PathFromUtf8(s_Path)) != ".banimgraph")
					m_Error = "Choose a .banimgraph path under Assets/";
				else if (FileSystem::Exists(Project::ResolvePath(s_Path)))
					m_Error = "The file already exists; Open it instead";
				else if (m_Context.Execute("asset.write", { { "path", s_Path }, { "content", AnimationGraphDraft::Default().dump(2) } }))
					Open(s_Path, m_Error);
			}
			if (!m_Draft.Path.empty())
			{
				ImGui::TextUnformatted(m_Draft.Path.c_str());
				if (ImGui::Button("Undo Graph"))
					m_Draft.Undo();
				ImGui::SameLine();
				if (ImGui::Button("Redo Graph"))
					m_Draft.Redo();

				if (ImGui::Button("Save"))
					Save();
				ImGui::SameLine();
				if (ImGui::Button("Revert"))
					m_Draft.Load(m_Draft.Path, m_Error);
				ImGui::SameLine();
				if (ImGui::Button("Apply to Selection"))
				{
					if (m_Context.Execute("component.set", { { "entity", static_cast<uint64_t>(m_Context.Session.GetSelection()) }, { "component", "Animator" }, { "data", { { "StateMachine", m_Draft.Path } } } }))
						m_Context.CommitHistory();
				}
				if (m_Draft.ExternalChange)
					ImGui::TextColored(EditorColors::Warning(), "Changed on disk: Save replaces it; Revert loads it.");
				std::string problem;
				if (!m_Draft.Validate(problem))
					ImGui::TextColored(EditorColors::Error(), "%s", problem.c_str());
				ImGui::BeginChild("Graph", ImVec2(0, 220), true, ImGuiWindowFlags_HorizontalScrollbar);
				DrawGraph();
				ImGui::EndChild();
				DrawFields();
				if (!ImGui::IsAnyItemActive())
					m_Draft.Record();
			}
			if (!m_Error.empty())
				ImGui::TextColored(EditorColors::Error(), "%s", m_Error.c_str());
		}
		ImGui::End();
	}
}
