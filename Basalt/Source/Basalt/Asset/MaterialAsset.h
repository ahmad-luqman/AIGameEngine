#pragma once

#include "Basalt/Asset/MeshSource.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Basalt {

	struct MaterialComponent;

	// Reads and writes material asset files (.bmaterial): a JSON object holding the full imported material model
	// (MaterialData), so authored materials keep alpha modes, sidedness, normal/occlusion strengths and per-map UV
	// transforms and sets. Texture fields are asset references like component asset fields: a path string or a
	// {UUID, Path} object (decoded through the active project; moves keep them valid). Readers are strict: unknown
	// keys, wrong types, non-finite numbers and out-of-range factors fail with the field's location.
	namespace MaterialAsset {

		inline constexpr uint32_t FormatVersion = 1;
		inline constexpr std::string_view Extension = ".bmaterial";

		// Keys of TextureTransforms in files, indexed by MaterialTextureSlot ("Albedo", "Normal", ...).
		extern const std::array<const char*, MaterialTextureSlotCount> SlotNames;
		// Texture field names, indexed by MaterialTextureSlot ("AlbedoMap", ...): the same as MaterialComponent's.
		extern const std::array<const char*, MaterialTextureSlotCount> MapFields;
		// Alpha mode names, indexed by AlphaMode.
		inline constexpr const char* AlphaModeNames[] = { "Opaque", "Mask", "Blend" };

		bool IsMaterialFile(std::string_view path);

		// The texture key of a slot.
		std::string& GetMap(MaterialData& material, MaterialTextureSlot slot);
		const std::string& GetMap(const MaterialData& material, MaterialTextureSlot slot);

		// The problem with a material's values (the field and value), or nullopt when the renderer can use them: the glTF
		// importer's rules (which also applies this), so a material is valid as a file exactly when it is as a model's:
		// finite numbers, clearcoat, clearcoat roughness, transmission and occlusion strength in [0, 1], a finite
		// nonnegative emissive intensity, an alpha mode, and texture coordinate sets 0 or 1. Inline Material components
		// always pass once the registry accepts them.
		std::optional<std::string> Validate(const MaterialData& material);

		// Parses a material file. Missing keys keep MaterialData's defaults; "Version" is required. UUID references
		// that no longer resolve stay as opaque keys (rendered without that texture) and are reported in
		// outWarnings; they fail without an active project. Name stays empty (AssetManager names materials by file).
		std::optional<MaterialData> FromJson(const nlohmann::json& data, std::string& outError, std::vector<std::string>* outWarnings = nullptr);

		// Writes every field, plus TextureTransforms entries that differ from the identity on UV0. With encodeReferences,
		// textures that exist under Assets/ become {UUID, Path} objects (creating metadata), as scene saves do; that
		// needs an active project and fails with the problem otherwise.
		std::optional<nlohmann::json> ToJson(const MaterialData& material, bool encodeReferences, std::string& outError);

		// Converts the path-string texture references of a parsed file to {UUID, Path} objects; only references to
		// onlyPath (and its sub-assets) when given. Move commands migrate material files with this. Needs an active
		// project (no-op without one).
		bool EncodeReferences(nlohmann::json& data, std::string& outError, const std::string& onlyPath = {});

		// The inline MaterialComponent override in the full model: its single UV transform applies to every map on UV0,
		// strengths are 1, it is single-sided, and alpha below one blends (alpha is coverage). The renderer draws
		// inline overrides through this, so both forms share one material path.
		MaterialData FromComponent(const MaterialComponent& component);

		// The non-empty texture keys of a material, in slot order.
		std::vector<std::string> GetTextures(const MaterialData& material);

	}

}
