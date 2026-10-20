#pragma once

#include "Basalt/Asset/MeshSource.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace Basalt {

	// The material editor's working copy of one material file (headless, so tests cover it). Edits change an
	// editable JSON form of the file: every field present, texture references as paths, and one TextureTransforms entry
	// per slot. Saving writes the canonical form (UUID references, identity transforms omitted) through asset.write.
	class MaterialAssetDraft
	{
	public:
		// Reads a project-relative .bmaterial. On failure (unreadable or invalid) the draft is left unchanged.
		bool Load(const std::string& path, std::string& outError);
		bool IsOpen() const { return !m_Path.empty(); }
		void Close();

		const std::string& GetPath() const { return m_Path; }
		// The editable JSON; widgets change it in place.
		nlohmann::json& GetData() { return m_Data; }
		const nlohmann::json& GetData() const { return m_Data; }
		// Edited since the last load or save.
		bool IsDirty() const { return m_Data != m_Saved; }

		// The edited material, or nullopt with the reason (an invalid value typed into a field).
		std::optional<MaterialData> Parse(std::string& outError) const;
		// The file text a save writes (UUID references), or nullopt with the reason. Needs an active project.
		std::optional<std::string> MakeFileText(std::string& outError) const;
		// The save went through: what was written is the new baseline.
		void MarkSaved();
		// Discards the edits, reading the file again (it may have changed on disk).
		bool Revert(std::string& outError);

		// The file was changed by something else (an agent, another tool): a clean draft follows it (true when it read
		// the new version), a dirty one keeps its edits and reports the conflict (HasExternalChange) until Revert or Save.
		bool OnFileChanged(std::string& outError);
		bool HasExternalChange() const { return m_ExternalChange; }

		// The editable form of a material (also the form of a fresh draft).
		static nlohmann::json ToEditable(const MaterialData& material);

	private:
		std::string m_Path;
		nlohmann::json m_Data;
		nlohmann::json m_Saved;
		bool m_ExternalChange = false;
	};

}
