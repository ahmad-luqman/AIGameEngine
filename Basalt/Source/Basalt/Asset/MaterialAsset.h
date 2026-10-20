#pragma once

#include "Basalt/Asset/MeshSource.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Basalt {

	class AssetRegistry;
	struct MaterialComponent;

	// Reads and writes material asset files (.bmaterial): a JSON object holding the full material model (MaterialData),
	// so authored materials keep alpha modes, sidedness, normal/occlusion strengths and per-map UV transforms and sets.
	// Texture fields are asset references like component asset fields: a path string or a {UUID, Path} object (moves
	// keep them valid). Readers are strict: unknown keys, wrong types, non-finite numbers and out-of-range factors fail
	// with the field's location.
	namespace MaterialAsset {

		inline constexpr uint32_t FormatVersion = 1;
		inline constexpr std::string_view Extension = ".bmaterial";

		// One texture slot as files name it. Indexed by MaterialTextureSlot: the one table the file format, the renderer's
		// bindings and the editor share, so a slot's names and field cannot drift apart. (MaterialComponent's maps, in
		// the .cpp, follow the same order; a test pins every slot of both.)
		struct SlotInfo
		{
			// Key in TextureTransforms ("Albedo").
			const char* Name;
			// The texture field, named like MaterialComponent's ("AlbedoMap").
			const char* MapField;
			std::string MaterialData::* Map;
		};
		inline constexpr SlotInfo Slots[] = {
			{ "Albedo", "AlbedoMap", &MaterialData::AlbedoMap },
			{ "Normal", "NormalMap", &MaterialData::NormalMap },
			{ "MetallicRoughness", "MetallicRoughnessMap", &MaterialData::MetallicRoughnessMap },
			{ "Occlusion", "OcclusionMap", &MaterialData::OcclusionMap },
			{ "Emissive", "EmissiveMap", &MaterialData::EmissiveMap },
			{ "Clearcoat", "ClearcoatMap", &MaterialData::ClearcoatMap },
			{ "ClearcoatRoughness", "ClearcoatRoughnessMap", &MaterialData::ClearcoatRoughnessMap },
			{ "ClearcoatNormal", "ClearcoatNormalMap", &MaterialData::ClearcoatNormalMap },
			{ "Transmission", "TransmissionMap", &MaterialData::TransmissionMap },
		};
		static_assert(std::size(Slots) == MaterialTextureSlotCount, "one SlotInfo per MaterialTextureSlot");
		// The other top-level fields after "Version", in the order the Material Editor shows them (written files sort their
		// keys); then the maps and TextureTransforms. A test keeps this list equal to what ToJson writes.
		inline constexpr const char* ScalarFields[] = { "AlbedoColor", "Metallic", "Roughness", "EmissiveColor", "EmissiveIntensity", "AlphaMode", "AlphaCutoff", "DoubleSided", "NormalScale", "OcclusionStrength", "Clearcoat", "ClearcoatRoughness", "ClearcoatNormalScale", "Transmission" };
		// The keys of one TextureTransforms entry.
		inline constexpr const char* TransformFields[] = { "Offset", "Scale", "Rotation", "TexCoord" };
		// Alpha mode names, indexed by AlphaMode.
		inline constexpr const char* AlphaModeNames[] = { "Opaque", "Mask", "Blend" };
		static_assert(std::size(AlphaModeNames) == static_cast<size_t>(AlphaMode::Blend) + 1, "one name per AlphaMode");

		bool IsMaterialFile(std::string_view path);
		// A key MaterialComponent::Material accepts: a .bmaterial file or a model's imported material
		// ("<.gltf/.glb>#material/<n>"). Unresolved UUID keys are not material keys (callers that keep them check those).
		bool IsMaterialKey(std::string_view key);

		// A key a material's map can load: an image file (AssetManager::IsTextureFile) or a model's embedded image
		// ("<.gltf/.glb>#image/<n>").
		bool IsTextureKey(std::string_view key);

		// The problem with a material's values (the field and value), or nullopt when the renderer can use them: the glTF
		// importer's rules (which also applies this), so a material is valid as a file exactly when it is as a model's:
		// finite numbers, clearcoat, clearcoat roughness, transmission and occlusion strength in [0, 1], a finite
		// nonnegative emissive intensity, an alpha mode, and texture coordinate sets 0 or 1. The Material component's
		// registry rules are these, so an inline material the registry accepts always passes.
		std::optional<std::string> Validate(const MaterialData& material);

		// Parses a material file's JSON. Missing keys keep MaterialData's defaults; "Version" is required. UUID references
		// decode through `registry` (the active project's when null; without either they fail). References that no
		// longer resolve stay as opaque keys (rendered without that texture) and are reported in outWarnings. Name stays
		// empty (AssetManager names materials by file).
		std::optional<MaterialData> FromJson(const nlohmann::json& data, std::string& outError, std::vector<std::string>* outWarnings = nullptr, const AssetRegistry* registry = nullptr);
		// The same from a file's text (a JSON syntax error fails with the parser's position).
		std::optional<MaterialData> FromText(std::string_view text, std::string& outError, std::vector<std::string>* outWarnings = nullptr, const AssetRegistry* registry = nullptr);

		// Writes every field (floats in their shortest exact form), plus TextureTransforms entries that differ from the
		// identity on UV0. With encodeReferences, textures that exist under Assets/ become {UUID, Path} objects (creating
		// metadata), as scene saves do; that needs an active project and fails with the problem otherwise.
		std::optional<nlohmann::json> ToJson(const MaterialData& material, bool encodeReferences, std::string& outError);

		// Converts the path-string texture references of a material file's JSON to {UUID, Path} objects; only references
		// to onlyPath (and its sub-assets) when given. Move commands migrate material files with this. Needs an active
		// project (no-op without one).
		bool EncodeReferences(nlohmann::json& data, std::string& outError, const std::string& onlyPath = {});

		// The inline MaterialComponent override in the full model: its single UV transform applies to every map on UV0,
		// strengths are 1, it is single-sided, and alpha below 0.999 blends (alpha is coverage). It ignores the
		// component's Material field (callers resolve that first).
		MaterialData FromComponent(const MaterialComponent& component);
		// The same without the nine texture paths (maps left empty): for the renderer, which binds the component's own
		// maps (GetMaps) every frame, and for validation, which reads only the values.
		MaterialData ScalarsFromComponent(const MaterialComponent& component);

		// Pointers to every slot's texture key, in slot order (empty strings included), into the argument: it must outlive
		// them (temporaries are rejected).
		std::array<const std::string*, MaterialTextureSlotCount> GetMaps(const MaterialData& material);
		std::array<const std::string*, MaterialTextureSlotCount> GetMaps(const MaterialComponent& component);
		std::array<const std::string*, MaterialTextureSlotCount> GetMaps(MaterialData&&) = delete;
		std::array<const std::string*, MaterialTextureSlotCount> GetMaps(MaterialComponent&&) = delete;
		// The non-empty texture keys of a material, in slot order.
		std::vector<std::string> GetTextures(const MaterialData& material);

		// A file name (without extension) for a material named `name`: characters outside [A-Za-z0-9 ._-] replaced, no
		// leading or trailing dots and spaces (Windows drops trailing ones; a leading dot would hide the file), Windows
		// device names (CON, NUL, COM1, ...) suffixed, at most 64 characters. Empty when nothing usable remains.
		std::string MakeFileName(std::string_view name);

	}

}
