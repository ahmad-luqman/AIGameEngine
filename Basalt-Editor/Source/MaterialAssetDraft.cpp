#include "MaterialAssetDraft.h"

#include "Basalt/Asset/AssetKey.h"
#include "Basalt/Asset/MaterialAsset.h"
#include "Basalt/Core/Assert.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Project/Project.h"

namespace Basalt {

	namespace {

		// Files write whole numbers as integers; widgets would then edit them as integers (Metallic 0 could never be
		// dragged to 0.5). Every number but the version and texture coordinate sets is a float in the editable form.
		void MakeFloats(nlohmann::json& value, const std::string& key = {})
		{
			if (value.is_number() && key != "TexCoord" && key != "Version")
				value = value.get<double>();
			else if (value.is_array())
				for (auto& element : value)
					MakeFloats(element, key);
			else if (value.is_object())
				for (auto& [name, element] : value.items())
					MakeFloats(element, name);
		}

	}

	nlohmann::json MaterialAssetDraft::ToEditable(const MaterialData& material)
	{
		std::string error;
		auto data = MaterialAsset::ToJson(material, false, error);
		BS_CORE_ASSERT(data, "drafts hold materials that passed MaterialAsset::Validate");
		if (!data)
			return nlohmann::json::object();
		// Every slot's transform is editable, not only those that differ from the identity.
		auto& transforms = (*data)["TextureTransforms"];
		if (!transforms.is_object())
			transforms = nlohmann::json::object();
		for (const MaterialAsset::SlotInfo& slot : MaterialAsset::Slots)
			if (!transforms.contains(slot.Name))
				transforms[slot.Name] = { { "Offset", { 0.0f, 0.0f } }, { "Scale", { 1.0f, 1.0f } }, { "Rotation", 0.0f }, { "TexCoord", 0 } };
		MakeFloats(*data);
		return std::move(*data);
	}

	nlohmann::json MaterialAssetDraft::Canonical(const nlohmann::json& editable) const
	{
		std::string error;
		const auto material = MaterialAsset::FromJson(editable, error);
		return material ? MaterialAsset::ToJson(*material, false, error).value_or(nlohmann::json()) : nlohmann::json();
	}

	void MaterialAssetDraft::SetBaseline(nlohmann::json editable)
	{
		m_Data = editable;
		m_SavedCanonical = Canonical(editable);
		m_Saved = std::move(editable);
		m_ExternalChange = false;
	}

	bool MaterialAssetDraft::Load(const std::string& path, std::string& outError)
	{
		const auto text = FileSystem::ReadTextFile(Project::ResolvePath(path));
		if (!text)
		{
			outError = "cannot read '" + path + "'";
			return false;
		}
		std::vector<std::string> warnings;
		const auto material = MaterialAsset::FromText(*text, outError, &warnings);
		if (!material)
			return false;
		m_Path = path;
		m_Warnings = std::move(warnings);
		SetBaseline(ToEditable(*material));
		return true;
	}

	void MaterialAssetDraft::Close()
	{
		m_Path.clear();
		m_Data = m_Saved = m_SavedCanonical = nlohmann::json();
		m_Warnings.clear();
		m_ExternalChange = false;
	}

	bool MaterialAssetDraft::IsDirty() const
	{
		if (m_Data == m_Saved)
			return false;
		const nlohmann::json canonical = Canonical(m_Data);
		return canonical.is_null() || canonical != m_SavedCanonical;
	}

	std::optional<MaterialData> MaterialAssetDraft::Parse(std::string& outError) const
	{
		return MaterialAsset::FromJson(m_Data, outError);
	}

	std::optional<std::string> MaterialAssetDraft::MakeFileText(std::string& outError) const
	{
		const auto material = Parse(outError);
		if (!material)
			return std::nullopt;
		const auto data = MaterialAsset::ToJson(*material, true, outError);
		if (!data)
			return std::nullopt;
		return data->dump(1, '\t');
	}

	bool MaterialAssetDraft::MarkSaved(const std::string& writtenText, std::string& outError)
	{
		std::vector<std::string> warnings;
		const auto material = MaterialAsset::FromText(writtenText, outError, &warnings);
		if (!material)
			return false;
		m_Warnings = std::move(warnings);
		SetBaseline(ToEditable(*material));
		return true;
	}

	bool MaterialAssetDraft::Revert(std::string& outError)
	{
		return IsOpen() && Load(m_Path, outError);
	}

	bool MaterialAssetDraft::OnFileChanged(std::string& outError)
	{
		if (!IsOpen())
			return false;
		if (IsDirty())
		{
			m_ExternalChange = true;
			return false;
		}
		if (Load(m_Path, outError))
			return true;
		// The draft no longer matches the file: saving it would replace what was written meanwhile.
		m_ExternalChange = true;
		return false;
	}

	void MaterialAssetDraft::OnAssetMoved(const std::string& from, const std::string& to)
	{
		const auto& project = Project::GetActive();
		if (!IsOpen() || !project)
			return;
		const AssetRegistry& assets = project->GetAssets();
		if (assets.ReferencesSource(m_Path, from))
			m_Path = to;
		for (nlohmann::json* data : { &m_Data, &m_Saved })
		{
			if (!data->is_object())
				continue;
			for (const MaterialAsset::SlotInfo& slot : MaterialAsset::Slots)
			{
				if (!data->contains(slot.MapField))
					continue;
				nlohmann::json& value = (*data)[slot.MapField];
				if (!value.is_string())
					continue;
				const std::string key = value.get<std::string>();
				if (assets.ReferencesSource(key, from))
					value = to + std::string(GetSubAssetFragment(key));
			}
		}
		m_SavedCanonical = Canonical(m_Saved);
	}

}
