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
#include <Basalt/Math/Math.h>
#include <Basalt/Physics/PhysicsWorld.h>

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
	TEST_CASE("Captured quaternion rotation fields take the shortest orientation path")
	{
		for (const std::string component : { "Transform", "BoneAttachment" })
		{
			Scene scene;
			auto entity = scene.CreateEntity("Door");
			std::string error;
			if (component == "BoneAttachment")
				entity.AddComponent<BoneAttachmentComponent>();
			REQUIRE(PropertyAnimation::FindField(component, "Rotation")->QuaternionRotation);
			for (const auto endpoints : std::vector<glm::vec2>{ { 0, 120 }, { 170, -170 } })
			{
				if (entity.HasComponent<PropertyAnimationComponent>())
					entity.RemoveComponent<PropertyAnimationComponent>();
				REQUIRE(ComponentRegistry::AddOrPatch(entity, component, { { "Rotation", { 0, endpoints.x, 0 } } }, error));
				REQUIRE(PropertyAnimation::Key(entity, component, "Rotation", 0, error));
				REQUIRE(ComponentRegistry::AddOrPatch(entity, component, { { "Rotation", { 0, endpoints.y, 0 } } }, error));
				REQUIRE(PropertyAnimation::Key(entity, component, "Rotation", 1, error));
				REQUIRE(PropertyAnimation::Sample(entity, entity.GetComponent<PropertyAnimationComponent>(), .5f, error));
				const auto actual = component == "Transform" ? entity.GetTransform().Rotation : entity.GetComponent<BoneAttachmentComponent>().Rotation;
				const auto expected = Math::QuatFromEuler(glm::radians(glm::vec3(0, endpoints.x == 0 ? 60.0f : 180.0f, 0)));
				CHECK(std::abs(glm::dot(actual, expected)) == doctest::Approx(1).epsilon(.0001));
			}
		}
	}
	TEST_CASE("Capture ignores empty duplicate tracks and physics configuration is not animatable")
	{
		Scene scene;
		auto entity = scene.CreateEntity();
		std::string error;
		REQUIRE(PropertyAnimation::Key(entity, "Transform", "Translation", 0, error));
		entity.GetTransform().Translation.x = 8;
		REQUIRE(PropertyAnimation::Key(entity, "Transform", "Translation", 1, error));
		entity.GetComponent<PropertyAnimationComponent>().Tracks.push_back({});
		entity.GetTransform().Translation.x = 3;
		REQUIRE(PropertyAnimation::Key(entity, "Transform", "Translation", .1f, error));
		const auto& animation = entity.GetComponent<PropertyAnimationComponent>();
		CHECK(animation.Tracks[0].Keys.size() == 3);
		CHECK(animation.Tracks[1].Keys.empty());
		REQUIRE(PropertyAnimation::Sample(entity, animation, 0, error));
		CHECK(entity.GetTransform().Translation.x == 0);
		for (const auto& info : ComponentRegistry::GetAll())
			if (!info.SupportsPropertyAnimation)
				for (const auto& field : info.Fields)
					CHECK(PropertyAnimation::FindField(info.Name, field) == nullptr);
		entity.AddComponent<BoxColliderComponent>();
		CHECK_FALSE(PropertyAnimation::Key(entity, "BoxCollider", "HalfExtents", 0, error));
		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "PropertyAnimation", { { "Tracks", json::array({ { { "Component", "RigidBody" }, { "Field", "Mass" } } }) } }, error));
	}
	TEST_CASE("Failed timelines log once suspend and require a successful repair")
	{
		Scene scene;
		auto entity = scene.CreateEntity("FailureProbe");
		std::string error;
		entity.AddComponent<PointLightComponent>();
		REQUIRE(PropertyAnimation::Key(entity, "PointLight", "Intensity", 0, error));
		REQUIRE(PropertyAnimation::Key(entity, "Transform", "Translation", 0, error));
		entity.GetTransform().Translation.x = 8;
		REQUIRE(PropertyAnimation::Key(entity, "Transform", "Translation", 1, error));
		scene.OnSimulationStart();
		auto* system = scene.GetPropertyAnimationSystem();
		scene.OnUpdate(Timestep(.1f));
		entity.RemoveComponent<PointLightComponent>();
		const auto since = Log::GetHistory().GetTotalCount();
		for (int i = 0; i < 4; i++)
			scene.OnUpdate(Timestep(.1f));
		CHECK(CountLogMessages(since, "Property animation 'FailureProbe'") == 1);
		CHECK(system->Status(entity)["failed"] == true);
		CHECK(system->Status(entity)["playing"] == false);
		CHECK_FALSE(system->Status(entity)["error"].get<std::string>().empty());
		CHECK_FALSE(system->Control(entity, "Pause", 0, error));
		CHECK_FALSE(system->Control(entity, "Resume", 0, error));
		for (const std::string action : { "Play", "Stop", "Seek" })
			CHECK_FALSE(system->Control(entity, action, .5f, error));
		CHECK(system->Status(entity)["failed"] == true);
		entity.AddComponent<PointLightComponent>();
		const auto position = entity.GetTransform().Translation;
		scene.OnUpdate(Timestep(.1f));
		CHECK(entity.GetTransform().Translation == position);
		for (const std::string action : { "Play", "Stop", "Seek" })
		{
			REQUIRE(system->Control(entity, action, .5f, error));
			CHECK(system->Status(entity)["failed"] == false);
			CHECK(system->Status(entity)["error"] == "");
			REQUIRE(system->Control(entity, "Play", 0, error));
			entity.RemoveComponent<PointLightComponent>();
			scene.OnUpdate(Timestep(.1f));
			entity.AddComponent<PointLightComponent>();
		}
		REQUIRE(ComponentRegistry::AddOrPatch(entity, "PropertyAnimation", { { "Speed", 2 } }, error));
		CHECK(system->Status(entity)["failed"] == false);
		scene.OnUpdate(Timestep(.1f));
		CHECK(entity.GetTransform().Translation.x == doctest::Approx(1.6f));
		scene.OnSimulationStop();
	}
	TEST_CASE("Reusing a destroyed entity UUID starts a fresh property clock")
	{
		Scene scene;
		auto entity = scene.CreateEntity("Old");
		const auto id = entity.GetUUID();
		std::string error;
		REQUIRE(PropertyAnimation::Key(entity, "Transform", "Translation", 0, error));
		entity.GetTransform().Translation.x = 8;
		REQUIRE(PropertyAnimation::Key(entity, "Transform", "Translation", 1, error));
		const auto authored = entity.GetComponent<PropertyAnimationComponent>(); // NOLINT(performance-unnecessary-copy-initialization) -- Own the tracks across DestroyEntity below.
		scene.OnSimulationStart();
		scene.OnUpdate(Timestep(.5f));
		scene.DestroyEntity(entity);
		auto replacement = scene.CreateEntityWithUUID(id, "New");
		replacement.AddComponent<PropertyAnimationComponent>(authored);
		scene.OnUpdate(Timestep(.25f));
		CHECK(replacement.GetTransform().Translation.x == 2);
		scene.OnSimulationStop();
	}
	TEST_CASE("Play property sampling follows Lua Update and precedes the kinematic physics step")
	{
		TempProject project("PropertyOrdering");
		const auto script = project.WriteFile("Assets/Ordering.lua", R"(
			return {OnUpdate=function(self) self.Entity:SetComponent("Transform",{Translation={99,0,0}}) end}
		)");
		Scene scene;
		auto entity = scene.CreateEntity("Mover");
		std::string error;
		entity.AddComponent<ScriptComponent>().Script = script;
		entity.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		entity.AddComponent<BoxColliderComponent>();
		REQUIRE(PropertyAnimation::Key(entity, "Transform", "Translation", 0, error));
		entity.GetTransform().Translation.x = 8;
		REQUIRE(PropertyAnimation::Key(entity, "Transform", "Translation", 1, error));
		entity.GetTransform().Translation.x = 0;
		scene.OnRuntimeStart();
		scene.OnUpdate(Timestep(.25f));
		CHECK(entity.GetTransform().Translation.x == 2);
		const auto hit = scene.GetPhysicsWorld()->Raycast({ 2, 3, 0 }, { 0, -1, 0 }, 5);
		REQUIRE(hit);
		CHECK(hit->EntityID == entity.GetUUID());
		CHECK(scene.GetScriptEngine()->GetErrors().empty());
		scene.OnRuntimeStop();
	}
	TEST_CASE("State deletion prunes incoming outgoing and any-state target edges")
	{
		AnimationGraphDraft draft;
		std::string error;
		auto data = AnimationGraphDraft::Default();
		data["States"].push_back({ { "Name", "Walk" }, { "Clip", "Walk" } });
		data["States"].push_back({ { "Name", "Run" }, { "Clip", "Run" } });
		data["Transitions"] = json::array({ { { "From", "Idle" }, { "To", "Walk" } }, { { "From", "Walk" }, { "To", "Run" } }, { { "To", "Walk" } }, { { "From", "Idle" }, { "To", "Run" } } });
		REQUIRE(draft.SetData(data, error));
		REQUIRE(draft.RenameState(1, "Move"));
		CHECK(draft.Data["Transitions"][0]["To"] == "Move");
		draft.RemoveState(1);
		REQUIRE(draft.Data["Transitions"].size() == 1);
		CHECK(draft.Data["Transitions"][0]["From"] == "Idle");
		CHECK(draft.Data["Transitions"][0]["To"] == "Run");
		REQUIRE(draft.Validate(error));
	}
	TEST_CASE("Property validation rejects malformed bounded tracks without changing authored data")
	{
		Scene scene;
		auto entity = scene.CreateEntity();
		std::string error;
		REQUIRE(PropertyAnimation::Key(entity, "Transform", "Translation", 0, error));
		const auto* info = ComponentRegistry::Find("PropertyAnimation");
		const auto baseline = info->Serialize(entity);
		const json track = { { "Component", "Transform" }, { "Field", "Translation" } };
		std::vector<json> invalid = { { { "Duration", 86401 } }, { { "Speed", 1001 } }, { { "Tracks", json::array({ { { "Component", "PropertyAnimation" }, { "Field", "Duration" } } }) } }, { { "Tracks", json::array({ { { "Component", "Mesh" }, { "Field", "MeshIndex" } } }) } }, { { "Tracks", json::array({ { { "Component", "Mesh" }, { "Field", "CastShadows" } } }) } }, { { "Tracks", json::array({ { { "Component", "Material" }, { "Field", "AlphaMode" } } }) } } };
		for (const auto times : std::vector<glm::vec2>{ { .8f, .2f }, { -.1f, .2f }, { 0, 1.1f } })
		{
			auto bad = track;
			bad["Keys"] = json::array({ { { "Time", times.x } }, { { "Time", times.y } } });
			invalid.push_back({ { "Tracks", json::array({ bad }) } });
		}
		auto badValue = track;
		badValue["Keys"] = json::array({ { { "Value", { 1, 2, 3 } } } });
		invalid.push_back({ { "Tracks", json::array({ badValue }) } });
		invalid.push_back({ { "Tracks", std::vector<json>(65, track) } });
		auto tooMany = track;
		tooMany["Keys"] = std::vector<json>(257, json::object());
		invalid.push_back({ { "Tracks", json::array({ tooMany }) } });
		for (const auto& bad : invalid)
		{
			CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "PropertyAnimation", bad, error));
			CHECK(info->Serialize(entity) == baseline);
		}
	}
	TEST_CASE("Numeric vector samples retain sibling data and held values emit no repeated updates")
	{
		Scene scene;
		auto entity = scene.CreateEntity();
		std::string error;
		auto& rect = entity.AddComponent<UIRectComponent>();
		rect.Position = { 2, 4 };
		rect.Size = { 7, 9 };
		entity.AddComponent<MaterialComponent>().AlbedoColor = { 0, .2f, .4f, .6f };
		for (const auto& target : std::vector<std::pair<std::string, std::string>>{ { "UIRect", "Position" }, { "Material", "AlbedoColor" } })
			REQUIRE(PropertyAnimation::Key(entity, target.first, target.second, 0, error));
		rect.Position = { 8, 10 };
		entity.GetComponent<MaterialComponent>().AlbedoColor = { 1, .4f, .6f, .8f };
		for (const auto& target : std::vector<std::pair<std::string, std::string>>{ { "UIRect", "Position" }, { "Material", "AlbedoColor" } })
			REQUIRE(PropertyAnimation::Key(entity, target.first, target.second, 1, error));
		struct Updates
		{
			int Count = 0;
			void OnUpdate(entt::registry&, entt::entity) { Count++; }
		} updates;
		scene.GetRegistry().on_update<UIRectComponent>().connect<&Updates::OnUpdate>(updates);
		REQUIRE(PropertyAnimation::Sample(entity, entity.GetComponent<PropertyAnimationComponent>(), .1f, error));
		const auto value = entity.GetComponent<UIRectComponent>();
		REQUIRE(PropertyAnimation::Sample(entity, entity.GetComponent<PropertyAnimationComponent>(), .1f, error));
		CHECK(updates.Count == 1);
		CHECK(entity.GetComponent<UIRectComponent>().Position == value.Position);
		CHECK(value.Size == glm::vec2(7, 9));
		CHECK(entity.GetComponent<MaterialComponent>().AlbedoColor.x == doctest::Approx(.1f));
		scene.GetRegistry().on_update<UIRectComponent>().disconnect(&updates);
	}
	TEST_CASE("Sampling reports partial failure with one component prefix and keeps later fields unchanged")
	{
		Scene scene;
		auto entity = scene.CreateEntity();
		entity.AddComponent<MaterialComponent>();
		entity.AddComponent<PointLightComponent>();
		PropertyAnimationComponent animation;
		animation.Tracks = { { "Material", "Roughness", false, { { 0, { .3f, 0, 0, 0 } } } }, { "PointLight", "Radius", false, { { 0, { -1, 0, 0, 0 } } } }, { "Transform", "Translation", false, { { 0, { 8, 0, 0, 0 } } } } };
		std::string error;
		CHECK_FALSE(PropertyAnimation::Sample(entity, animation, 0, error));
		CHECK(entity.GetComponent<MaterialComponent>().Roughness == doctest::Approx(.3f));
		CHECK(entity.GetTransform().Translation.x == 0);
		CHECK(error.starts_with("PointLight:"));
		CHECK(error.find("PointLight: PointLight:") == std::string::npos);
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
