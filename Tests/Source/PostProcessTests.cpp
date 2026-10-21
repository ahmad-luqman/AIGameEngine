#include <doctest/doctest.h>

#include <Basalt/Automation/AutomationSession.h>
#include <Basalt/Automation/CommandRegistry.h>
#include <Basalt/Project/Project.h>
#include <Basalt/Renderer/GraphicsSettings.h>
#include <Basalt/Renderer/PostProcessSettings.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/SceneSerializer.h>
#include <Basalt/Scripting/ScriptEngine.h>

#include "TestUtils.h"

#include <limits>
#include <memory>

using namespace Basalt;
using namespace BasaltTest;

TEST_SUITE("PostProcess")
{
	TEST_CASE("Post-process selection is deterministic and falls back to legacy scene settings")
	{
		auto scene = CreateRef<Scene>();
		scene->GetRendererSettings().Exposure = 0.8f;
		CHECK(ResolvePostProcess(*scene).Exposure == 0.8f);
		auto high = scene->CreateEntityWithUUID(20, "HighID");
		auto low = scene->CreateEntityWithUUID(10, "LowID");
		high.AddComponent<PostProcessComponent>().Exposure = 2;
		low.AddComponent<PostProcessComponent>().Exposure = 3;
		UUID selected = 0;
		CHECK(ResolvePostProcess(*scene, &selected).Exposure == 3);
		CHECK(selected == 10);
		high.GetComponent<PostProcessComponent>().Priority = 1;
		CHECK(ResolvePostProcess(*scene, &selected).Exposure == 2);
		CHECK(selected == 20);
		high.GetComponent<PostProcessComponent>().Enabled = false;
		CHECK(ResolvePostProcess(*scene).Exposure == 3);
		low.GetComponent<PostProcessComponent>().Enabled = false;
		CHECK(ResolvePostProcess(*scene, &selected).Exposure == 0.8f);
		CHECK(selected == 0);
		low.GetComponent<PostProcessComponent>().Enabled = true;
		auto copy = Scene::Copy(scene);
		CHECK(ResolvePostProcess(*copy).Exposure == 3);
		std::string error;
		Scene loaded;
		REQUIRE(SceneSerializer::DeserializeScene(loaded, SceneSerializer::SerializeScene(*scene), error));
		CHECK(ResolvePostProcess(loaded).Exposure == 3);
	}

	TEST_CASE("Post-process patches validate atomically through the component registry")
	{
		Scene scene;
		auto entity = scene.CreateEntity();
		const auto* info = ComponentRegistry::Find("PostProcess");
		REQUIRE(info);
		std::string error;
		REQUIRE(info->Deserialize(entity, { { "Exposure", 2 }, { "BloomEnabled", true }, { "AutoExposure", true }, { "FXAAEnabled", true } }, error));
		const auto before = info->Serialize(entity);
		for (const nlohmann::json& patch : std::vector<nlohmann::json>{
				 { { "MinExposure", 0 } }, { { "MaxExposure", 0 } }, { { "Exposure", 65505 } }, { { "MinExposure", 65505 } }, { { "MaxExposure", 65505 } }, { { "ExposureCompensation", -17 } }, { { "ExposureCompensation", 17 } }, { { "AdaptationSpeed", 101 } }, { { "SSAORadius", 101 } }, { { "SSAOIntensity", 11 } }, { { "SSAOBias", 2 } }, { { "BloomThreshold", 65505 } }, { { "BloomIntensity", 11 } }, { { "MaxExposure", 0.01 } }, { { "Exposure", 0 } }, { { "Exposure", 1e100 } }, { { "MinExposure", 100 }, { "MaxExposure", 1 } }, { { "AdaptationSpeed", -1 } }, { { "BloomScatter", 1.1 } }, { { "BloomThreshold", -1 } }, { { "BloomIntensity", -1 } }, { { "SSAOIntensity", -1 } }, { { "SSAOBias", -1 } }, { { "SSAORadius", 0 } }, { { "Tonemapper", "Typo" } }, { { "AutoExposure", 1 } }, { { "Priority", -1 } }, { { "Priority", 1.5 } }, { { "Priority", UINT64_MAX } }, { { "Unknown", true } } })
		{
			CAPTURE(patch.dump());
			CHECK_FALSE(info->Deserialize(entity, patch, error));
			CHECK_FALSE(error.empty());
			CHECK(info->Serialize(entity) == before);
		}
		auto invalid = entity.GetComponent<PostProcessComponent>();
		invalid.Exposure = std::numeric_limits<float>::quiet_NaN();
		CHECK_FALSE(ValidatePostProcess(invalid).empty());
		invalid = PostProcessComponent{};
		invalid.Tonemap = static_cast<Tonemapper>(99);
		CHECK_FALSE(ValidatePostProcess(invalid).empty());
	}

	TEST_CASE("Graphics preferences validate atomically and reject unknown keys")
	{
		GraphicsSettings settings;
		std::string error;
		REQUIRE(PatchGraphicsSettings({ { "VSync", false }, { "RenderScale", 0.5 }, { "ShadowQuality", "Low" }, { "SSAOQuality", "Off" }, { "DynamicResolution", true }, { "TargetFrameRate", 90 }, { "MinRenderScale", 0.25 } }, settings, error));
		CHECK_FALSE(settings.VSync);
		CHECK(settings.ShadowQuality == GraphicsQuality::Low);
		CHECK(settings.SSAOQuality == GraphicsQuality::Off);
		const auto before = SerializeGraphicsSettings(settings);
		for (const nlohmann::json& patch : std::vector<nlohmann::json>{
				 nlohmann::json::array(), { { "DynamicResolution", 1 } }, { { "RenderScale", 0 } }, { { "RenderScale", 1e100 } }, { { "VSync", 1 } }, { { "ShadowQuality", "Ultra" } }, { { "SSAOQuality", 0 } }, { { "TargetFrameRate", 1 } }, { { "MinRenderScale", 5 } }, { { "VSync", true }, { "Typo", 1 } } })
		{
			CAPTURE(patch.dump());
			CHECK_FALSE(PatchGraphicsSettings(patch, settings, error));
			CHECK_FALSE(error.empty());
			CHECK(SerializeGraphicsSettings(settings) == before);
		}
	}

	TEST_CASE("Every post-process field round-trips without aliasing and invalid enums serialize safely")
	{
		Scene scene;
		auto entity = scene.CreateEntity();
		const auto* info = ComponentRegistry::Find("PostProcess");
		REQUIRE(info);
		const nlohmann::json values = {
			{ "Enabled", false }, { "Priority", 42 }, { "Tonemapper", "AgX" }, { "Exposure", 2.0 }, { "AutoExposure", true }, { "MinExposure", 0.5 }, { "MaxExposure", 8.0 }, { "ExposureCompensation", -2.0 }, { "AdaptationSpeed", 5.0 }, { "SSAOEnabled", false }, { "SSAORadius", 0.8 }, { "SSAOIntensity", 1.5 }, { "SSAOBias", 0.1 }, { "FXAAEnabled", true }, { "TAAEnabled", true }, { "BloomEnabled", false }, { "BloomThreshold", 3.0 }, { "BloomIntensity", 0.25 }, { "BloomScatter", 0.4 }
		};
		std::string error;
		REQUIRE(info->Deserialize(entity, values, error));
		const auto serialized = info->Serialize(entity);
		for (const auto& [key, value] : values.items())
		{
			CAPTURE(key);
			if (value.is_number_float())
				CHECK(serialized[key].get<double>() == doctest::Approx(value.get<double>()));
			else
				CHECK(serialized[key] == value);
		}
		Scene loaded;
		REQUIRE(SceneSerializer::DeserializeScene(loaded, SceneSerializer::SerializeScene(scene), error));
		CHECK(info->Serialize(loaded.GetEntityByUUID(entity.GetUUID())) == serialized);
		entity.GetComponent<PostProcessComponent>().Tonemap = static_cast<Tonemapper>(99);
		CHECK(info->Serialize(entity)["Tonemapper"] == "None");
		scene.GetRendererSettings().Tonemap = static_cast<Tonemapper>(99);
		CHECK(SceneSerializer::SerializeScene(scene)["Renderer"]["Tonemapper"] == "None");
		GraphicsSettings graphics;
		graphics.ShadowQuality = static_cast<GraphicsQuality>(99);
		CHECK(SerializeGraphicsSettings(graphics)["ShadowQuality"] == "Off");
	}

	TEST_CASE("Render revisions distinguish address reuse, copies and new Play sessions")
	{
		alignas(Scene) std::byte storage[sizeof(Scene)];
		auto* first = std::construct_at(reinterpret_cast<Scene*>(storage));
		const auto revision = first->GetRenderRevision();
		std::destroy_at(first);
		auto* second = std::construct_at(reinterpret_cast<Scene*>(storage));
		CHECK(second->GetRenderRevision() > revision);
		const auto beforePlay = second->GetRenderRevision();
		second->OnRuntimeStart();
		CHECK(second->GetRenderRevision() > beforePlay);
		second->OnRuntimeStop();
		std::destroy_at(second);
		auto original = CreateRef<Scene>();
		CHECK(Scene::Copy(original)->GetRenderRevision() > original->GetRenderRevision());
	}

	TEST_CASE("Legacy scene setting edits report the overriding PostProcess entity")
	{
		CommandRegistry commands;
		AutomationSession session;
		commands.Execute(session, "scene.new", { { "empty", true } });
		commands.Execute(session, "entity.create", { { "name", "Look" }, { "components", { { "PostProcess", nlohmann::json::object() } } } });
		const auto result = commands.Execute(session, "scene.settings", { { "Renderer", { { "Exposure", 2 } } } });
		REQUIRE(result.contains("warnings"));
		CHECK(result["warnings"][0].get<std::string>().find("Look") != std::string::npos);
		CHECK(result["Renderer"]["Exposure"] == 2);
		CHECK_FALSE(commands.Execute(session, "scene.settings", { { "Name", "Renamed" } }).contains("warnings"));
	}

	TEST_CASE("Explicit host graphics overrides remain above Lua session preferences")
	{
		GraphicsOverrides overrides;
		overrides.VSync = false;
		overrides.RenderScale = 0.75f;
		overrides.DynamicResolution = true;
		overrides.TargetFrameRate = UnreachableTargetFrameRate;
		overrides.MinRenderScale = 0.25f;
		Scene scene;
		scene.OnRuntimeStart(overrides.Apply(GraphicsSettings{}));
		Eval(scene, "Graphics.SetSettings({VSync=true,RenderScale=1,DynamicResolution=false,TargetFrameRate=60,MinRenderScale=0.5,SSAOQuality='Off'}); return true");
		const auto effective = overrides.Apply(scene.GetGraphicsSettings());
		CHECK_FALSE(effective.VSync);
		CHECK(effective.RenderScale == 0.75f);
		CHECK(effective.DynamicResolution.Enabled);
		CHECK(effective.DynamicResolution.TargetFrameRate == UnreachableTargetFrameRate);
		CHECK(effective.DynamicResolution.MinScale == 0.25f);
		CHECK(effective.SSAOQuality == GraphicsQuality::Off);
		CHECK(scene.GetGraphicsSettings().RenderScale == 1.0f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Runtime diagnostic graphics settings round-trip through Lua")
	{
		Scene scene;
		GraphicsSettings initial;
		initial.DynamicResolution.TargetFrameRate = UnreachableTargetFrameRate;
		scene.OnRuntimeStart(initial);
		CHECK(Eval(scene, "Graphics.SetSettings(Graphics.GetSettings()); return true") == "true");
		CHECK(scene.GetGraphicsSettings().DynamicResolution.TargetFrameRate == UnreachableTargetFrameRate);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Project graphics defaults round-trip through automation and disk")
	{
		TempProject project("GraphicsDefaults");
		CommandRegistry commands;
		AutomationSession session;
		const auto patch = nlohmann::json{ { "VSync", false }, { "RenderScale", 0.5 }, { "ShadowQuality", "Low" }, { "SSAOQuality", "Off" } };
		commands.Execute(session, "project.set", { { "graphics", patch } });
		CHECK(commands.Execute(session, "project.info", nlohmann::json::object())["graphics"]["SSAOQuality"] == "Off");
		std::string error;
		const auto loaded = Project::Load(Project::GetActive()->GetDirectory(), error);
		REQUIRE(loaded);
		CHECK_FALSE(loaded->GetConfig().Graphics.VSync);
		CHECK(loaded->GetConfig().Graphics.RenderScale == 0.5f);
		CHECK(loaded->GetConfig().Graphics.ShadowQuality == GraphicsQuality::Low);
		CHECK_THROWS_AS(commands.Execute(session, "project.set", { { "graphics", { { "VSync", true }, { "ShadowQuality", "Ultra" } } } }), CommandError);
		CHECK_FALSE(Project::GetActive()->GetConfig().Graphics.VSync);
		CHECK_THROWS_AS(commands.Execute(session, "project.set", { { "renderScale", 1 }, { "graphics", { { "RenderScale", 0.75 } } } }), CommandError);
		CHECK(Project::GetActive()->GetConfig().Graphics.RenderScale == 0.5f);
		CHECK_NOTHROW(commands.Execute(session, "project.set", { { "graphics", nullptr } }));
		for (const nlohmann::json& invalid : std::vector<nlohmann::json>{ { { "ShadowQuality", "Ultra" } }, { { "SSAOQuality", 0 } }, { { "VSync", "yes" } } })
		{
			project.WriteFile(Project::FileName, invalid.dump());
			CHECK_FALSE(Project::Load(Project::GetActive()->GetDirectory(), error));
			CHECK(error.find("invalid project file '") == 0);
			CHECK(error.find(Project::FileName) != std::string::npos);
			CHECK(error.find(invalid.begin().key()) != std::string::npos);
		}
		// Properties advertised by unrelated commands must not silently accept graphics patches.
		CHECK_THROWS_AS(commands.Execute(session, "scene.new", { { "graphics", patch } }), CommandError);
		CHECK_THROWS_AS(commands.Execute(session, "entity.create", { { "graphics", patch } }), CommandError);
	}

	TEST_CASE("Lua graphics settings are session scoped, survive scene loads and reset on restart")
	{
		TempProject project("PostGraphics");
		Project::GetActive()->GetConfig().Graphics.RenderScale = 0.75f;
		Project::GetActive()->GetConfig().Graphics.ShadowQuality = GraphicsQuality::Low;
		Scene next("Next");
		std::string error;
		REQUIRE(SceneSerializer::SaveScene(next, Project::ResolvePath("Assets/Scenes/Next.bscene"), error));
		Scene scene;
		CHECK_FALSE(scene.SetGraphicsSettings({ { "VSync", false } }, error));
		scene.OnRuntimeStart();
		CHECK(Eval(scene, "return Graphics.GetSettings().RenderScale") == "0.75");
		CHECK(Eval(scene, "return Graphics.GetSettings().ShadowQuality") == "Low");
		const auto serialized = SceneSerializer::SerializeScene(scene);
		Eval(scene, "Graphics.SetSettings({VSync=false, RenderScale=0.5, SSAOQuality='Off'}); return true");
		CHECK(scene.GetGraphicsSettings().RenderScale == 0.5f);
		CHECK_FALSE(scene.GetGraphicsSettings().VSync);
		CHECK(SceneSerializer::SerializeScene(scene) == serialized);
		CHECK(Project::GetActive()->GetConfig().Graphics.RenderScale == 0.75f);
		CHECK(Eval(scene, "return pcall(function() Graphics.SetSettings({VSync=true,RenderScale=0}) end)") == "false");
		CHECK_FALSE(scene.GetGraphicsSettings().VSync);
		const auto revision = scene.GetRenderRevision();
		Eval(scene, "Scene.Load('Assets/Scenes/Next.bscene'); return true");
		scene.OnUpdate(Step);
		CHECK(scene.GetName() == "Next");
		CHECK(scene.GetRenderRevision() > revision);
		CHECK(scene.GetGraphicsSettings().RenderScale == 0.5f);
		scene.OnRuntimeStop();
		scene.OnRuntimeStart();
		CHECK(scene.GetGraphicsSettings().RenderScale == 0.75f);
		CHECK(scene.GetGraphicsSettings().VSync);
		scene.OnRuntimeStop();
	}
}
