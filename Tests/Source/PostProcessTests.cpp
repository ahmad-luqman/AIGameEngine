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
				 { { "Exposure", 0 } }, { { "Exposure", 1e100 } }, { { "MinExposure", 100 }, { "MaxExposure", 1 } }, { { "AdaptationSpeed", -1 } }, { { "BloomScatter", 1.1 } }, { { "BloomThreshold", -1 } }, { { "BloomIntensity", -1 } }, { { "SSAOIntensity", -1 } }, { { "SSAOBias", -1 } }, { { "SSAORadius", 0 } }, { { "Tonemapper", "Typo" } }, { { "AutoExposure", 1 } }, { { "Priority", -1 } }, { { "Priority", 1.5 } }, { { "Priority", UINT64_MAX } }, { { "Unknown", true } } })
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
				 { { "RenderScale", 0 } }, { { "RenderScale", 1e100 } }, { { "VSync", 1 } }, { { "ShadowQuality", "Ultra" } }, { { "SSAOQuality", 0 } }, { { "TargetFrameRate", 1 } }, { { "MinRenderScale", 5 } }, { { "VSync", true }, { "Typo", 1 } } })
		{
			CHECK_FALSE(PatchGraphicsSettings(patch, settings, error));
			CHECK(SerializeGraphicsSettings(settings) == before);
		}
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
		CHECK_FALSE(loaded->GetConfig().VSync);
		CHECK(loaded->GetConfig().RenderScale == 0.5f);
		CHECK(loaded->GetConfig().ShadowQuality == GraphicsQuality::Low);
		CHECK_THROWS_AS(commands.Execute(session, "project.set", { { "graphics", { { "VSync", true }, { "ShadowQuality", "Ultra" } } } }), CommandError);
		CHECK_FALSE(Project::GetActive()->GetConfig().VSync);
		// Properties advertised by unrelated commands must not silently accept graphics patches.
		CHECK_THROWS_AS(commands.Execute(session, "scene.new", { { "graphics", patch } }), CommandError);
		CHECK_THROWS_AS(commands.Execute(session, "entity.create", { { "graphics", patch } }), CommandError);
	}

	TEST_CASE("Lua graphics settings are session scoped, survive scene loads and reset on restart")
	{
		TempProject project("PostGraphics");
		Project::GetActive()->GetConfig().RenderScale = 0.75f;
		Project::GetActive()->GetConfig().ShadowQuality = GraphicsQuality::Low;
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
		CHECK(Project::GetActive()->GetConfig().RenderScale == 0.75f);
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
