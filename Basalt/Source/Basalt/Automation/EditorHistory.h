#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	// Named scene transactions retain reversible patches and one current state, independent of the GUI.
	class EditorHistory
	{
	public:
		static constexpr size_t MaxEntries = 128;
		static constexpr size_t MaxPatchBytes = size_t{ 32 } * 1024 * 1024;
		using Restore = std::function<bool(nlohmann::json&, std::string& error)>;

		// Replaces the baseline and clears history/transactions. Failure preserves existing state.
		bool Reset(nlohmann::json state);
		// Captures a standalone edit; rejects an active transaction. No-op edits preserve redo.
		bool Commit(nlohmann::json state, const std::string& name);
		// Transactions retain the pre-edit baseline until End or a successful Cancel. They cannot nest.
		bool Begin(const std::string& name);
		bool End(nlohmann::json state);
		bool Cancel(const Restore& restore);
		bool IsActive() const { return m_Transaction.has_value(); }
		std::string TransactionName() const { return m_Transaction.value_or(""); }
		const std::string& GetError() const { return m_Error; }
		const std::string& GetWarning() const { return m_Warning; }
		bool CanUndo() const { return !IsActive() && m_Index > 0; }
		bool CanRedo() const { return !IsActive() && m_Index < m_Entries.size(); }
		// Restore reports failure without changing the live scene and replaces its JSON argument with
		// the canonical restored state (e.g. normalized rotations). Cursor/base move only on success.
		bool Undo(const Restore& restore);
		bool Redo(const Restore& restore);
		std::string UndoName() const;
		std::string RedoName() const;
		// Ordered entry labels/payload sizes, cursor, capabilities and active transaction for UI/API clients.
		nlohmann::json Describe() const;
		size_t GetSize() const { return m_Entries.size(); }
		size_t GetPatchBytes() const { return m_PatchBytes; }
		// Human-readable labels for top-level command captures.
		static std::string CommandName(const std::string& command, const nlohmann::json& params);

	private:
		struct Entry
		{
			std::string Name, Forward, Backward;
			size_t Bytes() const { return Name.size() + Forward.size() + Backward.size(); }
		};
		bool Move(bool forward, const Restore& restore);
		bool Fail(const std::string& operation, const std::string& reason);
		nlohmann::json m_Current;
		std::vector<Entry> m_Entries;
		std::optional<std::string> m_Transaction;
		std::string m_Error, m_Warning;
		size_t m_Index = 0, m_PatchBytes = 0;
	};

}
