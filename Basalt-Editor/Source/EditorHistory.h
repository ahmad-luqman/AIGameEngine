#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace Basalt {

	// Snapshot-based undo/redo: every committed edit stores the whole scene as JSON. Simple and always
	// correct (any edit, from any panel or automation command, is undoable); scenes are small enough.
	class EditorHistory
	{
	public:
		static constexpr size_t MaxSnapshots = 128;

		// Starts a new history with the given state (scene opened or created).
		void Reset(nlohmann::json state);
		// Records the state after an edit. Identical consecutive states are ignored.
		void Commit(nlohmann::json state);

		bool CanUndo() const { return m_Index > 0; }
		bool CanRedo() const { return m_Index + 1 < m_States.size(); }
		// Return the state to restore.
		const nlohmann::json& Undo();
		const nlohmann::json& Redo();

		size_t GetSize() const { return m_States.size(); }

	private:
		std::vector<nlohmann::json> m_States;
		size_t m_Index = 0;
	};

}
