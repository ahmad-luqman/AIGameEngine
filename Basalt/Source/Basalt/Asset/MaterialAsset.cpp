#include "Basalt/Asset/MaterialAsset.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/EnumNames.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Scene/Components.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/ranges.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Basalt::MaterialAsset {

	using json = nlohmann::json;

	const std::array<const char*, MaterialTextureSlotCount> SlotNames = { "Albedo", "Normal", "MetallicRoughness", "Occlusion", "Emissive", "Clearcoat", "ClearcoatRoughness", "ClearcoatNormal", "Transmission" };
	const std::array<const char*, MaterialTextureSlotCount> MapFields = { "AlbedoMap", "NormalMap", "MetallicRoughnessMap", "OcclusionMap", "EmissiveMap", "ClearcoatMap", "ClearcoatRoughnessMap", "ClearcoatNormalMap", "TransmissionMap" };

	namespace {

		// Every top-level key, in the order ToJson writes them.
		constexpr std::initializer_list<std::string_view> TopLevelKeys = { "Version", "AlbedoColor", "Metallic", "Roughness", "EmissiveColor", "EmissiveIntensity", "AlphaMode", "AlphaCutoff", "DoubleSided", "NormalScale", "OcclusionStrength", "Clearcoat", "ClearcoatRoughness", "ClearcoatNormalScale", "Transmission", "AlbedoMap", "NormalMap", "MetallicRoughnessMap", "OcclusionMap", "EmissiveMap", "ClearcoatMap", "ClearcoatRoughnessMap", "ClearcoatNormalMap", "TransmissionMap", "TextureTransforms" };
		constexpr std::initializer_list<std::string_view> TransformKeys = { "Offset", "Scale", "Rotation", "TexCoord" };

		// Reads fields while tracking the location a failure reports, like the animation graph parser.
		class MaterialParser
		{
		public:
			explicit MaterialParser(std::vector<std::string>* warnings)
				: m_Warnings(warnings)
			{
			}

			MaterialData Parse(const json& data)
			{
				Object(data, TopLevelKeys);
				m_Location = "Version";
				uint32_t version = 0;
				if (!data.contains("Version") || !JsonToUInt32(data["Version"], version))
					throw std::runtime_error("required, a nonnegative integer");
				if (version != FormatVersion)
					throw std::runtime_error(fmt::format("unsupported version {} (expected {})", version, FormatVersion));

				MaterialData material;
				Vector(data, "AlbedoColor", material.AlbedoColor);
				Number(data, "Metallic", material.Metallic);
				Number(data, "Roughness", material.Roughness);
				Vector(data, "EmissiveColor", material.EmissiveColor);
				Number(data, "EmissiveIntensity", material.EmissiveIntensity);
				if (const auto* mode = Find(data, "AlphaMode"))
				{
					const auto parsed = mode->is_string() ? EnumFromName<AlphaMode>(AlphaModeNames, mode->get_ref<const std::string&>()) : std::nullopt;
					if (!parsed)
						throw std::runtime_error("expected \"Opaque\", \"Mask\" or \"Blend\"");
					material.Alpha = *parsed;
				}
				Number(data, "AlphaCutoff", material.AlphaCutoff);
				if (const auto* doubleSided = Find(data, "DoubleSided"))
				{
					if (!doubleSided->is_boolean())
						throw std::runtime_error("expected a boolean");
					material.DoubleSided = doubleSided->get<bool>();
				}
				Number(data, "NormalScale", material.NormalScale);
				Number(data, "OcclusionStrength", material.OcclusionStrength);
				Number(data, "Clearcoat", material.Clearcoat);
				Number(data, "ClearcoatRoughness", material.ClearcoatRoughness);
				Number(data, "ClearcoatNormalScale", material.ClearcoatNormalScale);
				Number(data, "Transmission", material.Transmission);
				for (size_t slot = 0; slot < MaterialTextureSlotCount; slot++)
					if (const auto* reference = Find(data, MapFields[slot]))
						GetMap(material, static_cast<MaterialTextureSlot>(slot)) = Reference(*reference);
				if (const auto* transforms = Find(data, "TextureTransforms"))
				{
					if (!transforms->is_object())
						throw std::runtime_error("expected an object");
					for (const auto& [key, value] : transforms->items())
						if (std::find_if(SlotNames.begin(), SlotNames.end(), [&](const char* name) { return key == name; }) == SlotNames.end())
						{
							m_Location.clear();
							throw std::runtime_error("TextureTransforms: unknown slot '" + key + "' (expected " + fmt::format("{}", fmt::join(SlotNames, ", ")) + ")");
						}
					for (size_t slot = 0; slot < MaterialTextureSlotCount; slot++)
						if (const auto* transform = Find(*transforms, SlotNames[slot]))
						{
							const std::string location = "TextureTransforms." + std::string(SlotNames[slot]);
							m_Location = location;
							Object(*transform, TransformKeys);
							auto& value = material.TextureTransforms[slot];
							Vector(*transform, "Offset", value.Offset, location + ".");
							Vector(*transform, "Scale", value.Scale, location + ".");
							Number(*transform, "Rotation", value.Rotation, location + ".");
							if (const auto* texCoord = Find(*transform, "TexCoord", location + "."); texCoord && (!JsonToUInt32(*texCoord, value.TexCoord) || value.TexCoord > 1))
								throw std::runtime_error("expected 0 or 1");
						}
				}
				m_Location.clear();
				if (auto problem = Validate(material))
					throw std::runtime_error(*problem);
				return material;
			}

			const std::string& GetLocation() const { return m_Location; }

		private:
			void Object(const json& value, std::initializer_list<std::string_view> keys)
			{
				std::string error;
				if (!value.is_object())
					throw std::runtime_error("expected an object");
				if (!RejectUnknownKeys(value, keys, m_Location.empty() ? "material" : m_Location, error))
				{
					// The key names the problem; the location is already part of the message.
					m_Location.clear();
					throw std::runtime_error(error);
				}
			}

			const json* Find(const json& object, const char* key, const std::string& prefix = {})
			{
				const auto it = object.find(key);
				if (it == object.end())
					return nullptr;
				m_Location = prefix + key;
				return &*it;
			}

			void Number(const json& object, const char* key, float& value, const std::string& prefix = {})
			{
				if (const auto* element = Find(object, key, prefix); element && !JsonToFloat(*element, value))
					throw std::runtime_error("expected a finite float");
			}

			template<glm::length_t N>
			void Vector(const json& object, const char* key, glm::vec<N, float>& value, const std::string& prefix = {})
			{
				const auto* element = Find(object, key, prefix);
				if (!element)
					return;
				if (!element->is_array() || element->size() != N)
					throw std::runtime_error(fmt::format("expected an array of {} numbers", N));
				for (glm::length_t i = 0; i < N; i++)
					if (!JsonToFloat((*element)[i], value[i]))
						throw std::runtime_error(fmt::format("expected an array of {} finite floats", N));
			}

			std::string Reference(const json& value)
			{
				if (value.is_string())
					return value.get<std::string>();
				if (!value.is_object())
					throw std::runtime_error("asset reference must be a path string or {UUID, Path} strings");
				const auto& project = Project::GetActive();
				if (!project)
					throw std::runtime_error("UUID asset references require an active project");
				std::string path, error;
				if (!project->GetAssets().Decode(value, path, error, true))
					throw std::runtime_error(error);
				if (!error.empty() && m_Warnings)
					m_Warnings->push_back(m_Location + ": " + error);
				return path;
			}

			std::vector<std::string>* m_Warnings = nullptr;
			std::string m_Location;
		};

		// The shortest decimal that reads back as the same float: files are written by people and agents too, and 0.6
		// should not read 0.6000000238418579.
		json Shortest(float value)
		{
			// Read back by the JSON parser, which ignores the C locale's decimal separator (strtod would not).
			const json shortest = ParseJson(fmt::format("{}", value));
			// Near FLT_MAX the shortest decimal exceeds the float range as a double, which JsonToFloat would reject.
			float check = 0;
			return shortest.is_number() && JsonToFloat(shortest, check) && check == value ? shortest : json(static_cast<double>(value));
		}
		json ToJson(const glm::vec2& v)
		{
			return json::array({ Shortest(v.x), Shortest(v.y) });
		}
		json ToJson(const glm::vec3& v)
		{
			return json::array({ Shortest(v.x), Shortest(v.y), Shortest(v.z) });
		}
		json ToJson(const glm::vec4& v)
		{
			return json::array({ Shortest(v.x), Shortest(v.y), Shortest(v.z), Shortest(v.w) });
		}

		bool IsIdentity(const TextureTransform& transform)
		{
			return transform.Offset == glm::vec2(0) && transform.Scale == glm::vec2(1) && transform.Rotation == 0 && transform.TexCoord == 0;
		}

	}

	bool IsMaterialFile(std::string_view path)
	{
		return FileSystem::GetLowercaseExtension(FileSystem::PathFromUtf8(std::string(path))) == Extension;
	}

	std::string& GetMap(MaterialData& material, MaterialTextureSlot slot)
	{
		return const_cast<std::string&>(GetMap(std::as_const(material), slot));
	}

	const std::string& GetMap(const MaterialData& material, MaterialTextureSlot slot)
	{
		switch (slot)
		{
			case MaterialTextureSlot::Albedo:
				return material.AlbedoMap;
			case MaterialTextureSlot::Normal:
				return material.NormalMap;
			case MaterialTextureSlot::MetallicRoughness:
				return material.MetallicRoughnessMap;
			case MaterialTextureSlot::Occlusion:
				return material.OcclusionMap;
			case MaterialTextureSlot::Emissive:
				return material.EmissiveMap;
			case MaterialTextureSlot::Clearcoat:
				return material.ClearcoatMap;
			case MaterialTextureSlot::ClearcoatRoughness:
				return material.ClearcoatRoughnessMap;
			case MaterialTextureSlot::ClearcoatNormal:
				return material.ClearcoatNormalMap;
			case MaterialTextureSlot::Transmission:
				return material.TransmissionMap;
			case MaterialTextureSlot::Count:
				break;
		}
		BS_CORE_ASSERT(false, "invalid material texture slot");
		return material.AlbedoMap;
	}

	std::optional<std::string> Validate(const MaterialData& material)
	{
		// The glTF importer's rules (and the Material component's), plus finiteness everywhere: materials from files,
		// models and components are interchangeable, so none may be valid in one form and not another.
		for (glm::length_t i = 0; i < 4; i++)
			if (!std::isfinite(material.AlbedoColor[i]))
				return fmt::format("AlbedoColor must be finite (got {})", material.AlbedoColor[i]);
		for (glm::length_t i = 0; i < 3; i++)
			if (!std::isfinite(material.EmissiveColor[i]))
				return fmt::format("EmissiveColor must be finite (got {})", material.EmissiveColor[i]);
		const std::pair<const char*, float> finiteFields[] = { { "Metallic", material.Metallic }, { "Roughness", material.Roughness }, { "AlphaCutoff", material.AlphaCutoff }, { "NormalScale", material.NormalScale }, { "ClearcoatNormalScale", material.ClearcoatNormalScale } };
		for (const auto& [name, value] : finiteFields)
			if (!std::isfinite(value))
				return fmt::format("{} {} must be finite", name, value);
		const std::pair<const char*, float> unitFields[] = { { "OcclusionStrength", material.OcclusionStrength }, { "Clearcoat", material.Clearcoat }, { "ClearcoatRoughness", material.ClearcoatRoughness }, { "Transmission", material.Transmission } };
		for (const auto& [name, value] : unitFields)
			if (!std::isfinite(value) || value < 0 || value > 1)
				return fmt::format("{} {} must be in [0, 1]", name, value);
		if (!std::isfinite(material.EmissiveIntensity) || material.EmissiveIntensity < 0)
			return fmt::format("EmissiveIntensity {} must be finite and nonnegative", material.EmissiveIntensity);
		if (static_cast<uint32_t>(material.Alpha) > static_cast<uint32_t>(AlphaMode::Blend))
			return "AlphaMode is not Opaque, Mask or Blend";
		for (size_t slot = 0; slot < MaterialTextureSlotCount; slot++)
		{
			const auto& transform = material.TextureTransforms[slot];
			if (!Math::IsFinite(transform.Offset) || !Math::IsFinite(transform.Scale) || !std::isfinite(transform.Rotation))
				return fmt::format("TextureTransforms.{} must be finite", SlotNames[slot]);
			if (transform.TexCoord > 1)
				return fmt::format("TextureTransforms.{}.TexCoord {} must be 0 or 1", SlotNames[slot], transform.TexCoord);
		}
		return std::nullopt;
	}

	std::optional<MaterialData> FromJson(const json& data, std::string& outError, std::vector<std::string>* outWarnings)
	{
		MaterialParser parser(outWarnings);
		try
		{
			return parser.Parse(data);
		}
		catch (const std::exception& error)
		{
			outError = parser.GetLocation().empty() ? std::string(error.what()) : parser.GetLocation() + ": " + error.what();
		}
		return std::nullopt;
	}

	std::optional<json> ToJson(const MaterialData& material, bool encodeReferences, std::string& outError)
	{
		if (auto problem = Validate(material))
		{
			outError = *problem;
			return std::nullopt;
		}
		const auto& project = Project::GetActive();
		if (encodeReferences && !project)
		{
			outError = "encoding material references requires an active project";
			return std::nullopt;
		}
		json data = {
			{ "Version", FormatVersion },
			{ "AlbedoColor", ToJson(material.AlbedoColor) },
			{ "Metallic", Shortest(material.Metallic) },
			{ "Roughness", Shortest(material.Roughness) },
			{ "EmissiveColor", ToJson(material.EmissiveColor) },
			{ "EmissiveIntensity", Shortest(material.EmissiveIntensity) },
			{ "AlphaMode", AlphaModeNames[static_cast<size_t>(material.Alpha)] },
			{ "AlphaCutoff", Shortest(material.AlphaCutoff) },
			{ "DoubleSided", material.DoubleSided },
			{ "NormalScale", Shortest(material.NormalScale) },
			{ "OcclusionStrength", Shortest(material.OcclusionStrength) },
			{ "Clearcoat", Shortest(material.Clearcoat) },
			{ "ClearcoatRoughness", Shortest(material.ClearcoatRoughness) },
			{ "ClearcoatNormalScale", Shortest(material.ClearcoatNormalScale) },
			{ "Transmission", Shortest(material.Transmission) },
		};
		for (size_t slot = 0; slot < MaterialTextureSlotCount; slot++)
		{
			const std::string& path = GetMap(material, static_cast<MaterialTextureSlot>(slot));
			json reference = path;
			if (encodeReferences && !project->GetAssets().Encode(path, reference, outError))
			{
				outError.insert(0, std::string(MapFields[slot]) + ": ");
				return std::nullopt;
			}
			data[MapFields[slot]] = std::move(reference);
		}
		json transforms = json::object();
		for (size_t slot = 0; slot < MaterialTextureSlotCount; slot++)
		{
			const auto& transform = material.TextureTransforms[slot];
			if (!IsIdentity(transform))
				transforms[SlotNames[slot]] = { { "Offset", ToJson(transform.Offset) }, { "Scale", ToJson(transform.Scale) }, { "Rotation", Shortest(transform.Rotation) }, { "TexCoord", transform.TexCoord } };
		}
		if (!transforms.empty())
			data["TextureTransforms"] = std::move(transforms);
		return data;
	}

	bool EncodeReferences(json& data, std::string& outError, const std::string& onlyPath)
	{
		const auto& project = Project::GetActive();
		if (!project || !data.is_object())
			return true;
		for (const char* field : MapFields)
		{
			auto value = data.find(field);
			if (value == data.end() || !value->is_string())
				continue;
			const std::string path = value->get<std::string>();
			if (!onlyPath.empty() && !project->GetAssets().ReferencesSource(path, onlyPath))
				continue;
			json reference;
			if (!project->GetAssets().Encode(path, reference, outError))
			{
				outError.insert(0, std::string(field) + ": ");
				return false;
			}
			*value = std::move(reference);
		}
		return true;
	}

	MaterialData FromComponent(const MaterialComponent& component)
	{
		MaterialData material;
		material.AlbedoColor = component.AlbedoColor;
		material.Metallic = component.Metallic;
		material.Roughness = component.Roughness;
		material.EmissiveColor = component.EmissiveColor;
		material.EmissiveIntensity = component.EmissiveIntensity;
		material.AlbedoMap = component.AlbedoMap;
		material.NormalMap = component.NormalMap;
		material.MetallicRoughnessMap = component.MetallicRoughnessMap;
		material.OcclusionMap = component.OcclusionMap;
		material.EmissiveMap = component.EmissiveMap;
		material.Clearcoat = component.Clearcoat;
		material.ClearcoatRoughness = component.ClearcoatRoughness;
		material.Transmission = component.Transmission;
		material.ClearcoatMap = component.ClearcoatMap;
		material.ClearcoatRoughnessMap = component.ClearcoatRoughnessMap;
		material.ClearcoatNormalMap = component.ClearcoatNormalMap;
		material.TransmissionMap = component.TransmissionMap;
		// Overrides have no alpha mode: alpha below one blends and is coverage.
		material.Alpha = component.AlbedoColor.a < 0.999f ? AlphaMode::Blend : AlphaMode::Opaque;
		material.TextureTransforms.fill({ component.UVOffset, component.UVScale, component.UVRotation, 0 });
		return material;
	}

	std::vector<std::string> GetTextures(const MaterialData& material)
	{
		std::vector<std::string> textures;
		for (size_t slot = 0; slot < MaterialTextureSlotCount; slot++)
			if (const auto& map = GetMap(material, static_cast<MaterialTextureSlot>(slot)); !map.empty())
				textures.push_back(map);
		return textures;
	}

}
