#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace Basalt {

	// Named scene transactions retain reversible patches and one current state, independent of the GUI.
	class EditorHistory
	{
	public:
		static constexpr size_t MaxEntries = 128;
		static constexpr size_t MaxPatchBytes = size_t{ 32 } * 1024 * 1024;
		using Restore = std::function<bool(const nlohmann::json&)>;

		void Reset(nlohmann::json state);
		void Commit(nlohmann::json state, const std::string& name);
		bool Begin(const std::string& name);
		bool End(nlohmann::json state);
		bool Cancel(const Restore& restore);
		bool IsActive() const { return !m_Transaction.empty(); }
		bool CanUndo() const { return !IsActive() && m_Index > 0; }
		bool CanRedo() const { return !IsActive() && m_Index < m_Entries.size(); }
		bool Undo(const Restore& restore);
		bool Redo(const Restore& restore);
		std::string UndoName() const;
		std::string RedoName() const;
		nlohmann::json Describe() const;
		size_t GetSize() const { return m_Entries.size(); }
		size_t GetPatchBytes() const { return m_PatchBytes; }
		static std::string CommandName(const std::string& command, const nlohmann::json& params);

	private:
		struct Entry
		{
			std::string Name, Forward, Backward;
			size_t Bytes() const { return Name.size() + Forward.size() + Backward.size(); }
		};
		bool Move(bool forward, const Restore& restore);
		nlohmann::json m_Current;
		std::vector<Entry> m_Entries;
		std::string m_Transaction;
		size_t m_Index = 0, m_PatchBytes = 0;
	};

}
