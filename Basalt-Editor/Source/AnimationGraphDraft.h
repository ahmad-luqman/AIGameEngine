#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <functional>
#include <vector>

namespace Basalt {
	// Headless graph working copy: reference-preserving edits and validation shared by the panel and tests.
	class AnimationGraphDraft
	{
	public:
		bool Load(const std::string& path, std::string& error);
		bool SetData(const nlohmann::json& data, std::string& error);
		bool Validate(std::string& error) const;
		bool IsDirty() const { return Data != m_Saved; }
		void MarkSaved()
		{
			m_Saved = Data;
			ExternalChange = false;
		}
		bool RenameState(size_t index, const std::string& name);
		bool RenameParameter(size_t index, const std::string& name);
		void SetParameterType(size_t index, const std::string& type);
		void RemoveState(size_t index);
		void RemoveParameter(size_t index);
		static nlohmann::json Default();
		bool OnFileChanged(std::string& error);
		// Coalesce active widget drags: the panel records once after a completed edit.
		void Record();
		bool Undo();
		bool Redo();

		std::string Path;
		nlohmann::json Data;
		bool ExternalChange = false;

	private:
		nlohmann::json m_Saved;
		std::vector<nlohmann::json> m_Undo, m_Redo;
	};
	// A name text buffer independent of ImGui frames. Rejected text stays pending until corrected or cancelled.
	struct AnimationGraphNameEdit
	{
		std::string Text;
		bool Pending = false;
		std::string Problem;
		void Sync(const std::string& current);
		void Cancel(const std::string& current);
		bool Commit(const std::function<bool(const std::string&)>& rename);
	};

}
