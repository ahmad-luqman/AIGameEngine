#include "Basalt/Automation/EditorHistory.h"

#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"

#include <algorithm>
#include <string_view>
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

	bool EditorHistory::Fail(const std::string& operation, const std::string& reason)
	{
		m_Error = operation + ": " + reason;
		BS_CORE_ERROR("History {}", m_Error);
		return false;
	}

	bool EditorHistory::Reset(nlohmann::json state)
	{
		try
		{
			state = Encode(std::move(state));
			m_Current = std::move(state);
			m_Entries.clear();
			m_Transaction.reset();
			m_Index = m_PatchBytes = 0;
			m_Error.clear();
			m_Warning.clear();
			return true;
		}
		catch (const std::exception& error)
		{
			return Fail("Reset failed", error.what());
		}
	}

	bool EditorHistory::Commit(nlohmann::json state, const std::string& name)
	{
		m_Error.clear();
		m_Warning.clear();
		if (IsActive())
			return Fail("Commit rejected", "finish the active transaction first");
		try
		{
			state = Encode(std::move(state));
			if (state == m_Current)
				return true;
			Entry entry{ name.empty() ? "Edit Scene" : name, nlohmann::json::diff(m_Current, state).dump(), nlohmann::json::diff(state, m_Current).dump() };
			// Allocate before truncating redo, so a failed capture does not destroy retained history.
			m_Entries.reserve(m_Index + 1);
			while (m_Entries.size() > m_Index)
			{
				m_PatchBytes -= m_Entries.back().Bytes();
				m_Entries.pop_back();
			}
			const bool oversized = entry.Bytes() > MaxPatchBytes;
			m_PatchBytes += entry.Bytes();
			m_Entries.push_back(std::move(entry));
			m_Current = std::move(state);
			while (m_Entries.size() > MaxEntries || m_PatchBytes > MaxPatchBytes)
			{
				m_PatchBytes -= m_Entries.front().Bytes();
				m_Entries.erase(m_Entries.begin());
			}
			m_Index = m_Entries.size();
			if (oversized)
			{
				m_Warning = "Edit too large to undo; history cleared";
				BS_CORE_WARN("History: {} ({})", m_Warning, name);
			}
			return true;
		}
		catch (const std::exception& error)
		{
			return Fail("Capture failed", error.what());
		}
	}

	bool EditorHistory::Begin(const std::string& name)
	{
		m_Error.clear();
		if (IsActive() || name.empty())
			return false;
		m_Transaction = name;
		return true;
	}

	bool EditorHistory::End(nlohmann::json state)
	{
		auto transaction = std::exchange(m_Transaction, std::nullopt);
		if (!transaction)
			return false;
		if (Commit(std::move(state), *transaction))
			return true;
		m_Transaction = std::move(transaction);
		return false;
	}

	bool EditorHistory::Cancel(const Restore& restore)
	{
		m_Error.clear();
		if (!IsActive())
			return false;
		try
		{
			std::string error;
			auto restored = Decode(m_Current);
			if (!restore(restored, error))
				return Fail("Cancel failed", error.empty() ? "scene restoration failed" : error);
			m_Current = Encode(std::move(restored));
			m_Transaction.reset();
			return true;
		}
		catch (const std::exception& error)
		{
			return Fail("Cancel failed", error.what());
		}
	}

	bool EditorHistory::Move(bool forward, const Restore& restore)
	{
		m_Error.clear();
		if (forward ? !CanRedo() : !CanUndo())
			return false;
		const std::string operation = forward ? "Redo failed" : "Undo failed";
		try
		{
			const Entry& entry = m_Entries[forward ? m_Index : m_Index - 1];
			const auto patch = ParseJson(forward ? entry.Forward : entry.Backward);
			auto next = m_Current.patch(patch);
			std::string error;
			auto restored = Decode(next);
			if (!restore(restored, error))
				return Fail(operation, error.empty() ? "scene restoration failed" : error);
			m_Current = Encode(std::move(restored));
			if (forward)
				++m_Index;
			else
				--m_Index;
			return true;
		}
		catch (const std::exception& error)
		{
			return Fail(operation, error.what());
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
		return { { "canUndo", CanUndo() }, { "canRedo", CanRedo() }, { "undo", UndoName() }, { "redo", RedoName() }, { "transaction", TransactionName() }, { "index", m_Index }, { "entries", entries }, { "patchBytes", m_PatchBytes } };
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
		static const std::pair<std::string_view, std::string_view> Names[] = {
			{ "entity.create", "Create Entity" }, { "entity.destroy", "Delete Entities" }, { "editor.delete", "Delete Entities" }, { "entity.duplicate", "Duplicate Entities" }, { "editor.duplicate", "Duplicate Entities" }, { "entity.rename", "Rename Entity" }, { "entity.set_parent", "Reparent Entity" }, { "editor.transform", "Transform Selection" }, { "editor.vertex_snap", "Vertex Snap" }
		};
		for (const auto& [key, label] : Names)
			if (command == key)
				return std::string(label);
		return command;
	}

}
