#include "Basalt/Automation/EditorHistory.h"

#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"

#include <algorithm>
#include <utility>

namespace Basalt {

	namespace {
		// UUID keys and successor links preserve hierarchy order without rewriting later array offsets.
		nlohmann::json Encode(nlohmann::json state)
		{
			if (state.contains("Entities") && state["Entities"].is_array())
			{
				auto entities = nlohmann::json::object();
				const auto& source = state["Entities"];
				state["$FirstEntity"] = source.empty() ? "" : std::to_string(source[0]["ID"].get<uint64_t>());
				for (size_t i = 0; i < source.size(); ++i)
				{
					const std::string id = std::to_string(source[i]["ID"].get<uint64_t>());
					const std::string next = i + 1 == source.size() ? "" : std::to_string(source[i + 1]["ID"].get<uint64_t>());
					entities[id] = { { "Data", source[i] }, { "Next", next } };
				}
				state["Entities"] = std::move(entities);
			}
			return state;
		}

		nlohmann::json Decode(nlohmann::json state)
		{
			if (state.contains("Entities") && state["Entities"].is_object())
			{
				auto entities = nlohmann::json::array();
				std::string id = state.at("$FirstEntity").get<std::string>();
				for (size_t i = 0; i < state["Entities"].size(); ++i)
				{
					const auto& entry = state["Entities"].at(id);
					entities.push_back(entry.at("Data"));
					id = entry.at("Next").get<std::string>();
				}
				state.erase("$FirstEntity");
				state["Entities"] = std::move(entities);
			}
			return state;
		}
	}

	void EditorHistory::Reset(nlohmann::json state)
	{
		m_Current = Encode(std::move(state));
		m_Entries.clear();
		m_Transaction.clear();
		m_Index = m_PatchBytes = 0;
	}

	void EditorHistory::Commit(nlohmann::json state, const std::string& name)
	{
		if (IsActive())
			return;
		state = Encode(std::move(state));
		if (state == m_Current)
			return;
		Entry entry{ name.empty() ? "Edit Scene" : name, nlohmann::json::diff(m_Current, state).dump(), nlohmann::json::diff(state, m_Current).dump() };
		while (m_Entries.size() > m_Index)
		{
			m_PatchBytes -= m_Entries.back().Bytes();
			m_Entries.pop_back();
		}
		m_PatchBytes += entry.Bytes();
		m_Entries.push_back(std::move(entry));
		m_Current = std::move(state);
		while (m_Entries.size() > MaxEntries || m_PatchBytes > MaxPatchBytes)
		{
			m_PatchBytes -= m_Entries.front().Bytes();
			m_Entries.erase(m_Entries.begin());
		}
		m_Index = m_Entries.size();
	}

	bool EditorHistory::Begin(const std::string& name)
	{
		if (IsActive() || name.empty())
			return false;
		m_Transaction = name;
		return true;
	}

	bool EditorHistory::End(nlohmann::json state)
	{
		if (!IsActive())
			return false;
		const std::string name = std::exchange(m_Transaction, {});
		Commit(std::move(state), name);
		return true;
	}

	bool EditorHistory::Cancel(const Restore& restore)
	{
		if (!IsActive() || !restore(Decode(m_Current)))
			return false;
		m_Transaction.clear();
		return true;
	}

	bool EditorHistory::Move(bool forward, const Restore& restore)
	{
		if (forward ? !CanRedo() : !CanUndo())
			return false;
		try
		{
			const Entry& entry = m_Entries[forward ? m_Index : m_Index - 1];
			const auto patch = ParseJson(forward ? entry.Forward : entry.Backward);
			auto next = m_Current.patch(patch);
			if (!restore(Decode(next)))
				return false;
			m_Current = std::move(next);
			if (forward)
				++m_Index;
			else
				--m_Index;
			return true;
		}
		catch (const nlohmann::json::exception& error)
		{
			BS_CORE_ERROR("History patch failed: {}", error.what());
			return false;
		}
	}

	bool EditorHistory::Undo(const Restore& restore)
	{
		return Move(false, restore);
	}
	bool EditorHistory::Redo(const Restore& restore)
	{
		return Move(true, restore);
	}
	std::string EditorHistory::UndoName() const
	{
		return CanUndo() ? m_Entries[m_Index - 1].Name : "";
	}
	std::string EditorHistory::RedoName() const
	{
		return CanRedo() ? m_Entries[m_Index].Name : "";
	}

	nlohmann::json EditorHistory::Describe() const
	{
		auto entries = nlohmann::json::array();
		for (const auto& entry : m_Entries)
			entries.push_back({ { "name", entry.Name }, { "bytes", entry.Bytes() } });
		return { { "canUndo", CanUndo() }, { "canRedo", CanRedo() }, { "undo", UndoName() }, { "redo", RedoName() }, { "transaction", m_Transaction }, { "index", m_Index }, { "entries", entries }, { "patchBytes", m_PatchBytes } };
	}

	std::string EditorHistory::CommandName(const std::string& command, const nlohmann::json& params)
	{
		if (command == "component.set" || command == "editor.component_set")
		{
			std::string name = "Set " + params.value("component", std::string());
			if (const auto data = params.find("data"); data != params.end() && data->is_object() && data->size() == 1)
				name += " " + data->begin().key();
			return name;
		}
		if (command == "component.remove")
			return "Remove " + params.value("component", std::string());
		if (command == "scene.settings" && params.size() == 1)
		{
			std::string name = "Set " + params.begin().key();
			if (params.begin()->is_object() && params.begin()->size() == 1)
				name += " " + params.begin()->begin().key();
			return name;
		}
		if (command == "entity.create")
			return "Create Entity";
		if (command == "entity.destroy" || command == "editor.delete")
			return "Delete Entities";
		if (command == "entity.duplicate" || command == "editor.duplicate")
			return "Duplicate Entities";
		if (command == "entity.rename")
			return "Rename Entity";
		if (command == "entity.set_parent")
			return "Reparent Entity";
		if (command == "editor.transform")
			return "Transform Selection";
		if (command == "editor.vertex_snap")
			return "Vertex Snap";
		return command;
	}

}
