#include <doctest/doctest.h>

#include "AnimationGraphDraft.h"
#include "AutomationTestUtils.h"
#include "TestUtils.h"

#include <Basalt/Animation/AnimationGraph.h>
#include <Basalt/Animation/PropertyAnimation.h>
#include <Basalt/Automation/AutomationSession.h>
#include <Basalt/Automation/CommandRegistry.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/SceneSerializer.h>

#include <limits>

using namespace Basalt;
using namespace BasaltTest;
using json = nlohmann::json;

TEST_SUITE("Animation authoring")
{
	TEST_CASE("Property keys capture sorted registry values and replace exact-time keys")
	{
		Scene scene;
		auto entity = scene.CreateEntity("Door");
		std::string error;
		entity.GetTransform().Translation = { 10, 20, 30 };
		REQUIRE(PropertyAnimation::Key(entity, "Transform", "Translation", 1, error));
		entity.GetTransform().Translation = { 0, 4, 8 };
		REQUIRE(PropertyAnimation::Key(entity, "Transform", "Translation", 0, error));
		auto& animation = entity.GetComponent<PropertyAnimationComponent>();
		REQUIRE(animation.Tracks.size() == 1);
		REQUIRE(animation.Tracks[0].Keys.size() == 2);
		CHECK(animation.Tracks[0].Keys[0].Time == 0);
		REQUIRE(PropertyAnimation::Sample(entity, animation, .25f, error));
		CHECK(entity.GetTransform().Translation == glm::vec3(2.5f, 8, 13.5f));
		REQUIRE(PropertyAnimation::Key(entity, "Transform", "Translation", 0, error));
		CHECK(animation.Tracks[0].Keys.size() == 2);
		animation.Tracks[0].Stepped = true;
		REQUIRE(PropertyAnimation::Sample(entity, animation, .75f, error));
		CHECK(entity.GetTransform().Translation == glm::vec3(2.5f, 8, 13.5f));
		REQUIRE(PropertyAnimation::Sample(entity, animation, 1, error));
		CHECK(entity.GetTransform().Translation == glm::vec3(10, 20, 30));
		CHECK_FALSE(PropertyAnimation::Key(entity, "Transform", "NoField", 0, error));
		CHECK_FALSE(PropertyAnimation::Key(entity, "Tag", "Tag", 0, error));
		CHECK_FALSE(PropertyAnimation::Key(entity, "PropertyAnimation", "Speed", 0, error));
		CHECK_FALSE(PropertyAnimation::Sample(entity, animation, std::numeric_limits<float>::quiet_NaN(), error));
	}
	TEST_CASE("Property scalar sampling validates target data and authoring errors atomically")
	{
		Scene scene;
		auto entity = scene.CreateEntity();
		entity.AddComponent<PointLightComponent>().Intensity = 2;
		std::string error;
		REQUIRE(PropertyAnimation::Key(entity, "PointLight", "Intensity", .25f, error));
		entity.GetComponent<PointLightComponent>().Intensity = 10;
		REQUIRE(PropertyAnimation::Key(entity, "PointLight", "Intensity", .75f, error));
		auto& animation = entity.GetComponent<PropertyAnimationComponent>();
		REQUIRE(PropertyAnimation::Sample(entity, animation, .5f, error));
		CHECK(entity.GetComponent<PointLightComponent>().Intensity == doctest::Approx(6));
		REQUIRE(PropertyAnimation::Sample(entity, animation, 0, error));
		CHECK(entity.GetComponent<PointLightComponent>().Intensity == 2);
		REQUIRE(PropertyAnimation::Sample(entity, animation, 1, error));
		CHECK(entity.GetComponent<PointLightComponent>().Intensity == 10);
		const auto* info = ComponentRegistry::Find("PropertyAnimation");
		const auto before = info->Serialize(entity);
		const std::vector<json> invalid = {
			json{ { "Duration", 0 } }, json{ { "Speed", -1 } },
			json{ { "Tracks", json::array({ json{ { "Component", "Typo" }, { "Field", "Intensity" } } }) } },
			json{ { "Tracks", json::array({ json{ { "Keys", json::array({ json{ { "Time", .5f } }, json{ { "Time", .5f } } }) } } }) } }
		};
		for (const auto& bad : invalid)
		{
			CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "PropertyAnimation", bad, error));
			CHECK(info->Serialize(entity) == before);
		}
		animation.Tracks[0].Field = "Radius";
		animation.Tracks[0].Keys[0].Value.x = -5;
		animation.Tracks[0].Keys[1].Value.x = -1;
		CHECK_FALSE(PropertyAnimation::Sample(entity, animation, .5f, error));
		CHECK(entity.GetComponent<PointLightComponent>().Radius == PointLightComponent{}.Radius);
		entity.RemoveComponent<PointLightComponent>();
		CHECK_FALSE(PropertyAnimation::Sample(entity, animation, .5f, error));
		CHECK_FALSE(entity.HasComponent<PointLightComponent>());
	}
	TEST_CASE("Property patches group constrained fields and duplicate tracks use their last sample")
	{
		Scene scene;
		auto entity = scene.CreateEntity("Camera");
		entity.AddComponent<CameraComponent>();
		PropertyAnimationComponent animation;
		// Applying Near alone would fail against the old Far. Both values must share one registry patch.
		animation.Tracks = { { "Camera", "Near", false, { { 0, { 2000, 0, 0, 0 } } } }, { "Camera", "Far", false, { { 0, { 3000, 0, 0, 0 } } } } };
		std::string error;
		REQUIRE(PropertyAnimation::Sample(entity, animation, .5f, error));
		CHECK(entity.GetComponent<CameraComponent>().Camera.GetPerspectiveNearClip() == 2000);
		CHECK(entity.GetComponent<CameraComponent>().Camera.GetPerspectiveFarClip() == 3000);
		animation.Tracks.push_back({ "Camera", "Near", false, { { 0, { 2500, 0, 0, 0 } } } });
		REQUIRE(ComponentRegistry::AddOrPatch(entity, "PropertyAnimation", { { "Tracks", json::array() } }, error));
		entity.GetComponent<PropertyAnimationComponent>() = animation;
		REQUIRE(PropertyAnimation::Sample(entity, animation, 0, error));
		CHECK(entity.GetComponent<CameraComponent>().Camera.GetPerspectiveNearClip() == 2500);
		REQUIRE(PropertyAnimation::Key(entity, "Camera", "Near", 0, error));
		const auto& tracks = entity.GetComponent<PropertyAnimationComponent>().Tracks;
		CHECK(tracks[0].Keys[0].Value.x == 2000);
		CHECK(tracks[2].Keys[0].Value.x == 2500);
	}
	TEST_CASE("Property playback is isolated per session and reacts to registry patches")
	{
		auto edit = CreateRef<Scene>();
		auto entity = edit->CreateEntity("Platform");
		std::string error;
		REQUIRE(PropertyAnimation::Key(entity, "Transform", "Translation", 0, error));
		entity.GetTransform().Translation.x = 8;
		REQUIRE(PropertyAnimation::Key(entity, "Transform", "Translation", 1, error));
		entity.GetTransform().Translation.x = 0;
		auto play = Scene::Copy(edit);
		auto copy = play->GetEntityByUUID(entity.GetUUID());
		play->OnSimulationStart();
		play->OnUpdate(Timestep(.25f));
		CHECK(copy.GetTransform().Translation.x == 2);
		CHECK(entity.GetTransform().Translation.x == 0);
		play->SetPaused(true);
		play->OnUpdate(Timestep(.25f));
		CHECK(copy.GetTransform().Translation.x == 2);
		play->SetPaused(false);
		REQUIRE(ComponentRegistry::AddOrPatch(copy, "PropertyAnimation", { { "Looping", false }, { "Duration", 2 } }, error));
		play->OnUpdate(Timestep(.5f));
		CHECK(copy.GetTransform().Translation.x == 2);
		play->OnUpdate(Timestep(3));
		CHECK(copy.GetTransform().Translation.x == 8);
		REQUIRE(ComponentRegistry::AddOrPatch(copy, "PropertyAnimation", { { "Looping", true }, { "Duration", 1 } }, error));
		play->OnUpdate(Timestep(1.25f));
		CHECK(copy.GetTransform().Translation.x == 2);
		copy.RemoveComponent<PropertyAnimationComponent>();
		REQUIRE(PropertyAnimation::Key(copy, "Transform", "Translation", 0, error));
		copy.GetTransform().Translation.x = 10;
		REQUIRE(PropertyAnimation::Key(copy, "Transform", "Translation", 1, error));
		play->OnUpdate(Timestep(.5f));
		CHECK(copy.GetTransform().Translation.x == 6);
		play->OnSimulationStop();
		CHECK(entity.GetTransform().Translation.x == 0);
	}
	TEST_CASE("Property playback controls pause resume seek stop and restart")
	{
		Scene scene;
		auto entity = scene.CreateEntity("Controlled");
		std::string error;
		REQUIRE(PropertyAnimation::Key(entity, "Transform", "Translation", 0, error));
		entity.GetTransform().Translation.x = 4;
		REQUIRE(PropertyAnimation::Key(entity, "Transform", "Translation", 1, error));
		entity.GetComponent<PropertyAnimationComponent>().PlayOnStart = false;
		scene.OnSimulationStart();
		auto* system = scene.GetPropertyAnimationSystem();
		REQUIRE(system);
		CHECK(system->Status(entity)["playing"] == false);
		REQUIRE(system->Control(entity, "Play", 0, error));
		scene.OnUpdate(Timestep(.25f));
		CHECK(entity.GetTransform().Translation.x == 1);
		REQUIRE(system->Control(entity, "Pause", 0, error));
		scene.OnUpdate(Timestep(.25f));
		CHECK(entity.GetTransform().Translation.x == 1);
		REQUIRE(system->Control(entity, "Seek", .75f, error));
		CHECK(entity.GetTransform().Translation.x == 3);
		CHECK(system->Status(entity)["playing"] == false);
		REQUIRE(system->Control(entity, "Resume", 0, error));
		scene.OnUpdate(Timestep(.125f));
		CHECK(entity.GetTransform().Translation.x == 3.5f);
		REQUIRE(system->Control(entity, "Stop", 0, error));
		CHECK(entity.GetTransform().Translation.x == 0);
		CHECK(system->Status(entity)["playing"] == false);
		CHECK_FALSE(system->Control(entity, "Typo", 0, error));
		CHECK_FALSE(system->Control(entity, "Seek", 2, error));
		scene.OnSimulationStop();
	}
	TEST_CASE("Animation graph draft preserves references and prunes dependent transitions")
	{
		AnimationGraphDraft draft;
		std::string error;
		auto data = AnimationGraphDraft::Default();
		data["Parameters"] = { { { "Name", "Speed" }, { "Type", "Float" } } };
		data["States"].push_back({ { "Name", "Walk" }, { "BlendSpace", "Move" }, { "ParameterX", "Speed" } });
		data["Transitions"] = { { { "From", "Idle" }, { "To", "Walk" }, { "Conditions", { { { "Parameter", "Speed" }, { "Value", .5f }, { "Operator", "Greater" } } } } } };
		REQUIRE(draft.SetData(data, error));
		draft.MarkSaved();
		CHECK_FALSE(draft.IsDirty());
		REQUIRE(draft.RenameState(0, "Rest"));
		CHECK(draft.Data["InitialState"] == "Rest");
		CHECK(draft.Data["Transitions"][0]["From"] == "Rest");
		CHECK_FALSE(draft.RenameState(0, "Walk"));
		REQUIRE(draft.RenameParameter(0, "Velocity"));
		CHECK(draft.Data["States"][1]["ParameterX"] == "Velocity");
		CHECK(draft.Data["Transitions"][0]["Conditions"][0]["Parameter"] == "Velocity");
		REQUIRE(draft.Validate(error));
		CHECK(draft.IsDirty());
		draft.SetParameterType(0, "Trigger");
		CHECK(draft.Data["Transitions"][0]["Conditions"][0]["Value"] == true);
		CHECK(draft.Data["States"][1]["ParameterX"] == "");
		REQUIRE(draft.Validate(error));
		draft.RemoveParameter(0);
		CHECK(draft.Data["Transitions"].empty());
		CHECK(draft.Data["States"][1]["ParameterX"] == "");
		REQUIRE(draft.Validate(error));
		draft.RemoveState(0);
		CHECK(draft.Data["InitialState"] == "Walk");
		draft.RemoveState(0);
		CHECK(draft.Data["States"].size() == 1);
		const auto before = draft.Data;
		CHECK_FALSE(draft.SetData({ { "States", json::array() } }, error));
		CHECK(draft.Data == before);
	}
	TEST_CASE("Graph drafts follow clean reloads and retain dirty or invalid conflicts")
	{
		TempProject project("GraphDraftReload");
		const std::string path = "Assets/Reload.banimgraph";
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath(path), AnimationGraphDraft::Default().dump()));
		AnimationGraphDraft draft;
		std::string error;
		REQUIRE(draft.Load(path, error));
		REQUIRE(draft.RenameState(0, "Edited"));
		draft.Record();
		REQUIRE(draft.Undo());
		CHECK_FALSE(draft.IsDirty());
		REQUIRE(draft.Redo());
		CHECK(draft.IsDirty());
		REQUIRE(draft.Undo());
		REQUIRE(draft.RenameState(0, "Branched"));
		CHECK_FALSE(draft.Redo());
		CHECK(draft.Data["InitialState"] == "Branched");
		REQUIRE(draft.RenameState(0, "Edited"));
		CHECK_FALSE(draft.OnFileChanged(error));
		CHECK(draft.ExternalChange);
		CHECK(draft.Data["InitialState"] == "Edited");
		REQUIRE(draft.Load(path, error));
		auto changed = AnimationGraphDraft::Default();
		changed["States"][0]["Clip"] = "Bend";
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath(path), changed.dump()));
		REQUIRE(draft.OnFileChanged(error));
		CHECK(draft.Data["States"][0]["Clip"] == "Bend");
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath(path), "{}"));
		CHECK_FALSE(draft.OnFileChanged(error));
		CHECK(draft.ExternalChange);
		CHECK(draft.Data["States"][0]["Clip"] == "Bend");
	}
	TEST_CASE("Animation authoring commands use dirty tracking and validated asset writes")
	{
		TempProject project("AnimationAuthoring");
		AutomationSession session;
		CommandRegistry commands;
		Run(commands, session, "scene.new");
		Run(commands, session, "entity.create", { { "name", "Door" } });
		Run(commands, session, "animation.key", { { "entity", "Door" }, { "component", "Transform" }, { "field", "Translation" }, { "time", 0 } });
		Run(commands, session, "component.set", { { "entity", "Door" }, { "component", "Transform" }, { "data", { { "Translation", { 8, 0, 0 } } } } });
		Run(commands, session, "animation.key", { { "entity", "Door" }, { "component", "Transform" }, { "field", "Translation" }, { "time", 1 } });
		Run(commands, session, "animation.sample", { { "entity", "Door" }, { "time", .5f } });
		CHECK(session.GetScene()->FindEntityByName("Door").GetTransform().Translation.x == 4);
		CHECK(session.IsDirty());
		RunError(commands, session, "animation.key", { { "entity", "Door" }, { "component", "Tag" }, { "field", "Tag" }, { "time", 0 } });
		Run(commands, session, "asset.write", { { "path", "Assets/Test.banimgraph" }, { "content", AnimationGraphDraft::Default().dump() } });
		AnimationGraphDraft draft;
		std::string error;
		REQUIRE(draft.Load("Assets/Test.banimgraph", error));
		CHECK_FALSE(draft.IsDirty());
		RunError(commands, session, "asset.write", { { "path", "Assets/Test.banimgraph" }, { "content", "{}" } });
		REQUIRE(draft.Load("Assets/Test.banimgraph", error));
		CHECK(draft.Data["InitialState"] == "Idle");
	}
}
