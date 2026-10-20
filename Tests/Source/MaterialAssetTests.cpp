#include <doctest/doctest.h>
#include <spdlog/fmt/fmt.h>

#include <Basalt/Asset/AssetKey.h>
#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Asset/AssetRegistry.h>
#include <Basalt/Asset/ImageUtils.h>
#include <Basalt/Asset/MaterialAsset.h>
#include <Basalt/Asset/Thumbnails.h>
#include <Basalt/Automation/CommandRegistry.h>
#include <Basalt/Core/JobSystem.h>
#include <Basalt/Core/JsonUtils.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Project/ProjectValidator.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/SceneSerializer.h>

#include "AnimationTestUtils.h"
#include "AutomationTestUtils.h"
#include "MaterialAssetDraft.h"
#include "MeshTestUtils.h"
#include "TestUtils.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <set>

using namespace Basalt;
using json = nlohmann::json;
using AnimationTest::ClearAssetCache;
using BasaltTest::Contains;
using BasaltTest::Run;
using BasaltTest::RunError;
using BasaltTest::TempProject;

namespace {

	// Every field away from its default, so a dropped field fails a round trip.
	MaterialData FullMaterial()
	{
		MaterialData m;
		m.AlbedoColor = { 0.25f, 0.5f, 0.75f, 0.5f };
		m.Metallic = 0.75f;
		m.Roughness = 0.125f;
		m.EmissiveColor = { 0.5f, 0.25f, 0.0f };
		m.EmissiveIntensity = 4.0f;
		m.Alpha = AlphaMode::Mask;
		m.AlphaCutoff = 0.375f;
		m.DoubleSided = true;
		m.NormalScale = 0.5f;
		m.OcclusionStrength = 0.25f;
		m.Clearcoat = 0.5f;
		m.ClearcoatRoughness = 0.25f;
		m.ClearcoatNormalScale = 2.0f;
		m.Transmission = 0.125f;
		for (size_t slot = 0; slot < MaterialTextureSlotCount; slot++)
		{
			m.*MaterialAsset::Slots[slot].Map = "Assets/Textures/" + std::string(MaterialAsset::Slots[slot].Name) + ".png";
			m.TextureTransforms[slot] = { { 0.125f * static_cast<float>(slot), 0.25f }, { 2.0f, 0.5f }, 0.5f, static_cast<uint32_t>(slot % 2) };
		}
		return m;
	}

	void CheckSame(const MaterialData& a, const MaterialData& b)
	{
		CHECK(a.AlbedoColor == b.AlbedoColor);
		CHECK(a.Metallic == b.Metallic);
		CHECK(a.Roughness == b.Roughness);
		CHECK(a.EmissiveColor == b.EmissiveColor);
		CHECK(a.EmissiveIntensity == b.EmissiveIntensity);
		CHECK(a.Alpha == b.Alpha);
		CHECK(a.AlphaCutoff == b.AlphaCutoff);
		CHECK(a.DoubleSided == b.DoubleSided);
		CHECK(a.NormalScale == b.NormalScale);
		CHECK(a.OcclusionStrength == b.OcclusionStrength);
		CHECK(a.Clearcoat == b.Clearcoat);
		CHECK(a.ClearcoatRoughness == b.ClearcoatRoughness);
		CHECK(a.ClearcoatNormalScale == b.ClearcoatNormalScale);
		CHECK(a.Transmission == b.Transmission);
		for (size_t slot = 0; slot < MaterialTextureSlotCount; slot++)
		{
			INFO(MaterialAsset::Slots[slot].Name);
			CHECK(a.*MaterialAsset::Slots[slot].Map == b.*MaterialAsset::Slots[slot].Map);
			CHECK(a.TextureTransforms[slot].Offset == b.TextureTransforms[slot].Offset);
			CHECK(a.TextureTransforms[slot].Scale == b.TextureTransforms[slot].Scale);
			CHECK(a.TextureTransforms[slot].Rotation == b.TextureTransforms[slot].Rotation);
			CHECK(a.TextureTransforms[slot].TexCoord == b.TextureTransforms[slot].TexCoord);
		}
	}

	json FileJson(const MaterialData& material)
	{
		std::string error;
		auto data = MaterialAsset::ToJson(material, false, error);
		REQUIRE_MESSAGE(data, error);
		return *data;
	}

	void WritePixel(const std::string& path)
	{
		std::string error;
		REQUIRE_MESSAGE(WritePng(Project::ResolvePath(path), { 255, 0, 0, 255 }, 1, 1, error), error);
	}

	// A glTF with two named materials (one with a transformed UV1 map) and the default material the importer adds.
	std::string WriteMaterialModel(const TempProject& project)
	{
		const auto root = project.GetDirectory() / "Assets" / "Models";
		std::filesystem::create_directories(root);
		BasaltTest::WriteSphereGltf(root);
		WritePixel("Assets/Models/Paint.png");
		json data = ParseJson(*FileSystem::ReadTextFile(root / "Sphere.gltf"));
		data["extensionsUsed"] = { "KHR_texture_transform" };
		data["images"] = { { { "uri", "Paint.png" } } };
		data["textures"] = { { { "source", 0 } } };
		// The sphere's UVs serve as TEXCOORD_1 too.
		data["meshes"][0]["primitives"][0]["attributes"]["TEXCOORD_1"] = 3;
		data["materials"] = {
			{ { "name", "Red/Paint" }, { "alphaMode", "MASK" }, { "alphaCutoff", 0.25 }, { "doubleSided", true }, { "pbrMetallicRoughness", { { "baseColorTexture", { { "index", 0 }, { "texCoord", 1 }, { "extensions", { { "KHR_texture_transform", { { "offset", { 0.5, 0.25 } }, { "scale", { 2.0, 3.0 } }, { "rotation", 0.75 } } } } } } } } }, { "normalTexture", { { "index", 0 }, { "scale", 0.5 } } } },
			{ { "name", "red/paint" }, { "pbrMetallicRoughness", { { "metallicFactor", 0.25 } } } },
		};
		REQUIRE(FileSystem::WriteTextFile(root / "Sphere.gltf", data.dump()));
		return "Assets/Models/Sphere.gltf";
	}

}

TEST_SUITE("MaterialAsset")
{
	TEST_CASE("Material files round trip every field, default missing keys and omit identity transforms")
	{
		const MaterialData material = FullMaterial();
		const json data = FileJson(material);
		std::string error;
		const auto parsed = MaterialAsset::FromJson(data, error);
		REQUIRE_MESSAGE(parsed, error);
		CheckSame(*parsed, material);

		const auto defaults = MaterialAsset::FromJson({ { "Version", 1 } }, error);
		REQUIRE_MESSAGE(defaults, error);
		CheckSame(*defaults, MaterialData{});
		const json written = FileJson(MaterialData{});
		CHECK_FALSE(written.contains("TextureTransforms"));
		CHECK(written["AlphaMode"] == "Opaque");
		CHECK(written["AlbedoMap"] == "");
		// Floats are written in their shortest form that reads back exactly.
		MaterialData decimal;
		decimal.Roughness = 0.6f;
		decimal.AlbedoColor = { 0.1f, 0.2f, 0.3f, 1.0f };
		CHECK(FileJson(decimal)["Roughness"].dump() == "0.6");
		CHECK(FileJson(decimal)["AlbedoColor"].dump() == "[0.1,0.2,0.3,1]");
		const auto readBack = MaterialAsset::FromJson(FileJson(decimal), error);
		REQUIRE(readBack);
		CHECK(readBack->Roughness == 0.6f);
		decimal.NormalScale = std::numeric_limits<float>::max();
		decimal.ClearcoatNormalScale = -std::numeric_limits<float>::max();
		const auto extreme = MaterialAsset::FromJson(FileJson(decimal), error);
		REQUIRE_MESSAGE(extreme, error);
		CHECK(extreme->NormalScale == std::numeric_limits<float>::max());
		CHECK(extreme->ClearcoatNormalScale == -std::numeric_limits<float>::max());
		MaterialData one;
		one.TextureTransforms[static_cast<size_t>(MaterialTextureSlot::Emissive)].TexCoord = 1;
		CHECK(FileJson(one)["TextureTransforms"].size() == 1);
		CHECK(FileJson(one)["TextureTransforms"].contains("Emissive"));
	}

	TEST_CASE("Each map key of the file format sets exactly its own slot")
	{
		// Literal keys: a swapped name in the slot table would still round trip, but change the format.
		const std::pair<const char*, MaterialTextureSlot> keys[] = { { "AlbedoMap", MaterialTextureSlot::Albedo }, { "NormalMap", MaterialTextureSlot::Normal }, { "MetallicRoughnessMap", MaterialTextureSlot::MetallicRoughness }, { "OcclusionMap", MaterialTextureSlot::Occlusion }, { "EmissiveMap", MaterialTextureSlot::Emissive }, { "ClearcoatMap", MaterialTextureSlot::Clearcoat }, { "ClearcoatRoughnessMap", MaterialTextureSlot::ClearcoatRoughness }, { "ClearcoatNormalMap", MaterialTextureSlot::ClearcoatNormal }, { "TransmissionMap", MaterialTextureSlot::Transmission } };
		const std::pair<const char*, MaterialTextureSlot> transforms[] = { { "Albedo", MaterialTextureSlot::Albedo }, { "Normal", MaterialTextureSlot::Normal }, { "MetallicRoughness", MaterialTextureSlot::MetallicRoughness }, { "Occlusion", MaterialTextureSlot::Occlusion }, { "Emissive", MaterialTextureSlot::Emissive }, { "Clearcoat", MaterialTextureSlot::Clearcoat }, { "ClearcoatRoughness", MaterialTextureSlot::ClearcoatRoughness }, { "ClearcoatNormal", MaterialTextureSlot::ClearcoatNormal }, { "Transmission", MaterialTextureSlot::Transmission } };
		for (const auto& [key, slot] : keys)
		{
			INFO(key);
			std::string error;
			const auto material = MaterialAsset::FromJson({ { "Version", 1 }, { key, "x" } }, error);
			REQUIRE_MESSAGE(material, error);
			for (size_t other = 0; other < MaterialTextureSlotCount; other++)
				CHECK((*material).*MaterialAsset::Slots[other].Map == (static_cast<MaterialTextureSlot>(other) == slot ? "x" : ""));
		}
		for (const auto& [key, slot] : transforms)
		{
			INFO(key);
			std::string error;
			const auto material = MaterialAsset::FromJson({ { "Version", 1 }, { "TextureTransforms", { { key, { { "TexCoord", 1 } } } } } }, error);
			REQUIRE_MESSAGE(material, error);
			for (size_t other = 0; other < MaterialTextureSlotCount; other++)
				CHECK(material->TextureTransforms[other].TexCoord == (static_cast<MaterialTextureSlot>(other) == slot ? 1u : 0u));
		}
	}

	TEST_CASE("Material file names are safe on every platform")
	{
		CHECK(MaterialAsset::MakeFileName("Red/Paint") == "Red_Paint");
		CHECK(MaterialAsset::MakeFileName("  .hidden. ") == "hidden");
		CHECK(MaterialAsset::MakeFileName("...").empty());
		CHECK(MaterialAsset::MakeFileName("").empty());
		CHECK(MaterialAsset::MakeFileName(std::string(100, 'a')).size() == 64);
		CHECK(MaterialAsset::MakeFileName("Ölfarbe") == "__lfarbe");
		CHECK(MaterialAsset::MakeFileName("CON") == "CON_");
		// Windows reserves a device name with any extension: the suffix goes on the part before the first dot.
		CHECK(MaterialAsset::MakeFileName("nul.001") == "nul_.001");
		CHECK(MaterialAsset::MakeFileName("Con.Metal") == "Con_.Metal");
		const std::string longReserved = MaterialAsset::MakeFileName("CON." + std::string(70, 'a'));
		CHECK(longReserved.size() == 64);
		CHECK(longReserved.starts_with("CON_."));
		for (const char* name : { "aux", "LPT9.x.y", "prn.", "Nul" })
		{
			INFO(name);
			const std::string safe = MaterialAsset::MakeFileName(name);
			const std::string stem = FileSystem::ToLowerAscii(safe.substr(0, safe.find('.')));
			CHECK(std::string_view(stem).ends_with('_'));
		}
		CHECK(MaterialAsset::MakeFileName("Console") == "Console");
		CHECK(MaterialAsset::MakeFileName("com1") == "com1_");
	}

	TEST_CASE("Material files reject malformed values with the field's location")
	{
		struct Case
		{
			const char* Name;
			json Patch;
			const char* Error;
		};
		const Case cases[] = {
			{ "missing version", json::object(), "Version" },
			{ "future version", { { "Version", 2 } }, "unsupported version 2" },
			{ "unknown key", { { "Colour", 1 } }, "unknown key 'Colour'" },
			{ "string number", { { "Metallic", "1" } }, "Metallic: expected a finite float" },
			{ "huge number", { { "Roughness", 1e300 } }, "Roughness: expected a finite float" },
			{ "short color", { { "AlbedoColor", { 1, 1, 1 } } }, "AlbedoColor: expected an array of 4 numbers" },
			{ "alpha mode", { { "AlphaMode", "MASK" } }, "AlphaMode: expected" },
			{ "double sided", { { "DoubleSided", 1 } }, "DoubleSided: expected a boolean" },
			{ "clearcoat range", { { "Clearcoat", 1.5 } }, "Clearcoat 1.5 must be in [0, 1]" },
			{ "occlusion range", { { "OcclusionStrength", -0.5 } }, "OcclusionStrength -0.5 must be in [0, 1]" },
			{ "emissive intensity", { { "EmissiveIntensity", -1 } }, "EmissiveIntensity -1 must be finite and nonnegative" },
			{ "map type", { { "NormalMap", 3 } }, "NormalMap: asset reference must be" },
			{ "transforms type", { { "TextureTransforms", json::array() } }, "TextureTransforms: expected an object" },
			{ "unknown slot", { { "TextureTransforms", { { "Diffuse", json::object() } } } }, "unknown slot 'Diffuse'" },
			{ "unknown transform key", { { "TextureTransforms", { { "Normal", { { "Angle", 1 } } } } } }, "TextureTransforms.Normal: unknown key 'Angle'" },
			{ "texcoord", { { "TextureTransforms", { { "Albedo", { { "TexCoord", 2 } } } } } }, "TextureTransforms.Albedo.TexCoord: expected 0 or 1" },
			{ "negative texcoord", { { "TextureTransforms", { { "Albedo", { { "TexCoord", -1 } } } } } }, "TexCoord: expected 0 or 1" },
			{ "offset", { { "TextureTransforms", { { "Emissive", { { "Offset", { 1, "x" } } } } } } }, "TextureTransforms.Emissive.Offset: expected an array of 2 finite floats" },
			{ "UUID without project", { { "AlbedoMap", { { "UUID", "12" }, { "Path", "Assets/a.png" } } } }, "AlbedoMap: UUID asset references require an active project" },
		};
		for (const auto& c : cases)
		{
			INFO(c.Name);
			json data = c.Patch.is_object() && !c.Patch.contains("Version") && std::string(c.Name) != "missing version" ? json{ { "Version", 1 } } : json::object();
			data.update(c.Patch);
			std::string error;
			CHECK_FALSE(MaterialAsset::FromJson(data, error));
			CHECK_MESSAGE(error.find(c.Error) != std::string::npos, error);
		}
		std::string error;
		CHECK_FALSE(MaterialAsset::FromJson(json::array(), error));
		CHECK(error == "expected an object");
	}

	TEST_CASE("Validation matches the importer and accepts every registry-valid inline material")
	{
		MaterialData material;
		CHECK_FALSE(MaterialAsset::Validate(material));
		// Factors outside glTF ranges that the renderer still draws remain valid, as the importer and the registry allow them.
		material.Metallic = 2.0f;
		material.AlbedoColor = { 4.0f, 1.0f, 1.0f, 1.0f };
		CHECK_FALSE(MaterialAsset::Validate(material));
		material.Roughness = std::numeric_limits<float>::infinity();
		CHECK(MaterialAsset::Validate(material).value_or("").find("Roughness") != std::string::npos);
		material = {};
		material.TextureTransforms[3].Rotation = std::numeric_limits<float>::quiet_NaN();
		CHECK(MaterialAsset::Validate(material).value_or("").find("TextureTransforms.Occlusion") != std::string::npos);
		material = {};
		material.Alpha = static_cast<AlphaMode>(7);
		CHECK(MaterialAsset::Validate(material));
		std::string error;
		CHECK_FALSE(MaterialAsset::ToJson(material, false, error));
		CHECK(error.find("AlphaMode") != std::string::npos);

		// An inline component the registry accepts always converts to a valid material.
		Scene scene;
		Entity entity = scene.CreateEntity("Inline");
		REQUIRE(ComponentRegistry::AddOrPatch(entity, "Material", { { "Metallic", 3 }, { "AlbedoColor", { 2, 2, 2, 0.5 } }, { "UVRotation", 1.5 }, { "Clearcoat", 1 } }, error));
		const MaterialData inlineMaterial = MaterialAsset::FromComponent(entity.GetComponent<MaterialComponent>());
		CHECK_FALSE(MaterialAsset::Validate(inlineMaterial));
	}

	TEST_CASE("An inline Material component converts to the equivalent full material")
	{
		MaterialComponent component;
		component.AlbedoColor = { 0.5f, 0.25f, 1.0f, 1.0f };
		component.Metallic = 0.5f;
		component.EmissiveIntensity = 2.0f;
		component.AlbedoMap = "Assets/a.png";
		component.TransmissionMap = "Assets/t.png";
		component.Transmission = 0.5f;
		component.UVOffset = { 0.25f, 0.5f };
		component.UVScale = { 2.0f, 4.0f };
		component.UVRotation = 0.5f;
		MaterialData material = MaterialAsset::FromComponent(component);
		CHECK(material.Alpha == AlphaMode::Opaque);
		CHECK_FALSE(material.DoubleSided);
		CHECK(material.NormalScale == 1.0f);
		CHECK(material.OcclusionStrength == 1.0f);
		CHECK(material.ClearcoatNormalScale == 1.0f);
		CHECK(material.AlbedoMap == "Assets/a.png");
		CHECK(material.TransmissionMap == "Assets/t.png");
		CHECK(material.IsBlended());
		for (const auto& transform : material.TextureTransforms)
		{
			CHECK(transform.Offset == component.UVOffset);
			CHECK(transform.Scale == component.UVScale);
			CHECK(transform.Rotation == component.UVRotation);
			CHECK(transform.TexCoord == 0);
		}
		// Alpha below one blends with alpha as coverage, exactly like the old override path.
		component.AlbedoColor.a = 0.998f;
		CHECK(MaterialAsset::FromComponent(component).Alpha == AlphaMode::Blend);
		component.AlbedoColor.a = 0.9995f;
		CHECK(MaterialAsset::FromComponent(component).Alpha == AlphaMode::Opaque);
	}

	TEST_CASE("Sub-asset keys split at the first known marker only")
	{
		CHECK(GetAssetSource("Assets/a.glb#image/3") == "Assets/a.glb");
		CHECK(GetAssetSource("Assets/a.glb#material/12") == "Assets/a.glb");
		CHECK(GetAssetSource("Assets/a#b.png") == "Assets/a#b.png");
		CHECK(GetSubAssetFragment("Assets/a.glb#material/1") == "#material/1");
		CHECK(GetSubAssetFragment("Assets/a.png").empty());
		CHECK(ParseSubAssetIndex("Assets/a.glb#material/12", ImportedMaterialMarker) == 12u);
		CHECK_FALSE(ParseSubAssetIndex("Assets/a.glb#material/12", EmbeddedImageMarker));
		for (const char* bad : { "Assets/a.glb#material/", "Assets/a.glb#material/x", "Assets/a.glb#material/1x", "Assets/a.glb#material/-1", "Assets/a.glb#material/99999999999", "Assets/a.glb" })
		{
			INFO(bad);
			CHECK_FALSE(ParseSubAssetIndex(bad, ImportedMaterialMarker));
		}
		CHECK(MakeSubAssetKey("Assets/a.glb", ImportedMaterialMarker, 3) == "Assets/a.glb#material/3");
		// Material and texture keys name the right kind of file.
		CHECK(MaterialAsset::IsMaterialKey("Assets/a.bmaterial"));
		CHECK(MaterialAsset::IsMaterialKey("Assets/a.GLTF#material/0"));
		for (const char* bad : { "Assets/a.png#material/0", "Assets/a.bmaterial#material/0", "Assets/a.glb", "Assets/a.glb#image/0", "Assets/a.bmaterial#image/0", "" })
		{
			INFO(bad);
			CHECK_FALSE(MaterialAsset::IsMaterialKey(bad));
		}
		CHECK(MaterialAsset::IsTextureKey("Assets/a.png"));
		CHECK(MaterialAsset::IsTextureKey("Assets/a.glb#image/2"));
		for (const char* bad : { "Assets/a.wav", "builtin://Cube", "Assets/a.glb", "Assets/a.png#image/0", "Assets/a.glb#material/0" })
		{
			INFO(bad);
			CHECK_FALSE(MaterialAsset::IsTextureKey(bad));
		}
	}

	TEST_CASE("Material references use UUIDs, survive moves and keep sub-asset fragments")
	{
		ClearAssetCache clear;
		TempProject project("MaterialIdentity");
		CommandRegistry registry;
		AutomationSession session;
		WritePixel("Assets/Textures/Rock.png");
		const std::string model = WriteMaterialModel(project);
		MaterialData material;
		material.AlbedoMap = "Assets/Textures/Rock.png";
		material.EmissiveMap = model.substr(0, model.size()) + "#image/0";
		std::string error;
		const auto encoded = MaterialAsset::ToJson(material, true, error);
		REQUIRE_MESSAGE(encoded, error);
		REQUIRE((*encoded)["AlbedoMap"].is_object());
		CHECK((*encoded)["AlbedoMap"]["Path"] == "Assets/Textures/Rock.png");
		CHECK((*encoded)["EmissiveMap"]["Path"] == model + "#image/0");
		CHECK((*encoded)["NormalMap"] == "");
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Materials/Rock.bmaterial"), encoded->dump()));
		// A legacy path-only file migrates on move like scenes and prefabs do.
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Materials/Legacy.bmaterial"), json{ { "Version", 1 }, { "AlbedoMap", "Assets/Textures/Rock.png" }, { "NormalMap", "Assets/Textures/Other.png" } }.dump()));

		// Loaded before the moves: the move commands drop what they invalidate.
		REQUIRE(AssetManager::GetMaterial("Assets/Materials/Rock.bmaterial"));
		std::vector<std::pair<std::string, std::string>> notified;
		session.OnAssetMoved = [&](const std::string& from, const std::string& to) { notified.emplace_back(from, to); };
		Run(registry, session, "asset.move", { { "from", "Assets/Textures/Rock.png" }, { "to", "Assets/Textures/Moved/Rock.png" } });
		Run(registry, session, "asset.move", { { "from", model }, { "to", "Assets/Models/Moved.gltf" } });
		CHECK(notified == std::vector<std::pair<std::string, std::string>>{ { "Assets/Textures/Rock.png", "Assets/Textures/Moved/Rock.png" }, { model, "Assets/Models/Moved.gltf" } });
		const auto moved = AssetManager::GetMaterial("Assets/Materials/Rock.bmaterial");
		REQUIRE(moved);
		CHECK(moved->AlbedoMap == "Assets/Textures/Moved/Rock.png");
		CHECK(moved->EmissiveMap == "Assets/Models/Moved.gltf#image/0");
		CHECK(moved->Name == "Rock");
		const json legacy = ParseJson(*FileSystem::ReadTextFile(Project::ResolvePath("Assets/Materials/Legacy.bmaterial")));
		REQUIRE(legacy["AlbedoMap"].is_object());
		// Only references to the moved file are rewritten.
		CHECK(legacy["NormalMap"] == "Assets/Textures/Other.png");
		const auto legacyMaterial = AssetManager::GetMaterial("Assets/Materials/Legacy.bmaterial");
		REQUIRE(legacyMaterial);
		CHECK(legacyMaterial->AlbedoMap == "Assets/Textures/Moved/Rock.png");

		// A texture whose identity goes away outside the session: the cached material drops with it (its decoded path is
		// stale), renders without it and says so once; when it comes back, the next change resolves it again.
		const std::string texture = "Assets/Textures/Moved/Rock.png";
		const std::string meta = *FileSystem::ReadTextFile(Project::ResolvePath(texture + ".meta"));
		std::filesystem::remove(Project::ResolvePath(texture));
		std::filesystem::remove(Project::ResolvePath(texture + ".meta"));
		REQUIRE(Project::GetActive()->GetAssets().Refresh(error));
		const auto dropped = AssetManager::ReloadChanged({ texture });
		CHECK(std::find(dropped.begin(), dropped.end(), "Assets/Materials/Rock.bmaterial") != dropped.end());
		const auto since = Log::GetHistory().GetTotalCount();
		const auto unresolved = AssetManager::GetMaterial("Assets/Materials/Rock.bmaterial");
		REQUIRE(unresolved);
		CHECK(AssetRegistry::IsUnresolvedKey(unresolved->AlbedoMap));
		CHECK(BasaltTest::CountLogMessages(since, "unresolved asset UUID") == 1);
		CHECK(BasaltTest::CountLogMessages(since, "(hint: Assets/Textures/Rock.png)") == 1);
		const auto report = ValidateProject(*Project::GetActive());
		CHECK(std::any_of(report.GetIssues().begin(), report.GetIssues().end(), [](const ValidationIssue& issue) { return issue.File == "Assets/Materials/Rock.bmaterial" && issue.Message.find("unresolved asset UUID") != std::string::npos; }));
		WritePixel(texture);
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath(texture + ".meta"), meta));
		REQUIRE(Project::GetActive()->GetAssets().Refresh(error));
		AssetManager::ReloadChanged({ texture + ".meta" });
		const auto restored = AssetManager::GetMaterial("Assets/Materials/Rock.bmaterial");
		REQUIRE(restored);
		CHECK(restored->AlbedoMap == texture);
	}

	TEST_CASE("A failed material-file migration restores every document")
	{
		ClearAssetCache clear;
		TempProject project("MaterialMoveRestore");
		CommandRegistry registry;
		AutomationSession session;
		WritePixel("Assets/Rock.png");
		const json legacy = { { "Version", 1 }, { "AlbedoMap", "Assets/Rock.png" } };
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/A.bmaterial"), legacy.dump()));
		// The destination's folder is a file, so the final rename fails after the material was rewritten.
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Blocked"), "x"));
		CHECK_FALSE(RunError(registry, session, "asset.move", { { "from", "Assets/Rock.png" }, { "to", "Assets/Blocked/Rock.png" } }).empty());
		CHECK(ParseJson(*FileSystem::ReadTextFile(Project::ResolvePath("Assets/A.bmaterial"))) == legacy);
		CHECK(FileSystem::Exists(Project::ResolvePath("Assets/Rock.png")));
	}

	TEST_CASE("AssetManager caches material files and resolves imported materials by index")
	{
		ClearAssetCache clear;
		TempProject project("MaterialCache");
		const std::string model = WriteMaterialModel(project);
		const std::string path = "Assets/M.bmaterial";
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath(path), FileJson(FullMaterial()).dump()));
		const auto first = AssetManager::GetMaterial(path);
		REQUIRE(first);
		CHECK(AssetManager::GetMaterial(path) == first);
		CHECK(AssetManager::RequestMaterial(path) == first);
		CheckSame(*first, FullMaterial());

		// A change on disk is picked up after ReloadChanged; a broken file fails closed, once, then recovers.
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath(path), R"({"Version": 1, "Clearcoat": 9})"));
		CHECK(AssetManager::GetMaterial(path) == first);
		CHECK(AssetManager::ReloadChanged({ path }) == std::vector<std::string>{ path });
		const auto since = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(AssetManager::GetMaterial(path));
		CHECK_FALSE(AssetManager::GetMaterial(path));
		CHECK(BasaltTest::CountLogMessages(since, "Clearcoat 9 must be in [0, 1]") == 1);
		CHECK(AssetManager::GetError(path).find("Clearcoat") != std::string::npos);
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath(path), R"({"Version": 1, "Metallic": 1})"));
		AssetManager::ReloadChanged({ path });
		REQUIRE(AssetManager::GetMaterial(path));
		const auto recovered = AssetManager::GetMaterial(path);
		REQUIRE(recovered);
		CHECK(recovered->Metallic == 1.0f);

		// Imported materials alias their model and keep everything the importer read.
		const auto imported = AssetManager::GetMaterial(model + "#material/0");
		REQUIRE(imported);
		CHECK(imported->Name == "Red/Paint");
		CHECK(imported->Alpha == AlphaMode::Mask);
		CHECK(imported->AlphaCutoff == doctest::Approx(0.25f));
		CHECK(imported->DoubleSided);
		CHECK(imported->NormalScale == doctest::Approx(0.5f));
		CHECK(imported->TextureTransforms[0].TexCoord == 1);
		CHECK(imported->TextureTransforms[0].Rotation == doctest::Approx(0.75f));
		CHECK(imported->AlbedoMap == "Assets/Models/Paint.png");
		const auto second = AssetManager::GetMaterial(model + "#material/1");
		REQUIRE(second);
		CHECK(second->Metallic == doctest::Approx(0.25f));
		// The importer's default material for primitives without one is not addressable.
		CHECK_FALSE(AssetManager::GetMaterial(model + "#material/2"));
		CHECK(AssetManager::GetError(model + "#material/2").find("has no material 2 (it has 2)") != std::string::npos);
		// The largest index must not wrap the bounds check around.
		CHECK_FALSE(AssetManager::GetMaterial(model + "#material/4294967295"));
		CHECK(AssetManager::GetError(model + "#material/4294967295").find("has no material 4294967295") != std::string::npos);
		CHECK_FALSE(AssetManager::GetMaterial(model + "#material/x"));
		CHECK(AssetManager::GetError(model + "#material/x").find("is not a material") != std::string::npos);
		CHECK_FALSE(AssetManager::GetMaterial("builtin://Sphere#material/0"));
		CHECK(AssetManager::GetError("builtin://Sphere#material/0").find("is not a material") != std::string::npos);
		CHECK_FALSE(AssetManager::GetMaterial("asset-uuid://42/416e"));
		CHECK(AssetManager::GetError("asset-uuid://42/416e") == "unresolved asset UUID 42 (hint: An)");
		// Keys whose source is not a model never try to load one.
		CHECK_FALSE(AssetManager::GetMaterial("Assets/Models/Paint.png#material/0"));
		CHECK(AssetManager::GetError("Assets/Models/Paint.png#material/0").find("is not a material") != std::string::npos);
		CHECK_FALSE(AssetManager::GetMaterial("Assets/Models/Paint.png"));
		CHECK(AssetManager::GetError("Assets/Models/Paint.png").find("is not a material") != std::string::npos);

		// A change to a file the model reads drops its imported materials with it.
		REQUIRE(AssetManager::GetMaterial(model + "#material/0"));
		const auto byBuffer = AssetManager::ReloadChanged({ "Assets/Models/Sphere.bin" });
		CHECK(std::find(byBuffer.begin(), byBuffer.end(), model + "#material/0") != byBuffer.end());
		REQUIRE(AssetManager::GetMaterial(model + "#material/0"));
		CHECK_FALSE(AssetManager::GetMaterial(model + "#material/2"));
		// So does a change to the model itself.
		auto keys = AssetManager::ReloadChanged({ model });
		CHECK(std::find(keys.begin(), keys.end(), model + "#material/0") != keys.end());
		CHECK(std::find(keys.begin(), keys.end(), model + "#material/2") != keys.end());
		const auto loaded = AssetManager::GetLoadedKeys();
		CHECK(std::none_of(loaded.begin(), loaded.end(), [&](const std::string& key) { return key.starts_with(model + "#material/"); }));
		// Reloading the model itself drops them too.
		REQUIRE(AssetManager::GetMaterial(model + "#material/0"));
		AssetManager::Reload(model);
		const auto reloaded = AssetManager::GetLoadedKeys();
		CHECK(std::none_of(reloaded.begin(), reloaded.end(), [&](const std::string& key) { return key.starts_with(model + "#material/"); }));
		// A material handed out before stays valid: it shares its model's lifetime.
		CHECK(imported->Name == "Red/Paint");
		CHECK(imported->Alpha == AlphaMode::Mask);
		{
			AssetManager::TemporaryLoads temporary;
			REQUIRE(AssetManager::GetMaterial(model + "#material/1"));
		}
		const auto after = AssetManager::GetLoadedKeys();
		CHECK(std::find(after.begin(), after.end(), model + "#material/1") == after.end());
		CHECK(std::find(after.begin(), after.end(), path) != after.end());
	}

	TEST_CASE("The importer rejects material values a material file could not hold")
	{
		ClearAssetCache clear;
		TempProject project("MaterialImportValidation");
		const std::string model = WriteMaterialModel(project);
		const auto path = Project::ResolvePath(model);
		json data = ParseJson(*FileSystem::ReadTextFile(path));
		// Beyond float range: cgltf reads it as infinity.
		data["materials"][1]["pbrMetallicRoughness"]["roughnessFactor"] = 1e39;
		REQUIRE(FileSystem::WriteTextFile(path, data.dump()));
		CHECK_FALSE(AssetManager::GetMesh(model));
		CHECK(AssetManager::GetError(model).find("material 'red/paint': Roughness inf must be finite") != std::string::npos);
	}

	TEST_CASE("Material component references must name a material")
	{
		TempProject project("MaterialField");
		Scene scene;
		Entity entity = scene.CreateEntity("E");
		std::string error;
		for (const char* valid : { "", "Assets/M.bmaterial", "Assets/M.BMATERIAL", "Assets/Ship.glb#material/0", "asset-uuid://7/416e" })
		{
			INFO(valid);
			CHECK_MESSAGE(ComponentRegistry::AddOrPatch(entity, "Material", { { "Material", valid } }, error), error);
		}
		for (const char* invalid : { "Assets/a.png", "Assets/Ship.glb", "Assets/Ship.glb#image/0", "Assets/Ship.glb#material/x", "Assets/a.png#material/0", "Assets/M.bmaterial#material/0" })
		{
			INFO(invalid);
			CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Material", { { "Material", invalid } }, error));
			CHECK(error.find("must be a .bmaterial file or <.gltf/.glb model>#material/<n>") != std::string::npos);
		}
		CHECK(entity.GetComponent<MaterialComponent>().Material == "asset-uuid://7/416e");
		// The numeric rules are the material file's, so the inline values are always a valid material.
		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Material", { { "Clearcoat", 1.5 } }, error));
		CHECK(error.find("Clearcoat 1.5 must be in [0, 1]") != std::string::npos);
		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Material", { { "EmissiveIntensity", -1 } }, error));
		CHECK(error.find("EmissiveIntensity -1 must be finite and nonnegative") != std::string::npos);
	}

	TEST_CASE("Material commands write validated files from defaults, entities and models")
	{
		ClearAssetCache clear;
		TempProject project("MaterialCommands");
		CommandRegistry registry;
		AutomationSession session;
		WritePixel("Assets/Textures/Rock.png");
		const std::string model = WriteMaterialModel(project);

		// asset.write validates and keeps the old content on failure; missing textures are warnings.
		const std::string path = "Assets/Materials/Written.bmaterial";
		const std::string valid = json{ { "Version", 1 }, { "AlbedoMap", "Assets/Textures/Missing.png" } }.dump();
		const auto written = Run(registry, session, "asset.write", { { "path", path }, { "content", valid } });
		REQUIRE(written.contains("warnings"));
		CHECK(written["warnings"][0].get<std::string>().find("AlbedoMap: 'Assets/Textures/Missing.png' does not exist") != std::string::npos);
		CHECK(RunError(registry, session, "asset.write", { { "path", path }, { "content", R"({"Version": 1, "Transmission": 2})" } }).find("invalid material: Transmission 2 must be in [0, 1]") != std::string::npos);
		CHECK(RunError(registry, session, "asset.write", { { "path", path }, { "content", "{" } }).find("invalid material") != std::string::npos);
		CHECK(Run(registry, session, "asset.read", { { "path", path } })["content"] == valid);
		// Wrong kinds of maps, letter case and unresolved UUIDs are reported too.
		std::filesystem::create_directories(Project::ResolvePath("Assets/Audio"));
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Audio/Beep.wav"), "RIFF"));
		const json problems = { { "Version", 1 }, { "NormalMap", "Assets/Audio/Beep.wav" }, { "EmissiveMap", "Assets/Textures/rock.png" }, { "TransmissionMap", { { "UUID", "77" }, { "Path", "Assets/Textures/Gone.png" } } } };
		const auto warned = Run(registry, session, "asset.write", { { "path", "Assets/Materials/Problems.bmaterial" }, { "content", problems.dump() } });
		const std::string warnings = warned.value("warnings", json::array()).dump();
		CHECK_MESSAGE(Contains(warnings, "NormalMap: 'Assets/Audio/Beep.wav' is not a texture"), warnings);
		CHECK_MESSAGE(Contains(warnings, "EmissiveMap: 'Assets/Textures/rock.png', but the file on disk is 'Assets/Textures/Rock.png'"), warnings);
		CHECK_MESSAGE(Contains(warnings, "TransmissionMap: unresolved asset UUID 77 (hint: Assets/Textures/Gone.png)"), warnings);
		// Every folder's letter case counts, as do paths outside Assets/, backslashes and external "embedded" images.
		const json paths = { { "Version", 1 }, { "AlbedoMap", "Assets/textures/Rock.png" }, { "NormalMap", "Textures/Rock.png" }, { "MetallicRoughnessMap", "Assets\\Textures\\Rock.png" }, { "EmissiveMap", model + "#image/0" }, { "OcclusionMap", model + "#image/3" } };
		const std::string pathWarnings = Run(registry, session, "asset.write", { { "path", "Assets/Materials/Paths.bmaterial" }, { "content", paths.dump() } }).value("warnings", json::array()).dump();
		CHECK_MESSAGE(Contains(pathWarnings, "AlbedoMap: 'Assets/textures/Rock.png', but the file on disk is 'Assets/Textures/Rock.png'"), pathWarnings);
		CHECK_MESSAGE(Contains(pathWarnings, "NormalMap: 'Textures/Rock.png' is outside Assets/"), pathWarnings);
		CHECK_MESSAGE(Contains(pathWarnings, "MetallicRoughnessMap: 'Assets\\\\Textures\\\\Rock.png' uses backslashes"), pathWarnings);
		CHECK_MESSAGE(Contains(pathWarnings, "EmissiveMap: image 0 of '" + model + "' is the external file 'Paint.png'"), pathWarnings);
		CHECK_MESSAGE(Contains(pathWarnings, "OcclusionMap: '" + model + "' has 1 image(s)"), pathWarnings);
		// Writing a material that is already loaded replaces it everywhere it is used.
		REQUIRE(AssetManager::GetMaterial(path));
		Run(registry, session, "asset.write", { { "path", path }, { "content", R"({"Version": 1, "Metallic": 0.75})" } });
		const auto rewritten = AssetManager::GetMaterial(path);
		REQUIRE(rewritten);
		CHECK(rewritten->Metallic == 0.75f);

		// material.create: defaults, then from an entity's inline values or its asset.
		CHECK(Run(registry, session, "material.create", { { "path", "Assets/Materials/Default.bmaterial" } })["path"] == "Assets/Materials/Default.bmaterial");
		const auto defaults = AssetManager::GetMaterial("Assets/Materials/Default.bmaterial");
		REQUIRE(defaults);
		CheckSame(*defaults, MaterialData{});
		CHECK(RunError(registry, session, "material.create", { { "path", "Assets/Materials/Default.bmaterial" } }).find("already exists") != std::string::npos);
		CHECK(RunError(registry, session, "material.create", { { "path", "Assets/Materials/Wrong.json" } }).find(".bmaterial extension") != std::string::npos);
		CHECK(RunError(registry, session, "material.create", { { "path", "Materials/Outside.bmaterial" } }).find("must be under Assets/") != std::string::npos);
		Run(registry, session, "scene.new", { { "name", "Materials" }, { "empty", true } });
		Run(registry, session, "entity.create", { { "name", "Plain" } });
		CHECK(RunError(registry, session, "material.create", { { "path", "Assets/Materials/Plain.bmaterial" }, { "entity", "Plain" } }).find("has no Material component") != std::string::npos);
		Run(registry, session, "component.set", { { "entity", "Plain" }, { "component", "Material" }, { "data", { { "AlbedoColor", { 1, 0, 0, 0.5 } }, { "AlbedoMap", "Assets/Textures/Rock.png" }, { "UVScale", { 2, 2 } } } } });
		Run(registry, session, "material.create", { { "path", "Assets/Materials/Plain.bmaterial" }, { "entity", "Plain" } });
		const json plain = ParseJson(*FileSystem::ReadTextFile(Project::ResolvePath("Assets/Materials/Plain.bmaterial")));
		CHECK(plain["AlphaMode"] == "Blend");
		CHECK(plain["AlbedoMap"].is_object());
		CHECK(plain["TextureTransforms"].size() == MaterialTextureSlotCount);
		Run(registry, session, "component.set", { { "entity", "Plain" }, { "component", "Material" }, { "data", { { "Material", model + "#material/0" } } } });
		Run(registry, session, "material.create", { { "path", "Assets/Materials/FromAsset.bmaterial" }, { "entity", "Plain" } });
		const auto fromAsset = AssetManager::GetMaterial("Assets/Materials/FromAsset.bmaterial");
		REQUIRE(fromAsset);
		CHECK(fromAsset->Alpha == AlphaMode::Mask);
		// Overwriting replaces the cached copy as well.
		Run(registry, session, "component.set", { { "entity", "Plain" }, { "component", "Material" }, { "data", { { "Material", model + "#material/1" } } } });
		Run(registry, session, "material.create", { { "path", "Assets/Materials/FromAsset.bmaterial" }, { "entity", "Plain" }, { "overwrite", true } });
		const auto overwritten = AssetManager::GetMaterial("Assets/Materials/FromAsset.bmaterial");
		REQUIRE(overwritten);
		CHECK(overwritten->Alpha == AlphaMode::Opaque);
		CHECK(overwritten->Metallic == 0.25f);
		Run(registry, session, "component.set", { { "entity", "Plain" }, { "component", "Material" }, { "data", { { "Material", "Assets/Materials/Gone.bmaterial" } } } });
		CHECK(RunError(registry, session, "material.create", { { "path", "Assets/Materials/Gone2.bmaterial" }, { "entity", "Plain" } }).find("cannot read Material") != std::string::npos);

		// material.extract: one file per glTF material, names made safe and unique (case-insensitively).
		const auto extracted = Run(registry, session, "material.extract", { { "model", model } });
		REQUIRE(extracted["materials"].size() == 2);
		CHECK(extracted["materials"][0]["path"] == "Assets/Models/Materials/Red_Paint.bmaterial");
		CHECK(extracted["materials"][1]["path"] == "Assets/Models/Materials/red_paint_2.bmaterial");
		CHECK(extracted["materials"][0]["key"] == model + "#material/0");
		const auto first = AssetManager::GetMaterial("Assets/Models/Materials/Red_Paint.bmaterial");
		REQUIRE(first);
		CHECK(first->Alpha == AlphaMode::Mask);
		CHECK(first->TextureTransforms[0].TexCoord == 1);
		CHECK(first->TextureTransforms[0].Scale == glm::vec2(2.0f, 3.0f));
		CHECK(first->AlbedoMap == "Assets/Models/Paint.png");
		CHECK(RunError(registry, session, "material.extract", { { "model", model } }).find("already exists (pass overwrite=true to replace it); nothing was written") != std::string::npos);
		CHECK(Run(registry, session, "material.extract", { { "model", model }, { "folder", "Assets/Other" }, { "overwrite", true } })["materials"][1]["path"] == "Assets/Other/red_paint_2.bmaterial");
		CHECK(RunError(registry, session, "material.extract", { { "model", "Assets/Textures/Rock.png" } }).find("is not a glTF model") != std::string::npos);
		CHECK(RunError(registry, session, "material.extract", { { "model", model }, { "folder", "Outside" } }).find("must be under Assets/") != std::string::npos);
		// A write that fails part way says how many files were written (they are kept).
		std::filesystem::create_directories(Project::ResolvePath("Assets/Partial/red_paint_2.bmaterial"));
		const std::string partial = RunError(registry, session, "material.extract", { { "model", model }, { "folder", "Assets/Partial" }, { "overwrite", true } });
		CHECK_MESSAGE(Contains(partial, "(1 of 2 files were written before it)"), partial);
		CHECK(FileSystem::Exists(Project::ResolvePath("Assets/Partial/Red_Paint.bmaterial")));

		// asset.preload reads a material and requests its textures.
		const auto preload = Run(registry, session, "asset.preload", { { "paths", { "Assets/Materials/Plain.bmaterial", "Assets/Materials/Gone.bmaterial" } }, { "wait", true } });
		REQUIRE(preload.size() == 3);
		CHECK(preload[0]["status"] == "ready");
		CHECK(preload[1]["status"] == "failed");
		CHECK(Contains(preload[1]["error"].get<std::string>(), "cannot read material 'Assets/Materials/Gone.bmaterial'"));
		CHECK(preload[2]["path"] == "Assets/Textures/Rock.png");
		CHECK(preload[2]["status"] == "ready");
		// A texture whose UUID does not resolve is a failed entry, not silently skipped.
		const auto unresolved = Run(registry, session, "asset.preload", { { "paths", { "Assets/Materials/Problems.bmaterial" } }, { "wait", true } });
		const auto entry = std::find_if(unresolved.begin(), unresolved.end(), [](const json& e) { return e["path"].get<std::string>().starts_with("asset-uuid://77/"); });
		REQUIRE(entry != unresolved.end());
		CHECK((*entry)["status"] == "failed");
		CHECK((*entry)["error"] == "unresolved asset UUID 77 (hint: Assets/Textures/Gone.png)");
	}

	TEST_CASE("Project validation checks material files and their textures")
	{
		ClearAssetCache clear;
		TempProject project("MaterialValidation");
		WritePixel("Assets/Rock.png");
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Good.bmaterial"), json{ { "Version", 1 }, { "AlbedoMap", "Assets/Rock.png" } }.dump()));
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Missing.bmaterial"), json{ { "Version", 1 }, { "NormalMap", "Assets/missing.png" } }.dump()));
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Bad.bmaterial"), R"({"Version": 1, "Clearcoat": 3})"));
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Case.bmaterial"), json{ { "Version", 1 }, { "AlbedoMap", "Assets/rock.png" } }.dump()));
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Sound.wav"), "RIFF"));
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Kind.bmaterial"), json{ { "Version", 1 }, { "AlbedoMap", "Assets/Sound.wav" }, { "NormalMap", "builtin://Cube" } }.dump()));
		// Sub-assets must exist in their model: the sphere model has 2 materials, an external image 0 (a file of its own,
		// which "#image/0" cannot load) and here an embedded image 1.
		const std::string model = WriteMaterialModel(project);
		json gltf = ParseJson(*FileSystem::ReadTextFile(Project::ResolvePath(model)));
		gltf["images"].push_back({ { "uri", "data:image/png;base64,AAAA" } });
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath(model), gltf.dump()));
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Images.bmaterial"), json{ { "Version", 1 }, { "AlbedoMap", model + "#image/0" }, { "NormalMap", model + "#image/1" }, { "EmissiveMap", model + "#image/5" } }.dump()));
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Broken.glb"), "not a model"));
		Scene scene("SubAssets");
		Entity good = scene.CreateEntity("Good");
		good.AddComponent<MaterialComponent>().Material = model + "#material/1";
		Entity bad = scene.CreateEntity("Bad");
		bad.AddComponent<MaterialComponent>().Material = model + "#material/9";
		Entity broken = scene.CreateEntity("Broken");
		broken.AddComponent<MaterialComponent>().Material = "Assets/Broken.glb#material/0";
		// A component whose material file lost its identity still loads (the scene stays editable) and is reported.
		Entity lost = scene.CreateEntity("Lost");
		lost.AddComponent<MaterialComponent>().Material = "asset-uuid://999/4173736574732f476f6e652e626d6174657269616c";
		std::string error;
		REQUIRE_MESSAGE(SceneSerializer::SaveScene(scene, Project::ResolvePath("Assets/SubAssets.bscene"), error), error);
		const auto report = ValidateProject(*Project::GetActive());
		auto has = [&](const std::string& file, const std::string& text) {
			return std::any_of(report.GetIssues().begin(), report.GetIssues().end(), [&](const ValidationIssue& issue) { return issue.File == file && issue.Message.find(text) != std::string::npos; });
		};
		CHECK(has("Assets/Bad.bmaterial", "invalid material: Clearcoat 3 must be in [0, 1]"));
		CHECK(has("Assets/Missing.bmaterial", "NormalMap references the missing file 'Assets/missing.png'"));
		CHECK(has("Assets/Case.bmaterial", "letter case matters"));
		CHECK(has("Assets/Kind.bmaterial", "AlbedoMap references 'Assets/Sound.wav', which is not a texture"));
		CHECK(has("Assets/Kind.bmaterial", "NormalMap references 'builtin://Cube', which is not a texture"));
		CHECK(has("Assets/Images.bmaterial", fmt::format("EmissiveMap references '{0}#image/5', but '{0}' has 2 image(s)", model)));
		CHECK(has("Assets/Images.bmaterial", fmt::format("AlbedoMap references '{0}#image/0', but image 0 of '{0}' is the external file 'Paint.png'", model)));
		CHECK_FALSE(has("Assets/Images.bmaterial", "#image/1"));
		CHECK(has("Assets/SubAssets.bscene", "'Assets/Broken.glb#material/0', but 'Assets/Broken.glb' is not a readable glTF"));
		CHECK(has("Assets/SubAssets.bscene", "references an unresolved asset UUID 999 (hint: Assets/Gone.bmaterial)"));
		CHECK(has("Assets/SubAssets.bscene", "#material/9', but '" + model + "' has 2 material(s)"));
		CHECK_FALSE(has("Assets/SubAssets.bscene", "#material/1"));
		CHECK(std::none_of(report.GetIssues().begin(), report.GetIssues().end(), [](const ValidationIssue& issue) { return issue.File == "Assets/Good.bmaterial"; }));
	}

	TEST_CASE("Thumbnails of material files depend on their textures, and prefabs follow materials")
	{
		TempProject project("MaterialThumbnails");
		WritePixel("Assets/Rock.png");
		const std::string model = WriteMaterialModel(project);
		CHECK(ClassifyThumbnail("Assets/A.bmaterial") == ThumbnailKind::Material);
		CHECK(ClassifyThumbnail("Assets/A.BMATERIAL") == ThumbnailKind::Material);
		CHECK(IsRenderedThumbnail(ThumbnailKind::Material));
		CHECK(std::string(ThumbnailKindName(ThumbnailKind::Material)) == "Material");
		MaterialData material;
		material.AlbedoMap = "Assets/Rock.png";
		material.EmissiveMap = model + "#image/0";
		std::string error;
		const auto text = MaterialAsset::ToJson(material, true, error);
		REQUIRE_MESSAGE(text, error);
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/A.bmaterial"), text->dump()));
		const ThumbnailCache cache(project.GetDirectory());
		const auto dependencies = cache.GetDependencies("Assets/A.bmaterial");
		// UUID references also depend on their file's sidecar: a preview made while one did not resolve is remade when it does.
		CHECK(dependencies == std::vector<std::string>{ "Assets/Models/Paint.png", "Assets/Models/Sphere.bin", "Assets/Models/Sphere.gltf", "Assets/Models/Sphere.gltf.meta", "Assets/Rock.png", "Assets/Rock.png.meta" });
		const auto key = cache.MakeKey("Assets/A.bmaterial", error);
		REQUIRE(key);
		WritePixel("Assets/Rock.png");
		std::filesystem::last_write_time(Project::ResolvePath("Assets/Rock.png"), std::filesystem::file_time_type::clock::now() + std::chrono::hours(1));
		CHECK(cache.MakeKey("Assets/A.bmaterial", error) != key);

		// A prefab whose entity uses the material depends on its textures too.
		Scene scene;
		Entity entity = scene.CreateEntity("Prop");
		entity.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
		entity.AddComponent<MaterialComponent>().Material = "Assets/A.bmaterial";
		REQUIRE_MESSAGE(SceneSerializer::SavePrefab(entity, Project::ResolvePath("Assets/Prop.bprefab"), error), error);
		const auto prefab = cache.GetDependencies("Assets/Prop.bprefab");
		for (const char* file : { "Assets/A.bmaterial", "Assets/Rock.png", "Assets/Models/Sphere.gltf" })
		{
			INFO(file);
			CHECK(std::find(prefab.begin(), prefab.end(), file) != prefab.end());
		}

		// Editor previews of a component referencing the material follow the same files.
		MaterialComponent component;
		component.Material = "Assets/A.bmaterial";
		const auto tracked = MaterialPreviewDependencies(component);
		CHECK(std::find(tracked.begin(), tracked.end(), "Assets/Rock.png") != tracked.end());
		CHECK(std::find(tracked.begin(), tracked.end(), "Assets/A.bmaterial") != tracked.end());
		// A model's imported material follows the model's buffers and external images.
		component.Material = model + "#material/0";
		const auto imported = MaterialPreviewDependencies(component);
		CHECK(imported == std::vector<std::string>{ "Assets/Models/Paint.png", "Assets/Models/Sphere.bin", "Assets/Models/Sphere.gltf" });
		// So do by-value previews (tooltip swatches, drafts).
		MaterialData swatch;
		swatch.EmissiveMap = model + "#image/0";
		CHECK(MaterialPreviewDependencies(swatch) == imported);
		// A UUID that does not decode is still watched through its Path hint (and that file's sidecar).
		const json unknown = { { "Version", 1 }, { "AlbedoMap", { { "UUID", "123" }, { "Path", "Assets/Rock.png" } } } };
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Unknown.bmaterial"), unknown.dump()));
		const auto hinted = cache.GetDependencies("Assets/Unknown.bmaterial");
		CHECK(std::find(hinted.begin(), hinted.end(), "Assets/Rock.png") != hinted.end());
	}

	TEST_CASE("The material editor's draft tracks edits, saves canonically and follows external changes")
	{
		ClearAssetCache clear;
		TempProject project("MaterialDraft");
		WritePixel("Assets/Rock.png");
		const std::string path = "Assets/Draft.bmaterial";
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath(path), json{ { "Version", 1 }, { "AlbedoMap", "Assets/Rock.png" } }.dump()));
		MaterialAssetDraft draft;
		std::string error;
		CHECK_FALSE(draft.Load("Assets/Missing.bmaterial", error));
		CHECK_FALSE(draft.IsOpen());
		REQUIRE_MESSAGE(draft.Load(path, error), error);
		CHECK_FALSE(draft.IsDirty());
		// Every slot's transform is editable.
		CHECK(draft.GetData()["TextureTransforms"].size() == MaterialTextureSlotCount);
		// Whole numbers stay floats for the widgets (the file writes them as integers); texture coordinate sets stay integers.
		CHECK(draft.GetData()["Metallic"].is_number_float());
		CHECK(draft.GetData()["AlbedoColor"][3].is_number_float());
		CHECK(draft.GetData()["TextureTransforms"]["Normal"]["Scale"][0].is_number_float());
		CHECK(draft.GetData()["TextureTransforms"]["Normal"]["TexCoord"].is_number_integer());
		CHECK(draft.GetData()["Version"].is_number_float() == false);
		draft.GetData()["Metallic"] = 0.5;
		draft.GetData()["TextureTransforms"]["Normal"]["TexCoord"] = 1;
		CHECK(draft.IsDirty());
		const auto text = draft.MakeFileText(error);
		REQUIRE_MESSAGE(text, error);
		const json saved = ParseJson(*text);
		CHECK(saved["AlbedoMap"].is_object());
		CHECK(saved["TextureTransforms"].size() == 1);
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath(path), *text));
		REQUIRE_MESSAGE(draft.MarkSaved(*text, error), error);
		CHECK_FALSE(draft.IsDirty());
		CHECK(draft.GetData()["Metallic"] == 0.5);
		CHECK(draft.GetData()["Roughness"] == 0.5);
		// A value changed and changed back to a float that reads the same (a widget writes 0.6f back as
		// 0.60000002384...) is not an edit.
		draft.GetData()["Metallic"] = 0.6;
		const auto edited = draft.MakeFileText(error);
		REQUIRE_MESSAGE(edited, error);
		REQUIRE_MESSAGE(draft.MarkSaved(*edited, error), error);
		REQUIRE(draft.GetData()["Metallic"] == 0.6);
		draft.GetData()["Metallic"] = 0.25;
		CHECK(draft.IsDirty());
		draft.GetData()["Metallic"] = static_cast<double>(0.6f);
		CHECK_FALSE(draft.IsDirty());

		// Invalid edits are reported and never saved.
		draft.GetData()["Transmission"] = 5;
		CHECK_FALSE(draft.Parse(error));
		CHECK_FALSE(draft.MakeFileText(error));
		CHECK(error.find("Transmission") != std::string::npos);

		// A dirty draft keeps its edits when the file changes; Revert loads the new version.
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath(path), json{ { "Version", 1 }, { "Roughness", 0.25 } }.dump()));
		CHECK_FALSE(draft.OnFileChanged(error));
		CHECK(draft.HasExternalChange());
		CHECK(draft.GetData()["Transmission"] == 5);
		REQUIRE(draft.Revert(error));
		CHECK_FALSE(draft.HasExternalChange());
		CHECK(draft.GetData()["Roughness"] == 0.25);
		// A clean draft follows the file.
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath(path), json{ { "Version", 1 }, { "Roughness", 0.75 } }.dump()));
		CHECK(draft.OnFileChanged(error));
		CHECK(draft.GetData()["Roughness"] == 0.75);
		CHECK_FALSE(draft.IsDirty());
		// An unreadable new version leaves the draft as it was, says why and marks the conflict (a save would replace it).
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath(path), "{"));
		CHECK_FALSE(draft.OnFileChanged(error));
		CHECK_FALSE(error.empty());
		CHECK(draft.HasExternalChange());
		CHECK(draft.GetData()["Roughness"] == 0.75);
		draft.Close();
		CHECK_FALSE(draft.IsOpen());
	}

	TEST_CASE("The material editor's draft keeps unresolved references and follows moves")
	{
		ClearAssetCache clear;
		TempProject project("MaterialDraftMoves");
		CommandRegistry registry;
		AutomationSession session;
		WritePixel("Assets/Rock.png");
		std::string error;
		// An unresolved UUID round trips through an edit and a save, keeping its identity.
		const json unresolved = { { "UUID", "4242" }, { "Path", "Assets/Gone.png" } };
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Lost.bmaterial"), json{ { "Version", 1 }, { "AlbedoMap", unresolved } }.dump()));
		MaterialAssetDraft lost;
		REQUIRE_MESSAGE(lost.Load("Assets/Lost.bmaterial", error), error);
		REQUIRE(lost.GetWarnings().size() == 1);
		CHECK(Contains(lost.GetWarnings()[0], "unresolved asset UUID 4242"));
		lost.GetData()["Metallic"] = 0.5;
		const auto lostText = lost.MakeFileText(error);
		REQUIRE_MESSAGE(lostText, error);
		CHECK(ParseJson(*lostText)["AlbedoMap"] == unresolved);

		// A move of the draft's texture or of its own file is followed, edits and all.
		Run(registry, session, "asset.write", { { "path", "Assets/Wall.bmaterial" }, { "content", json{ { "Version", 1 }, { "AlbedoMap", "Assets/Rock.png" } }.dump() } });
		MaterialAssetDraft draft;
		REQUIRE_MESSAGE(draft.Load("Assets/Wall.bmaterial", error), error);
		draft.GetData()["Metallic"] = 0.5;
		session.OnAssetMoved = [&](const std::string& from, const std::string& to) { draft.OnAssetMoved(from, to); };
		Run(registry, session, "asset.move", { { "from", "Assets/Rock.png" }, { "to", "Assets/Stone/Rock.png" } });
		Run(registry, session, "asset.rename", { { "from", "Assets/Wall.bmaterial" }, { "to", "Assets/Walls.bmaterial" } });
		CHECK(draft.GetPath() == "Assets/Walls.bmaterial");
		CHECK(draft.GetData()["AlbedoMap"] == "Assets/Stone/Rock.png");
		CHECK(draft.GetData()["Metallic"] == 0.5);
		CHECK(draft.IsDirty());
		const auto text = draft.MakeFileText(error);
		REQUIRE_MESSAGE(text, error);
		const json saved = ParseJson(*text);
		REQUIRE(saved["AlbedoMap"].is_object());
		CHECK(saved["AlbedoMap"]["Path"] == "Assets/Stone/Rock.png");
	}

	TEST_CASE("Imported materials resolve without caching while their model streams in")
	{
		ClearAssetCache clear;
		TempProject project("MaterialStreaming");
		const std::string model = WriteMaterialModel(project);
		const std::string key = model + "#material/0";
		AssetManager::PublishCompleted();
		CHECK_FALSE(AssetManager::RequestMaterial(key));
		CHECK(AssetManager::IsLoading(model));
		CHECK(AssetManager::GetError(key).empty());
		const auto loading = AssetManager::GetLoadedKeys();
		CHECK(std::find(loading.begin(), loading.end(), key) == loading.end());
		JobSystem::Get().WaitIdle();
		// Finished workers publish only at the frame fence.
		CHECK_FALSE(AssetManager::RequestMaterial(key));
		AssetManager::PublishCompleted();
		const auto published = AssetManager::RequestMaterial(key);
		REQUIRE(published);
		CHECK(published->Name == "Red/Paint");
		CHECK(AssetManager::RequestMaterial(key) == published);

		// A model that fails is a cached failure until it changes.
		const std::string missing = "Assets/Models/Missing.gltf";
		CHECK_FALSE(AssetManager::RequestMaterial(missing + "#material/0"));
		JobSystem::Get().WaitIdle();
		AssetManager::PublishCompleted();
		CHECK_FALSE(AssetManager::RequestMaterial(missing + "#material/0"));
		CHECK(Contains(AssetManager::GetError(missing + "#material/0"), "cannot load 'Assets/Models/Missing.gltf' for its material"));
		std::filesystem::copy_file(Project::ResolvePath(model), Project::ResolvePath(missing));
		AssetManager::ReloadChanged({ missing });
		CHECK(AssetManager::GetMaterial(missing + "#material/0"));
	}

	TEST_CASE("Switching projects drops their material files")
	{
		ClearAssetCache clear;
		Ref<const MaterialData> first;
		{
			TempProject project("MaterialProjectA");
			REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/M.bmaterial"), R"({"Version": 1, "Metallic": 0.25})"));
			first = AssetManager::GetMaterial("Assets/M.bmaterial");
			REQUIRE(first);
		}
		TempProject project("MaterialProjectB");
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/M.bmaterial"), R"({"Version": 1, "Metallic": 0.75})"));
		const auto second = AssetManager::GetMaterial("Assets/M.bmaterial");
		REQUIRE(second);
		CHECK(second->Metallic == 0.75f);
		CHECK(first->Metallic == 0.25f);
	}

	TEST_CASE("Inline Material components bind every map in its own slot")
	{
		Scene scene;
		Entity entity = scene.CreateEntity("Maps");
		std::string error;
		json data = json::object();
		for (size_t slot = 0; slot < MaterialTextureSlotCount; slot++)
			data[MaterialAsset::Slots[slot].MapField] = "Assets/slot" + std::to_string(slot) + ".png";
		REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(entity, "Material", data, error), error);
		const auto& component = entity.GetComponent<MaterialComponent>();
		const auto maps = MaterialAsset::GetMaps(component);
		const MaterialData full = MaterialAsset::FromComponent(component);
		for (size_t slot = 0; slot < MaterialTextureSlotCount; slot++)
		{
			INFO(MaterialAsset::Slots[slot].MapField);
			const std::string expected = "Assets/slot" + std::to_string(slot) + ".png";
			CHECK(*maps[slot] == expected);
			CHECK(full.*MaterialAsset::Slots[slot].Map == expected);
		}
		// The renderer's textures-free conversion copies no paths but every value.
		const MaterialData scalars = MaterialAsset::ScalarsFromComponent(component);
		CHECK(MaterialAsset::GetTextures(scalars).empty());
		for (size_t slot = 0; slot < MaterialTextureSlotCount; slot++)
		{
			CHECK(scalars.TextureTransforms[slot].Scale == full.TextureTransforms[slot].Scale);
			CHECK(scalars.TextureTransforms[slot].TexCoord == full.TextureTransforms[slot].TexCoord);
		}
	}

	TEST_CASE("The editor's field list matches what material files hold")
	{
		MaterialData material;
		material.TextureTransforms[0].TexCoord = 1;
		std::string error;
		const auto data = MaterialAsset::ToJson(material, false, error);
		REQUIRE_MESSAGE(data, error);
		std::set<std::string> expected = { "Version", "TextureTransforms" };
		for (const char* field : MaterialAsset::ScalarFields)
			expected.insert(field);
		for (const auto& slot : MaterialAsset::Slots)
			expected.insert(slot.MapField);
		std::set<std::string> written;
		for (const auto& [key, value] : data->items())
			written.insert(key);
		CHECK(written == expected);
		std::set<std::string> transformKeys;
		for (const auto& [key, value] : (*data)["TextureTransforms"]["Albedo"].items())
			transformKeys.insert(key);
		CHECK(transformKeys == std::set<std::string>(std::begin(MaterialAsset::TransformFields), std::end(MaterialAsset::TransformFields)));
	}

	TEST_CASE("Built-in meshes give UV1 maps the same coordinates as UV0")
	{
		ClearAssetCache clear;
		const auto sphere = AssetManager::GetMesh("builtin://Sphere");
		REQUIRE(sphere);
		REQUIRE_FALSE(sphere->Vertices.empty());
		CHECK(std::all_of(sphere->Vertices.begin(), sphere->Vertices.end(), [](const Vertex& vertex) { return vertex.TexCoord1 == vertex.TexCoord; }));
		CHECK(std::any_of(sphere->Vertices.begin(), sphere->Vertices.end(), [](const Vertex& vertex) { return vertex.TexCoord1 != glm::vec2(0.0f); }));
	}

	TEST_CASE("Moves skip broken material files, follow open-scene imported materials and notify only on success")
	{
		ClearAssetCache clear;
		TempProject project("MaterialMoves");
		CommandRegistry registry;
		AutomationSession session;
		WritePixel("Assets/Rock.png");
		const std::string model = WriteMaterialModel(project);
		std::vector<std::pair<std::string, std::string>> notified;
		session.OnAssetMoved = [&](const std::string& from, const std::string& to) { notified.emplace_back(from, to); };

		// A material file that is not JSON cannot reference anything: it is skipped with a warning, not a failed move.
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Broken.bmaterial"), "{"));
		const auto moved = Run(registry, session, "asset.move", { { "from", "Assets/Rock.png" }, { "to", "Assets/Stone.png" } });
		CHECK(Contains(moved.value("warnings", json::array()).dump(), "cannot migrate 'Assets/Broken.bmaterial'"));
		CHECK(notified.size() == 1);
		// A broken scene still stops a move: it might reference the asset.
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Broken.bscene"), "{"));
		CHECK(Contains(RunError(registry, session, "asset.move", { { "from", "Assets/Stone.png" }, { "to", "Assets/Rock.png" } }), "cannot migrate 'Assets/Broken.bscene'"));
		CHECK(notified.size() == 1);
		std::filesystem::remove(Project::ResolvePath("Assets/Broken.bscene"));

		// The open scene's imported-material reference follows its model, fragment and all, and saves by UUID.
		Run(registry, session, "scene.new", { { "name", "Moves" }, { "empty", true } });
		Run(registry, session, "entity.create", { { "name", "Painted" } });
		Run(registry, session, "component.set", { { "entity", "Painted" }, { "component", "Material" }, { "data", { { "Material", model + "#material/0" } } } });
		Run(registry, session, "asset.move", { { "from", model }, { "to", "Assets/Models/Moved.gltf" } });
		CHECK(Run(registry, session, "component.get", { { "entity", "Painted" }, { "component", "Material" } })["Material"] == "Assets/Models/Moved.gltf#material/0");
		Run(registry, session, "scene.save", { { "path", "Assets/Moves.bscene" } });
		const json saved = ParseJson(*FileSystem::ReadTextFile(Project::ResolvePath("Assets/Moves.bscene")));
		const json reference = saved["Entities"][0]["Components"]["Material"]["Material"];
		REQUIRE(reference.is_object());
		CHECK(reference["Path"] == "Assets/Models/Moved.gltf#material/0");
		CHECK(reference["UUID"] == Run(registry, session, "asset.id", { { "path", "Assets/Models/Moved.gltf" } })["uuid"]);
	}

	TEST_CASE("A clean draft stays clean when its texture moves")
	{
		ClearAssetCache clear;
		TempProject project("MaterialCleanMove");
		CommandRegistry registry;
		AutomationSession session;
		WritePixel("Assets/Rock.png");
		Run(registry, session, "asset.write", { { "path", "Assets/Wall.bmaterial" }, { "content", json{ { "Version", 1 }, { "AlbedoMap", "Assets/Rock.png" } }.dump() } });
		MaterialAssetDraft draft;
		std::string error;
		REQUIRE_MESSAGE(draft.Load("Assets/Wall.bmaterial", error), error);
		session.OnAssetMoved = [&](const std::string& from, const std::string& to) { draft.OnAssetMoved(from, to); };
		Run(registry, session, "asset.move", { { "from", "Assets/Rock.png" }, { "to", "Assets/Stone.png" } });
		CHECK(draft.GetData()["AlbedoMap"] == "Assets/Stone.png");
		CHECK_FALSE(draft.IsDirty());
		CHECK_FALSE(draft.HasExternalChange());
	}

	TEST_CASE("Materials with unresolved textures reload only when metadata changes")
	{
		ClearAssetCache clear;
		TempProject project("MaterialUnresolvedReload");
		const std::string path = "Assets/Lost.bmaterial";
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath(path), json{ { "Version", 1 }, { "AlbedoMap", { { "UUID", "4242" }, { "Path", "Assets/Gone.png" } } } }.dump()));
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Note.lua"), "return {}"));
		REQUIRE(AssetManager::GetMaterial(path));
		// An unrelated file cannot make the UUID resolve: no reload, no repeated warning.
		CHECK(AssetManager::ReloadChanged({ "Assets/Note.lua" }).empty());
		// A sidecar can.
		const auto dropped = AssetManager::ReloadChanged({ "Assets/Other.png.meta" });
		CHECK(std::find(dropped.begin(), dropped.end(), path) != dropped.end());

		// A resolved texture's sidecar changing (a reassigned UUID) drops the material too.
		WritePixel("Assets/Rock.png");
		MaterialData material;
		material.AlbedoMap = "Assets/Rock.png";
		std::string error;
		const auto text = MaterialAsset::ToJson(material, true, error);
		REQUIRE_MESSAGE(text, error);
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Rock.bmaterial"), text->dump()));
		REQUIRE(AssetManager::GetMaterial("Assets/Rock.bmaterial"));
		const auto byMeta = AssetManager::ReloadChanged({ "Assets/Rock.png.meta" });
		CHECK(std::find(byMeta.begin(), byMeta.end(), "Assets/Rock.bmaterial") != byMeta.end());
	}

	TEST_CASE("material.extract names unnamed materials and reports texture problems")
	{
		ClearAssetCache clear;
		TempProject project("MaterialExtractNames");
		CommandRegistry registry;
		AutomationSession session;
		const std::string model = WriteMaterialModel(project);
		json gltf = ParseJson(*FileSystem::ReadTextFile(Project::ResolvePath(model)));
		gltf["materials"][0]["name"] = "...";
		gltf["materials"][1]["name"] = "CON";
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath(model), gltf.dump()));
		std::filesystem::remove(Project::ResolvePath("Assets/Models/Paint.png"));
		const auto extracted = Run(registry, session, "material.extract", { { "model", model } });
		REQUIRE(extracted["materials"].size() == 2);
		CHECK(extracted["materials"][0]["path"] == "Assets/Models/Materials/Material_0.bmaterial");
		CHECK(extracted["materials"][1]["path"] == "Assets/Models/Materials/CON_.bmaterial");
		const std::string warnings = extracted.value("warnings", json::array()).dump();
		CHECK_MESSAGE(Contains(warnings, "Assets/Models/Materials/Material_0.bmaterial: AlbedoMap: 'Assets/Models/Paint.png' does not exist"), warnings);
	}

	TEST_CASE("Validation decodes a material file's UUIDs with that project's registry")
	{
		ClearAssetCache clear;
		Ref<Project> validated;
		{
			TempProject first("MaterialValidatedProject");
			validated = Project::GetActive();
			WritePixel("Assets/Rock.png");
			MaterialData material;
			material.AlbedoMap = "Assets/Rock.png";
			std::string error;
			const auto text = MaterialAsset::ToJson(material, true, error);
			REQUIRE_MESSAGE(text, error);
			REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Rock.bmaterial"), text->dump()));
			// Another project is active while the first is validated.
			TempProject other("MaterialOtherProject");
			const auto report = ValidateProject(*validated);
			INFO(json(report.GetIssues().size()).dump());
			CHECK(std::none_of(report.GetIssues().begin(), report.GetIssues().end(), [](const ValidationIssue& issue) { return issue.File == "Assets/Rock.bmaterial"; }));
		}
	}
}
