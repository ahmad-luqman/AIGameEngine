#pragma once

#include "Basalt/Asset/MeshSource.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	// The material editor's working copy of one material file (headless, so tests cover it). Edits change an
	// editable JSON form of the file: every field present, texture references as paths, numbers as floats and one
	// TextureTransforms entry per slot. MakeFileText produces the canonical form a save writes (UUID references,
	// identity transforms omitted); the panel writes it through asset.write and then calls MarkSaved with it.
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
		// The edits change the material (an edit that is undone, or a value dragged back, is not one).
		bool IsDirty() const;
		// What loading reported: texture references whose UUID no longer resolves (shown as asset-uuid:// keys).
		const std::vector<std::string>& GetWarnings() const { return m_Warnings; }

		// The edited material, or nullopt with the reason (an invalid value typed into a field).
		std::optional<MaterialData> Parse(std::string& outError) const;
		// The file text a save writes (UUID references), or nullopt with the reason. Needs an active project.
		std::optional<std::string> MakeFileText(std::string& outError) const;
		// The save of `writtenText` (MakeFileText's) went through: it is the new baseline, and the draft shows it.
		bool MarkSaved(const std::string& writtenText, std::string& outError);
		// Discards the edits, reading the file again (it may have changed on disk).
		bool Revert(std::string& outError);

		// The file was changed by something else (an agent, another tool): a clean draft follows it (true when it read
		// the new version), a dirty one keeps its edits and reports the conflict (HasExternalChange) until Revert or Save.
		// A new version that cannot be read is a conflict too (outError says why).
		bool OnFileChanged(std::string& outError);
		bool HasExternalChange() const { return m_ExternalChange; }
		// asset.move/asset.rename moved `from` to `to`: the draft follows its own file and rewrites the maps that name the
		// moved file (or its sub-assets), in the edits and the baseline alike, so a later save keeps them valid.
		void OnAssetMoved(const std::string& from, const std::string& to);

		// The editable form of a material (also the form of a fresh draft).
		static nlohmann::json ToEditable(const MaterialData& material);

	private:
		// The material's file form without UUIDs, or null when the edits are invalid: what dirtiness compares.
		nlohmann::json Canonical(const nlohmann::json& editable) const;
		void SetBaseline(nlohmann::json editable);

		std::string m_Path;
		nlohmann::json m_Data;
		nlohmann::json m_Saved;
		nlohmann::json m_SavedCanonical;
		std::vector<std::string> m_Warnings;
		bool m_ExternalChange = false;
	};

}
