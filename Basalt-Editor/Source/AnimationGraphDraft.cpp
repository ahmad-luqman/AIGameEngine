#include "AnimationGraphDraft.h"

#include "Basalt/Animation/AnimationGraph.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Project/Project.h"

#include <algorithm>

namespace Basalt {
	nlohmann::json AnimationGraphDraft::Default()
	{
		return { { "InitialState", "Idle" }, { "Parameters", nlohmann::json::array() }, { "States", { { { "Name", "Idle" }, { "Clip", "" }, { "Looping", true }, { "RootMotion", false } } } }, { "Transitions", nlohmann::json::array() } };
	}
	bool AnimationGraphDraft::SetData(const nlohmann::json& data, std::string& error)
	{
		const auto graph = AnimationGraph::FromJson(data, error);
		if (!graph)
			return false;
		Data = data;
		if (!Data.contains("Parameters"))
			Data["Parameters"] = nlohmann::json::array();
		if (!Data.contains("Transitions"))
			Data["Transitions"] = nlohmann::json::array();
		// Populate optional fields for widgets without changing graph semantics.
		for (auto& p : Data["Parameters"])
			if (!p.contains("Default"))
				p["Default"] = p["Type"] == "Float" ? nlohmann::json(0.0f) : nlohmann::json(false);
		for (auto& s : Data["States"])
		{
			if (!s.contains("Looping"))
				s["Looping"] = true;
			if (!s.contains("RootMotion"))
				s["RootMotion"] = false;
			if (s.contains("BlendSpace"))
			{
				if (!s.contains("ParameterX"))
					s["ParameterX"] = "";
				if (!s.contains("ParameterY"))
					s["ParameterY"] = "";
			}
		}
		for (auto& t : Data["Transitions"])
		{
			if (!t.contains("From"))
				t["From"] = "";
			if (!t.contains("Duration"))
				t["Duration"] = 0.15f;
			if (!t.contains("Conditions"))
				t["Conditions"] = nlohmann::json::array();
			for (auto& c : t["Conditions"])
			{
				if (!c.contains("Operator"))
					c["Operator"] = "Equal";
				if (!c.contains("Value"))
					c["Value"] = true;
			}
		}
		// Graph numbers are all continuous values; widgets must allow fractional edits even for integer JSON.
		auto floats = [](auto&& self, nlohmann::json& value) -> void {
			if (value.is_number())
				value = value.get<double>();
			else if (value.is_structured())
				for (auto& child : value)
					self(self, child);
		};
		floats(floats, Data);
		return true;
	}
	bool AnimationGraphDraft::Load(const std::string& path, std::string& error)
	{
		const auto text = FileSystem::ReadTextFile(Project::ResolvePath(path));
		if (!text)
		{
			error = "cannot read '" + path + "'";
			return false;
		}
		const auto data = ParseJson(*text, error);
		if (data.is_discarded() || !SetData(data, error))
			return false;
		Path = path;
		m_Undo = { Data };
		m_Redo.clear();
		MarkSaved();
		return true;
	}
	bool AnimationGraphDraft::OnFileChanged(std::string& error)
	{
		if (!IsDirty() && Load(Path, error))
			return true;
		ExternalChange = true;
		return false;
	}
	void AnimationGraphDraft::Record()
	{
		if (!m_Undo.empty() && m_Undo.back() == Data)
			return;
		m_Undo.push_back(Data);
		if (m_Undo.size() > 64)
			m_Undo.erase(m_Undo.begin());
		m_Redo.clear();
	}
	bool AnimationGraphDraft::Undo()
	{
		Record();
		if (m_Undo.size() < 2)
			return false;
		m_Redo.push_back(m_Undo.back());
		m_Undo.pop_back();
		Data = m_Undo.back();
		return true;
	}
	bool AnimationGraphDraft::Redo()
	{
		Record();
		if (m_Redo.empty())
			return false;
		Data = m_Redo.back();
		m_Redo.pop_back();
		m_Undo.push_back(Data);
		return true;
	}
	bool AnimationGraphDraft::Validate(std::string& error) const
	{
		return AnimationGraph::FromJson(Data, error).has_value();
	}
	bool AnimationGraphDraft::RenameState(size_t index, const std::string& name)
	{
		auto& states = Data["States"];
		if (index >= states.size() || name.empty())
			return false;
		const auto old = states[index]["Name"];
		if (old == name)
			return true;
		for (const auto& s : states)
			if (s["Name"] == name)
				return false;
		states[index]["Name"] = name;
		if (Data["InitialState"] == old)
			Data["InitialState"] = name;
		for (auto& t : Data["Transitions"])
		{
			if (t["From"] == old)
				t["From"] = name;
			if (t["To"] == old)
				t["To"] = name;
		}
		return true;
	}
	bool AnimationGraphDraft::RenameParameter(size_t index, const std::string& name)
	{
		auto& parameters = Data["Parameters"];
		if (index >= parameters.size() || name.empty())
			return false;
		const auto old = parameters[index]["Name"];
		if (old == name)
			return true;
		for (const auto& p : parameters)
			if (p["Name"] == name)
				return false;
		parameters[index]["Name"] = name;
		for (auto& s : Data["States"])
			for (const char* axis : { "ParameterX", "ParameterY" })
				if (s.contains(axis) && s[axis] == old)
					s[axis] = name;
		for (auto& t : Data["Transitions"])
			for (auto& c : t["Conditions"])
				if (c["Parameter"] == old)
					c["Parameter"] = name;
		return true;
	}
	void AnimationGraphDraft::SetParameterType(size_t index, const std::string& type)
	{
		if (index >= Data["Parameters"].size() || (type != "Float" && type != "Bool" && type != "Trigger"))
			return;
		auto& p = Data["Parameters"][index];
		p["Type"] = type;
		p["Default"] = type == "Float" ? nlohmann::json(0.0f) : nlohmann::json(false);
		for (auto& state : Data["States"])
			for (const char* axis : { "ParameterX", "ParameterY" })
				if (type != "Float" && state.contains(axis) && state[axis] == p["Name"])
					state[axis] = "";
		for (auto& t : Data["Transitions"])
			for (auto& c : t["Conditions"])
				if (c["Parameter"] == p["Name"])
				{
					c["Operator"] = "Equal";
					c["Value"] = type == "Float" ? nlohmann::json(0.0f) : nlohmann::json(true);
				}
	}
	void AnimationGraphDraft::RemoveState(size_t index)
	{
		auto& states = Data["States"];
		if (states.size() <= 1 || index >= states.size())
			return;
		const auto name = states[index]["Name"];
		states.erase(states.begin() + static_cast<ptrdiff_t>(index));
		auto& transitions = Data["Transitions"];
		std::erase_if(transitions.get_ref<nlohmann::json::array_t&>(), [&](const auto& t) { return t.at("From") == name || t.at("To") == name; });
		if (Data["InitialState"] == name)
			Data["InitialState"] = states[0]["Name"];
	}
	void AnimationGraphDraft::RemoveParameter(size_t index)
	{
		auto& parameters = Data["Parameters"];
		if (index >= parameters.size())
			return;
		const auto name = parameters[index]["Name"];
		parameters.erase(parameters.begin() + static_cast<ptrdiff_t>(index));
		for (auto& s : Data["States"])
			for (const char* axis : { "ParameterX", "ParameterY" })
				if (s.contains(axis) && s[axis] == name)
					s[axis] = "";
		// Removing a condition would make a transition easier to fire. Remove its edge instead.
		auto& transitions = Data["Transitions"];
		std::erase_if(transitions.get_ref<nlohmann::json::array_t&>(), [&](const auto& t) {
			return std::any_of(t.at("Conditions").begin(), t.at("Conditions").end(), [&](const auto& c) { return c.at("Parameter") == name; });
		});
	}
}
