#include <doctest/doctest.h>

#include <Basalt/Asset/AssetKey.h>
#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Asset/AssetRegistry.h>
#include <Basalt/Asset/ImageUtils.h>
#include <Basalt/Asset/MaterialAsset.h>
#include <Basalt/Asset/Thumbnails.h>
#include <Basalt/Automation/CommandRegistry.h>
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

using namespace Basalt;
using json = nlohmann::json;
using AnimationTest::ClearAssetCache;
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
			MaterialAsset::GetMap(m, static_cast<MaterialTextureSlot>(slot)) = "Assets/Textures/" + std::string(MaterialAsset::SlotNames[slot]) + ".png";
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
			INFO(MaterialAsset::SlotNames[slot]);
			CHECK(MaterialAsset::GetMap(a, static_cast<MaterialTextureSlot>(slot)) == MaterialAsset::GetMap(b, static_cast<MaterialTextureSlot>(slot)));
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
		CHECK(MaterialAsset::FromJson(FileJson(decimal), error)->Roughness == 0.6f);
		decimal.NormalScale = std::numeric_limits<float>::max();
		decimal.ClearcoatNormalScale = -std::numeric_limits<float>::max();
		CHECK(MaterialAsset::FromJson(FileJson(decimal), error)->NormalScale == std::numeric_limits<float>::max());
		CHECK(MaterialAsset::FromJson(FileJson(decimal), error)->ClearcoatNormalScale == -std::numeric_limits<float>::max());
		MaterialData one;
		one.TextureTransforms[static_cast<size_t>(MaterialTextureSlot::Emissive)].TexCoord = 1;
		CHECK(FileJson(one)["TextureTransforms"].size() == 1);
		CHECK(FileJson(one)["TextureTransforms"].contains("Emissive"));
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

		Run(registry, session, "asset.move", { { "from", "Assets/Textures/Rock.png" }, { "to", "Assets/Textures/Moved/Rock.png" } });
		Run(registry, session, "asset.move", { { "from", model }, { "to", "Assets/Models/Moved.gltf" } });
		AssetManager::Clear();
		const auto moved = AssetManager::GetMaterial("Assets/Materials/Rock.bmaterial");
		REQUIRE(moved);
		CHECK(moved->AlbedoMap == "Assets/Textures/Moved/Rock.png");
		CHECK(moved->EmissiveMap == "Assets/Models/Moved.gltf#image/0");
		CHECK(moved->Name == "Rock");
		const json legacy = ParseJson(*FileSystem::ReadTextFile(Project::ResolvePath("Assets/Materials/Legacy.bmaterial")));
		REQUIRE(legacy["AlbedoMap"].is_object());
		// Only references to the moved file are rewritten.
		CHECK(legacy["NormalMap"] == "Assets/Textures/Other.png");
		CHECK(AssetManager::GetMaterial("Assets/Materials/Legacy.bmaterial")->AlbedoMap == "Assets/Textures/Moved/Rock.png");

		// A texture whose identity is gone renders without it and says so once.
		std::filesystem::remove(Project::ResolvePath("Assets/Textures/Moved/Rock.png"));
		std::filesystem::remove(Project::ResolvePath("Assets/Textures/Moved/Rock.png.meta"));
		REQUIRE(Project::GetActive()->GetAssets().Refresh(error));
		AssetManager::Clear();
		const auto since = Log::GetHistory().GetTotalCount();
		const auto unresolved = AssetManager::GetMaterial("Assets/Materials/Rock.bmaterial");
		REQUIRE(unresolved);
		CHECK(AssetRegistry::IsUnresolvedKey(unresolved->AlbedoMap));
		CHECK(BasaltTest::CountLogMessages(since, "unresolved asset UUID") == 1);
		const auto report = ValidateProject(*Project::GetActive());
		CHECK(std::any_of(report.GetIssues().begin(), report.GetIssues().end(), [](const ValidationIssue& issue) { return issue.File == "Assets/Materials/Rock.bmaterial" && issue.Message.find("unresolved asset UUID") != std::string::npos; }));
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
		CHECK(AssetManager::GetMaterial(path)->Metallic == 1.0f);

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
		CHECK(AssetManager::GetMaterial(model + "#material/1")->Metallic == doctest::Approx(0.25f));
		// The importer's default material for primitives without one is not addressable.
		CHECK_FALSE(AssetManager::GetMaterial(model + "#material/2"));
		CHECK(AssetManager::GetError(model + "#material/2").find("has no material 2 (it has 2)") != std::string::npos);
		CHECK_FALSE(AssetManager::GetMaterial(model + "#material/x"));
		CHECK(AssetManager::GetError(model + "#material/x").find("invalid imported material key") != std::string::npos);
		CHECK_FALSE(AssetManager::GetMaterial("builtin://Sphere#material/0"));
		CHECK_FALSE(AssetManager::GetMaterial("Assets/Models/Paint.png"));
		CHECK(AssetManager::GetError("Assets/Models/Paint.png").find("is not a material") != std::string::npos);

		// A model change drops its imported materials with it.
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
		for (const char* valid : { "", "Assets/M.bmaterial", "Assets/M.BMATERIAL", "Assets/Ship.glb#material/0" })
		{
			INFO(valid);
			CHECK_MESSAGE(ComponentRegistry::AddOrPatch(entity, "Material", { { "Material", valid } }, error), error);
		}
		for (const char* invalid : { "Assets/a.png", "Assets/Ship.glb", "Assets/Ship.glb#image/0", "Assets/Ship.glb#material/x" })
		{
			INFO(invalid);
			CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Material", { { "Material", invalid } }, error));
			CHECK(error.find("must be a .bmaterial file or <model>#material/<n>") != std::string::npos);
		}
		CHECK(entity.GetComponent<MaterialComponent>().Material == "Assets/Ship.glb#material/0");
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

		// material.create: defaults, then from an entity's inline values or its asset.
		CHECK(Run(registry, session, "material.create", { { "path", "Assets/Materials/Default.bmaterial" } })["path"] == "Assets/Materials/Default.bmaterial");
		CheckSame(*AssetManager::GetMaterial("Assets/Materials/Default.bmaterial"), MaterialData{});
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
		Run(registry, session, "material.create", { { "path", "Assets/Materials/FromAsset.bmaterial" }, { "entity", "Plain" }, { "overwrite", true } });
		CHECK(AssetManager::GetMaterial("Assets/Materials/FromAsset.bmaterial")->Alpha == AlphaMode::Mask);
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

		// asset.preload reads a material and requests its textures.
		const auto preload = Run(registry, session, "asset.preload", { { "paths", { "Assets/Materials/Plain.bmaterial", "Assets/Materials/Gone.bmaterial" } }, { "wait", true } });
		REQUIRE(preload.size() == 3);
		CHECK(preload[0]["status"] == "ready");
		CHECK(preload[1]["status"] == "failed");
		CHECK(preload[2]["path"] == "Assets/Textures/Rock.png");
		CHECK(preload[2]["status"] == "ready");
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
		const auto report = ValidateProject(*Project::GetActive());
		auto has = [&](const std::string& file, const std::string& text) {
			return std::any_of(report.GetIssues().begin(), report.GetIssues().end(), [&](const ValidationIssue& issue) { return issue.File == file && issue.Message.find(text) != std::string::npos; });
		};
		CHECK(has("Assets/Bad.bmaterial", "invalid material: Clearcoat 3 must be in [0, 1]"));
		CHECK(has("Assets/Missing.bmaterial", "NormalMap references the missing file 'Assets/missing.png'"));
		CHECK(has("Assets/Case.bmaterial", "letter case matters"));
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
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/A.bmaterial"), MaterialAsset::ToJson(material, true, error)->dump()));
		const ThumbnailCache cache(project.GetDirectory());
		const auto dependencies = cache.GetDependencies("Assets/A.bmaterial");
		CHECK(dependencies == std::vector<std::string>{ "Assets/Models/Paint.png", "Assets/Models/Sphere.bin", "Assets/Models/Sphere.gltf", "Assets/Rock.png" });
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
		draft.MarkSaved();
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
		// An unreadable new version leaves the draft as it was and says why.
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath(path), "{"));
		CHECK_FALSE(draft.OnFileChanged(error));
		CHECK_FALSE(error.empty());
		CHECK(draft.GetData()["Roughness"] == 0.75);
		draft.Close();
		CHECK_FALSE(draft.IsOpen());
	}
}
