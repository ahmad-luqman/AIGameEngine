#include "MaterialAssetDraft.h"

#include "Basalt/Asset/MaterialAsset.h"
#include "Basalt/Core/Assert.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"
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

		std::optional<nlohmann::json> ReadEditable(const std::string& path, std::string& outError)
		{
			const auto text = FileSystem::ReadTextFile(Project::ResolvePath(path));
			if (!text)
			{
				outError = "cannot read '" + path + "'";
				return std::nullopt;
			}
			const auto data = ParseJson(*text, outError);
			if (data.is_discarded())
				return std::nullopt;
			const auto material = MaterialAsset::FromJson(data, outError);
			if (!material)
				return std::nullopt;
			return MaterialAssetDraft::ToEditable(*material);
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
		for (const char* slot : MaterialAsset::SlotNames)
			if (!transforms.contains(slot))
				transforms[slot] = { { "Offset", { 0.0f, 0.0f } }, { "Scale", { 1.0f, 1.0f } }, { "Rotation", 0.0f }, { "TexCoord", 0 } };
		MakeFloats(*data);
		return std::move(*data);
	}

	bool MaterialAssetDraft::Load(const std::string& path, std::string& outError)
	{
		auto data = ReadEditable(path, outError);
		if (!data)
			return false;
		m_Path = path;
		m_Data = *data;
		m_Saved = std::move(*data);
		m_ExternalChange = false;
		return true;
	}

	void MaterialAssetDraft::Close()
	{
		m_Path.clear();
		m_Data = m_Saved = nlohmann::json();
		m_ExternalChange = false;
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

	void MaterialAssetDraft::MarkSaved()
	{
		m_Saved = m_Data;
		m_ExternalChange = false;
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
		return Load(m_Path, outError);
	}

}
