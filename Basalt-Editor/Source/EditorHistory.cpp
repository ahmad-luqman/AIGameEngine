#include "EditorHistory.h"

namespace Basalt {

	void EditorHistory::Reset(nlohmann::json state)
	{
		m_States.clear();
		m_States.push_back(std::move(state));
		m_Index = 0;
	}

	void EditorHistory::Commit(nlohmann::json state)
	{
		if (!m_States.empty() && m_States[m_Index] == state)
			return;
		// A new edit discards the redo branch.
		m_States.resize(m_Index + 1);
		m_States.push_back(std::move(state));
		if (m_States.size() > MaxSnapshots)
			m_States.erase(m_States.begin());
		m_Index = m_States.size() - 1;
	}

	const nlohmann::json& EditorHistory::Undo()
	{
		if (CanUndo())
			m_Index--;
		return m_States[m_Index];
	}

	const nlohmann::json& EditorHistory::Redo()
	{
		if (CanRedo())
			m_Index++;
		return m_States[m_Index];
	}

}
