#include <doctest/doctest.h>

#include <Basalt/Core/Log.h>
#include <Basalt/Particles/ParticleSystem.h>
#include <Basalt/Physics/PhysicsWorld.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/JointFields.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>
#include <Basalt/Scene/SceneDiff.h>
#include <Basalt/Scene/SceneSerializer.h>
#include <Basalt/Scripting/ScriptEngine.h>

#include "TestUtils.h"

#include <glm/gtc/epsilon.hpp>

#include <functional>
#include <map>
#include <set>

using namespace Basalt;

namespace {

	// Whether a serialized value has a field kind's JSON type (enum values: one of the options).
	bool MatchesFieldKind(const nlohmann::json& value, ComponentFieldKind kind, const std::vector<std::string>* enumOptions)
	{
		switch (kind)
		{
			case ComponentFieldKind::Number:
				return value.is_number();
			case ComponentFieldKind::Integer:
				return value.is_number_integer();
			case ComponentFieldKind::Boolean:
				return value.is_boolean();
			case ComponentFieldKind::String:
				return value.is_string();
			case ComponentFieldKind::Object:
				return value.is_object();
			case ComponentFieldKind::Entity:
				return value.is_number_unsigned();
			case ComponentFieldKind::Enum:
				return value.is_string() && enumOptions && std::ranges::find(*enumOptions, value.get<std::string>()) != enumOptions->end();
		}
		return false;
	}

}

namespace {

	bool Near(const glm::vec3& a, const glm::vec3& b, float epsilon = 1e-4f)
	{
		return glm::all(glm::epsilonEqual(a, b, epsilon));
	}

	// Structural JSON equality with a tolerance for floating-point numbers.
	bool JsonNear(const nlohmann::json& a, const nlohmann::json& b, double epsilon = 1e-4)
	{
		if (a.is_number() && b.is_number())
			return std::abs(a.get<double>() - b.get<double>()) <= epsilon;
		if (a.type() != b.type() || a.size() != b.size())
			return false;
		if (a.is_array())
		{
			for (size_t i = 0; i < a.size(); i++)
			{
				if (!JsonNear(a[i], b[i], epsilon))
					return false;
			}
			return true;
		}
		if (a.is_object())
		{
			for (auto it = a.begin(); it != a.end(); ++it)
			{
				if (!b.contains(it.key()) || !JsonNear(it.value(), b[it.key()], epsilon))
					return false;
			}
			return true;
		}
		return a == b;
	}

}

TEST_SUITE("Scene")
{
	TEST_CASE("Runtime UUIDs repeat across copies and simulation, skip collisions and ignore gameplay draws")
	{
		BasaltTest::TempProject project("RuntimeUUIDs");
		Project::GetActive()->GetConfig().RandomSeed = 1234;
		Ref<Scene> edit = CreateRef<Scene>();
		Entity root = edit->CreateEntityWithUUID(42, "Root");
		edit->SetParent(edit->CreateEntityWithUUID(43, "Child"), root, false);
		std::string error;
		const auto path = project.GetDirectory() / "Assets/Prefabs/IDs.bprefab";
		REQUIRE(SceneSerializer::SavePrefab(root, path, error));
		auto run = [&](bool simulate, bool draw) {
			Ref<Scene> scene = Scene::Copy(edit);
			if (simulate)
				scene->OnSimulationStart();
			else
				scene->OnRuntimeStart();
			if (draw)
				CHECK(BasaltTest::Eval(*scene, "for i = 1, 100 do math.random() end Math.Seed(99)") == "");
			std::vector<uint64_t> ids;
			Entity spawned = scene->CreateEntity("Spawned");
			ids.push_back(spawned.GetUUID());
			scene->DestroyEntity(spawned);
			ids.push_back(scene->CreateEntityWithUUID(42, "Duplicate").GetUUID());
			ids.push_back(scene->CreateEntityWithUUID(0, "Invalid").GetUUID());
			Entity prefab = SceneSerializer::InstantiatePrefab(*scene, path, "Assets/Prefabs/IDs.bprefab", {}, error);
			REQUIRE_MESSAGE(prefab, error);
			ids.push_back(prefab.GetUUID());
			ids.push_back(prefab.GetChildren()[0].GetUUID());
			Entity copy = scene->DuplicateEntity(prefab);
			ids.push_back(copy.GetUUID());
			ids.push_back(copy.GetChildren()[0].GetUUID());
			CHECK(std::set<uint64_t>(ids.begin(), ids.end()).size() == ids.size());
			CHECK(scene->GetEntityByUUID(42).GetName() == "Root");
			if (simulate)
				scene->OnSimulationStop();
			else
				scene->OnRuntimeStop();
			return ids;
		};
		const auto baseline = run(false, false);
		// Pins seed 1234, Random's mt19937 algorithm, and the high-then-low 32-bit draw layout.
		CHECK(baseline[0] == 3532910284440527571ULL);
		CHECK(run(false, true) == baseline);
		CHECK(run(true, false) == baseline);
		// An authored ID equal to the next draw stays reserved after its entity is destroyed.
		Scene collision;
		collision.CreateEntityWithUUID(baseline[0], "Reserved");
		collision.OnSimulationStart();
		collision.DestroyEntity(collision.GetEntityByUUID(baseline[0]));
		CHECK(static_cast<uint64_t>(collision.CreateEntity().GetUUID()) == baseline[1]);
		collision.OnSimulationStop();
		Scene retired;
		retired.OnSimulationStart();
		CHECK(static_cast<uint64_t>(retired.CreateEntity().GetUUID()) == baseline[0]);
		retired.DestroyEntity(retired.CreateEntityWithUUID(baseline[1]));
		CHECK(static_cast<uint64_t>(retired.CreateEntity().GetUUID()) == baseline[2]);
		retired.OnSimulationStop();
		Project::GetActive()->GetConfig().RandomSeed = 5678;
		CHECK(run(false, false) != baseline);
	}

	TEST_CASE("Runtime UUID stream continues across Scene.Load")
	{
		BasaltTest::TempProject project("RuntimeUUIDLoad");
		project.WriteFile("Assets/Scenes/Empty.bscene", R"({"Name":"Empty","Entities":[]})");
		std::vector<uint64_t> baseline;
		{
			Scene scene;
			scene.OnRuntimeStart();
			for (int i = 0; i < 3; ++i)
				baseline.push_back(scene.CreateEntity().GetUUID());
			scene.OnRuntimeStop();
		}
		Scene scene;
		scene.OnRuntimeStart();
		Entity first = scene.CreateEntity("First");
		CHECK(static_cast<uint64_t>(first.GetUUID()) == baseline[0]);
		CHECK(BasaltTest::Eval(scene, "Scene.Load('Assets/Scenes/Empty.bscene')") == "");
		scene.OnUpdate(BasaltTest::Step);
		CHECK_FALSE(scene.GetEntityByUUID(baseline[0]));
		// Editing the project seed during play takes effect only on the next session.
		Project::GetActive()->GetConfig().RandomSeed++;
		CHECK(static_cast<uint64_t>(scene.CreateEntity().GetUUID()) == baseline[1]);
		CHECK(static_cast<uint64_t>(scene.CreateEntity().GetUUID()) == baseline[2]);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Runtime identity starts before OnCreate and resets when the same scene restarts")
	{
		BasaltTest::TempProject project("RuntimeIdentityRestart");
		Project::GetActive()->GetConfig().RandomSeed = 1234;
		const std::string script = project.WriteFile("Assets/Scripts/Spawn.lua", R"(
			return { OnCreate = function(self) Scene.CreateEntity("CreatedAtStart") end }
		)");
		Scene scene;
		scene.CreateEntityWithUUID(42, "Spawner").AddComponent<ScriptComponent>().Script = script;
		for (int run = 0; run < 2; ++run)
		{
			scene.OnRuntimeStart();
			Entity spawned = scene.FindEntityByName("CreatedAtStart");
			REQUIRE(spawned);
			CHECK(static_cast<uint64_t>(spawned.GetUUID()) == 3532910284440527571ULL);
			scene.DestroyEntity(spawned);
			scene.OnRuntimeStop();
		}
		for (int run = 0; run < 2; ++run)
		{
			scene.OnSimulationStart();
			Entity spawned = scene.CreateEntity();
			CHECK(static_cast<uint64_t>(spawned.GetUUID()) == 3532910284440527571ULL);
			scene.DestroyEntity(spawned);
			scene.OnSimulationStop();
		}
		Scene editA;
		Scene editB;
		CHECK(editA.CreateEntity().GetUUID() != editB.CreateEntity().GetUUID());
	}

	TEST_CASE("Scene loads reserve every saved ID before generating omitted or zero IDs")
	{
		BasaltTest::TempProject project("LoadedIDReservation");
		Project::GetActive()->GetConfig().RandomSeed = 1234;
		constexpr uint64_t FirstDraw = 3532910284440527571ULL;
		for (bool omitted : { false, true })
		{
			auto data = nlohmann::json::parse(R"({"Entities":[
				{"ID":0,"Name":"Generated","Components":{"Joint":{"ConnectedEntity":3532910284440527571}}},
				{"ID":3532910284440527571,"Name":"Saved"},
				{"ID":42,"Name":"Child","Parent":3532910284440527571}]})");
			if (omitted)
				data["Entities"][0].erase("ID");
			project.WriteFile("Assets/Scenes/Reservation.bscene", data.dump());
			Scene scene;
			scene.OnRuntimeStart();
			CHECK(BasaltTest::Eval(scene, "Scene.Load('Assets/Scenes/Reservation.bscene')") == "");
			scene.OnUpdate(BasaltTest::Step);
			Entity saved = scene.FindEntityByName("Saved");
			Entity generated = scene.FindEntityByName("Generated");
			REQUIRE(saved);
			REQUIRE(generated);
			CHECK(static_cast<uint64_t>(saved.GetUUID()) == FirstDraw);
			CHECK(generated.GetUUID() != saved.GetUUID());
			CHECK(scene.GetEntityByUUID(generated.GetComponent<JointComponent>().ConnectedEntity) == saved);
			CHECK(scene.FindEntityByName("Child").GetParent() == saved);
			scene.OnRuntimeStop();
		}

		// Prefab source IDs must not reserve draws: they are remapped together with their references.
		Scene prefabScene;
		prefabScene.OnRuntimeStart();
		const auto entities = nlohmann::json::parse(R"([
			{"ID":3532910284440527571,"Name":"Root"},
			{"ID":42,"Name":"Child","Parent":3532910284440527571,"Components":{"Joint":{"ConnectedEntity":3532910284440527571}}}])");
		std::string error;
		Entity root = SceneSerializer::DeserializeEntityTree(prefabScene, entities, {}, error);
		REQUIRE_MESSAGE(root, error);
		CHECK(static_cast<uint64_t>(root.GetUUID()) == FirstDraw);
		REQUIRE(root.GetChildren().size() == 1);
		CHECK(root.GetChildren()[0].GetComponent<JointComponent>().ConnectedEntity == root.GetUUID());
		prefabScene.OnRuntimeStop();
	}

	TEST_CASE("Generated prefab IDs never overwrite source-ID remapping keys")
	{
		BasaltTest::TempProject project("PrefabSourceIDReservation");
		Project::GetActive()->GetConfig().RandomSeed = 1234;
		for (bool omitted : { false, true })
		{
			// The root's source ID equals the second stream draw, which the anonymous child receives.
			// Only explicit source IDs identify serialized reference targets; generated IDs are values.
			auto entities = nlohmann::json::parse(R"([
				{"ID":11475881202261285941,"Name":"Root"},
				{"ID":0,"Name":"Child","Parent":11475881202261285941,"Components":{"Joint":{"ConnectedEntity":11475881202261285941}}}])");
			if (omitted)
				entities[1].erase("ID");
			Scene scene;
			scene.OnRuntimeStart();
			std::string error;
			Entity root = SceneSerializer::DeserializeEntityTree(scene, entities, {}, error);
			REQUIRE_MESSAGE(root, error);
			REQUIRE(root.GetChildren().size() == 1);
			Entity child = root.GetChildren()[0];
			CHECK(static_cast<uint64_t>(root.GetUUID()) == 3532910284440527571ULL);
			CHECK(static_cast<uint64_t>(child.GetUUID()) == 11475881202261285941ULL);
			CHECK(child.GetComponent<JointComponent>().ConnectedEntity == root.GetUUID());
			scene.OnRuntimeStop();
		}
	}

	TEST_CASE("Duplicate source IDs are rejected before creating scene or prefab entities")
	{
		BasaltTest::TempProject project("DuplicateSourceIDs");
		const auto data = nlohmann::json::parse(R"({"Entities":[{"ID":42,"Name":"A"},{"ID":42,"Name":"B"}]})");
		Scene scene;
		std::string error;
		CHECK_FALSE(SceneSerializer::DeserializeScene(scene, data, error));
		CHECK(error.find("duplicate ID 42") != std::string::npos);
		CHECK(scene.GetEntityCount() == 0);
		project.WriteFile("Assets/Prefabs/Duplicate.bprefab", data.dump());
		scene.OnRuntimeStart();
		CHECK_FALSE(SceneSerializer::InstantiatePrefab(scene, project.GetDirectory() / "Assets/Prefabs/Duplicate.bprefab", "Assets/Prefabs/Duplicate.bprefab", {}, error));
		CHECK(error.find("duplicate ID 42") != std::string::npos);
		CHECK(scene.GetEntityCount() == 0);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Entities have core components and are found by UUID and name")
	{
		Scene scene;
		Entity entity = scene.CreateEntity("Player");
		REQUIRE(entity);
		CHECK(entity.HasComponent<IDComponent>());
		CHECK(entity.HasComponent<TagComponent>());
		CHECK(entity.HasComponent<TransformComponent>());
		CHECK(entity.GetName() == "Player");
		CHECK(scene.GetEntityByUUID(entity.GetUUID()) == entity);
		CHECK(scene.FindEntityByName("Player") == entity);
		CHECK_FALSE(scene.FindEntityByName("Nobody"));
		CHECK(scene.GetEntityCount() == 1);

		Entity fixed = scene.CreateEntityWithUUID(1234, "Fixed");
		CHECK(static_cast<uint64_t>(fixed.GetUUID()) == 1234);
		// Duplicate UUIDs are replaced rather than corrupting the map.
		Entity duplicate = scene.CreateEntityWithUUID(1234, "Duplicate");
		CHECK(static_cast<uint64_t>(duplicate.GetUUID()) != 1234);
		CHECK(scene.GetEntityCount() == 3);
	}

	TEST_CASE("Hierarchy: parenting, world transforms, cycles and recursive destruction")
	{
		Scene scene;
		Entity parent = scene.CreateEntity("Parent");
		Entity child = scene.CreateEntity("Child");
		Entity grandchild = scene.CreateEntity("Grandchild");

		parent.GetTransform().Translation = { 10.0f, 0.0f, 0.0f };
		child.GetTransform().Translation = { 0.0f, 5.0f, 0.0f };

		REQUIRE(scene.SetParent(child, parent, false));
		REQUIRE(scene.SetParent(grandchild, child, false));
		CHECK(child.GetParent() == parent);
		CHECK(parent.GetChildren().size() == 1);
		CHECK(scene.GetRootEntities().size() == 1);
		CHECK(Near(glm::vec3(scene.GetWorldTransform(child)[3]), { 10.0f, 5.0f, 0.0f }));

		// Cycles are rejected.
		CHECK_FALSE(scene.SetParent(parent, grandchild));
		CHECK_FALSE(scene.SetParent(parent, parent));

		// keepWorldTransform preserves the world position.
		REQUIRE(scene.SetParent(child, Entity{}, true));
		CHECK(Near(child.GetTransform().Translation, { 10.0f, 5.0f, 0.0f }));
		REQUIRE(scene.SetParent(child, parent, true));
		CHECK(Near(child.GetTransform().Translation, { 0.0f, 5.0f, 0.0f }));

		scene.SetWorldTransform(grandchild, Math::ComposeTransform({ 1.0f, 2.0f, 3.0f }, glm::quat(1, 0, 0, 0), glm::vec3(1.0f)));
		CHECK(Near(grandchild.GetTransform().Translation, { -9.0f, -3.0f, 3.0f }));

		const std::vector<Entity> ordered = scene.GetAllEntitiesOrdered();
		REQUIRE(ordered.size() == 3);
		CHECK(ordered[0] == parent);
		CHECK(ordered[2] == grandchild);

		scene.DestroyEntity(parent);
		CHECK(scene.GetEntityCount() == 0);
		CHECK_FALSE(child.IsValid());
	}

	TEST_CASE("DuplicateEntity deep-copies with new UUIDs")
	{
		Scene scene;
		Entity root = scene.CreateEntity("Root");
		root.AddComponent<PointLightComponent>().Intensity = 42.0f;
		Entity child = scene.CreateChildEntity(root, "Child");
		child.AddComponent<MeshComponent>().Mesh = "builtin://Cube";

		Entity copy = scene.DuplicateEntity(root);
		REQUIRE(copy);
		CHECK(copy != root);
		CHECK(copy.GetUUID() != root.GetUUID());
		CHECK(copy.GetComponent<PointLightComponent>().Intensity == 42.0f);
		REQUIRE(copy.GetChildren().size() == 1);
		Entity childCopy = copy.GetChildren()[0];
		CHECK(childCopy.GetUUID() != child.GetUUID());
		CHECK(childCopy.GetComponent<MeshComponent>().Mesh == "builtin://Cube");
		CHECK(root.GetChildren().size() == 1);
		CHECK(scene.GetEntityCount() == 4);
	}

	TEST_CASE("Scene::Copy preserves UUIDs, hierarchy, components and settings")
	{
		Ref<Scene> scene = CreateRef<Scene>("Original");
		Entity root = scene->CreateEntity("Root");
		Entity child = scene->CreateChildEntity(root, "Child");
		child.AddComponent<RigidBodyComponent>().Mass = 7.0f;
		scene->GetRendererSettings().Exposure = 2.5f;

		Ref<Scene> copy = Scene::Copy(scene);
		Entity copiedChild = copy->GetEntityByUUID(child.GetUUID());
		REQUIRE(copiedChild);
		CHECK(copiedChild.GetComponent<RigidBodyComponent>().Mass == 7.0f);
		CHECK(copiedChild.GetParent().GetUUID() == root.GetUUID());
		CHECK(copy->GetRendererSettings().Exposure == 2.5f);

		// Independent storage.
		copiedChild.GetComponent<RigidBodyComponent>().Mass = 1.0f;
		CHECK(child.GetComponent<RigidBodyComponent>().Mass == 7.0f);
	}

	TEST_CASE("Destroying an entity outside of an update is immediate")
	{
		Scene scene;
		scene.OnRuntimeStart();
		Entity entity = scene.CreateEntity("Doomed");
		scene.DestroyEntity(entity);
		// Not updating: destroyed immediately.
		CHECK_FALSE(entity.IsValid());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Primary camera and viewport resize")
	{
		Scene scene;
		Entity camera = scene.CreateEntity("Camera");
		camera.AddComponent<CameraComponent>();
		Entity fixed = scene.CreateEntity("Fixed");
		auto& fixedCamera = fixed.AddComponent<CameraComponent>();
		fixedCamera.Primary = false;
		fixedCamera.FixedAspectRatio = true;

		CHECK(scene.GetPrimaryCameraEntity() == camera);
		scene.OnViewportResize(800, 400);
		CHECK(camera.GetComponent<CameraComponent>().Camera.GetAspectRatio() == doctest::Approx(2.0f));
		CHECK(fixedCamera.Camera.GetAspectRatio() == doctest::Approx(16.0f / 9.0f));
	}
}

TEST_SUITE("Serialization")
{
	TEST_CASE("Every component's defaults pass its own rules; adding through the registry constructs once")
	{
		Scene scene;
		for (const ComponentInfo& info : ComponentRegistry::GetAll())
		{
			if (info.IsCore)
				continue;
			INFO("component: " << info.Name);
			Entity entity = scene.CreateEntity(info.Name);
			std::string error;
			CHECK_MESSAGE(info.Deserialize(entity, nlohmann::json::object(), error), error);
			CHECK(info.Has(entity));
		}

		// One construction with the final values, and no update: systems never see the defaults first.
		struct Counter
		{
			int Constructed = 0;
			int Updated = 0;
			float Intensity = 0.0f;
			void OnConstruct(entt::registry& registry, entt::entity handle)
			{
				Constructed++;
				Intensity = registry.get<PointLightComponent>(handle).Intensity;
			}
			void OnUpdate(entt::registry&, entt::entity) { Updated++; }
		} counter;
		scene.GetRegistry().on_construct<PointLightComponent>().connect<&Counter::OnConstruct>(counter);
		scene.GetRegistry().on_update<PointLightComponent>().connect<&Counter::OnUpdate>(counter);
		Entity lamp = scene.CreateEntity("Lamp");
		std::string error;
		REQUIRE(ComponentRegistry::AddOrPatch(lamp, "PointLight", { { "Intensity", 42 } }, error));
		CHECK(counter.Constructed == 1);
		CHECK(counter.Updated == 0);
		CHECK(counter.Intensity == 42.0f);
		// A rejected add leaves no component behind.
		Entity broken = scene.CreateEntity("Broken");
		CHECK_FALSE(ComponentRegistry::AddOrPatch(broken, "PointLight", { { "Intensity", "bright" } }, error));
		CHECK_FALSE(broken.HasComponent<PointLightComponent>());
		CHECK(counter.Constructed == 1);
		scene.GetRegistry().on_construct<PointLightComponent>().disconnect<&Counter::OnConstruct>(counter);
		scene.GetRegistry().on_update<PointLightComponent>().disconnect<&Counter::OnUpdate>(counter);
	}

	TEST_CASE("Every registered component round-trips through JSON")
	{
		Scene scene;
		Entity entity = scene.CreateEntity("Everything");
		for (const ComponentInfo& info : ComponentRegistry::GetAll())
			info.Add(entity);

		auto& transform = entity.GetTransform();
		transform.Translation = { 1.0f, 2.0f, 3.0f };
		transform.SetRotationEuler(glm::radians(glm::vec3(10.0f, 20.0f, 30.0f)));
		transform.Scale = { 2.0f, 2.0f, 2.0f };
		entity.GetComponent<CameraComponent>().Camera.SetPerspective(glm::radians(70.0f), 0.5f, 500.0f);
		entity.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		entity.GetComponent<RigidBodyComponent>().Layer = "Debris";
		entity.GetComponent<RigidBodyComponent>().Continuous = true;
		entity.GetComponent<RigidBodyComponent>().AllowSleeping = false;
		entity.GetComponent<MeshColliderComponent>() = { .Mesh = "Assets/Models/Level.glb", .MeshIndex = 2, .Convex = true, .OverrideMaterial = true, .Friction = 0.25f, .Restitution = 0.75f };
		entity.GetComponent<BoxColliderComponent>() = { .HalfExtents = { 1.0f, 2.0f, 3.0f }, .OverrideMaterial = true, .Friction = 0.125f, .Restitution = 0.5f };
		entity.GetComponent<SphereColliderComponent>().Friction = 0.9f;
		entity.GetComponent<CapsuleColliderComponent>().Restitution = 0.3f;
		entity.GetComponent<AnimatorComponent>() = { "Assets/Models/Fox.glb", "Run", 2.5f, false, false, false };
		entity.GetComponent<MaterialComponent>().AlbedoColor = { 0.1f, 0.2f, 0.3f, 0.4f };
		entity.GetComponent<ParticleEmitterComponent>() = { .PlayOnStart = false,
															.Looping = false,
															.Duration = 2.5f,
															.MaxParticles = 77,
															.Rate = 12.5f,
															.BurstCount = 9,
															.Shape = ParticleEmitterShape::Cone,
															.ShapeRadius = 0.75f,
															.BoxExtents = { 1.0f, 2.0f, 3.0f },
															.ConeAngle = 40.0f,
															.LifetimeMin = 0.5f,
															.LifetimeMax = 1.5f,
															.SpeedMin = -1.0f,
															.SpeedMax = 4.0f,
															.Velocity = { 0.0f, 1.0f, -2.0f },
															.GravityScale = 0.25f,
															.Drag = 0.5f,
															.SimulationSpace = ParticleSimulationSpace::Local,
															.StartColor = { 4.0f, 2.0f, 1.0f, 1.0f },
															.EndColor = { 0.5f, 0.0f, 0.0f, 0.0f },
															.StartSize = 0.1f,
															.EndSize = 0.6f,
															.Blend = ParticleBlendMode::Additive,
															.Texture = "Assets/Textures/Spark.png" };
		entity.GetComponent<UIRectComponent>() = { .Visible = false, .AnchorMin = { 0.0f, 0.25f }, .AnchorMax = { 1.0f, 0.75f }, .Pivot = { 0.0f, 1.0f }, .Position = { 12.5f, -4.0f }, .Size = { -20.0f, 64.0f } };
		entity.GetComponent<UIImageComponent>() = { .Texture = "Assets/Textures/Panel.png", .Color = { 0.5f, 0.25f, 1.0f, 0.75f }, .Slice = { 1.0f, 2.0f, 3.0f, 4.0f }, .SliceScale = 2.5f };
		entity.GetComponent<UITextComponent>() = { .Text = "Score: 10\nLives: 3", .Font = "Assets/Fonts/Karla-Regular.ttf", .FontSize = 48.0f, .Color = { 1.0f, 0.5f, 0.0f, 0.5f }, .HorizontalAlign = UIHorizontalAlign::Right, .VerticalAlign = UIVerticalAlign::Bottom, .Wrap = false, .Shadow = false };
		entity.GetComponent<UIButtonComponent>() = { .Interactable = false, .NormalTint = { 0.1f, 0.2f, 0.3f, 0.4f }, .HighlightTint = { 0.5f, 0.6f, 0.7f, 0.8f }, .PressedTint = { 0.9f, 0.8f, 0.7f, 0.6f }, .DisabledTint = { 0.25f, 0.25f, 0.25f, 0.25f }, .Target = UUID(0xF000000000000003ull), .OnClick = "StartGame" };
		entity.GetComponent<UISliderComponent>() = { .Interactable = false,
													 .Value = 7.5f,
													 .Min = -10.0f,
													 .Max = 20.0f,
													 .Step = 2.5f,
													 .TrackColor = { 0.1f, 0.1f, 0.1f, 0.5f },
													 .FillColor = { 0.2f, 0.3f, 0.4f, 0.5f },
													 .HandleColor = { 0.6f, 0.7f, 0.8f, 0.9f },
													 .HandleWidth = 12.0f,
													 .NormalTint = { 0.9f, 0.9f, 0.9f, 1.0f },
													 .HighlightTint = { 0.95f, 0.95f, 0.95f, 1.0f },
													 .PressedTint = { 0.7f, 0.7f, 0.7f, 1.0f },
													 .DisabledTint = { 0.3f, 0.3f, 0.3f, 0.3f },
													 .Target = UUID(0xF000000000000004ull),
													 .OnValueChanged = "SetVolume" };
		entity.GetComponent<UICheckboxComponent>() = { .Interactable = false, .Checked = true, .BoxColor = { 0.3f, 0.3f, 0.3f, 1.0f }, .CheckColor = { 1.0f, 0.0f, 0.0f, 1.0f }, .NormalTint = { 0.8f, 0.8f, 0.8f, 1.0f }, .HighlightTint = { 0.9f, 1.0f, 0.9f, 1.0f }, .PressedTint = { 0.6f, 0.6f, 0.6f, 1.0f }, .DisabledTint = { 0.4f, 0.4f, 0.4f, 0.4f }, .Target = UUID(0xF000000000000005ull), .OnValueChanged = "" };
		entity.GetComponent<ScriptComponent>().Script = "Assets/Scripts/Test.lua";
		entity.GetComponent<ScriptComponent>().Properties = { { "Speed", 3.5 }, { "Target", { 1, 2, 3 } } };
		JointComponent& joint = entity.GetComponent<JointComponent>();
		joint = { .Type = JointType::Distance,
				  .BodyEntity = UUID(0xF000000000000002ull),
				  .ConnectedEntity = UUID(0xF000000000000001ull),
				  .Anchor = { 1.0f, 2.0f, 3.0f },
				  .ConnectedAnchor = { -1.0f, -2.0f, -3.0f },
				  .Axis = { 0.0f, 0.0f, 1.0f },
				  .SecondaryAxis = { 0.0f, 1.0f, 0.0f },
				  .UseLimits = true,
				  .LimitMin = 0.5f,
				  .LimitMax = 4.0f,
				  .LimitSpringFrequency = 3.0f,
				  .LimitSpringDamping = 0.25f,
				  .LinearLimitMin = { -0.1f, -0.2f, -0.3f },
				  .LinearLimitMax = { 0.1f, 0.2f, 0.3f },
				  .AngularLimitMin = { -10.0f, -20.0f, -30.0f },
				  .AngularLimitMax = { 10.0f, 20.0f, 30.0f },
				  .FreeLinearAxes = { true, false, true },
				  .MotorMode = JointMotorMode::Velocity,
				  .MotorTarget = 12.0f,
				  .LinearMotorMode = { JointMotorMode::Position, JointMotorMode::Off, JointMotorMode::Velocity },
				  .AngularMotorMode = { JointMotorMode::Velocity, JointMotorMode::Position, JointMotorMode::Off },
				  .LinearMotorTarget = { 0.5f, 0.0f, -1.0f },
				  .AngularMotorTarget = { 45.0f, 10.0f, 0.0f },
				  .MotorMaxForce = 250.0f,
				  .MotorMaxTorque = 75.0f,
				  .MotorSpringFrequency = 8.0f,
				  .MotorSpringDamping = 0.5f,
				  .BreakForce = 100.0f,
				  .BreakTorque = 50.0f,
				  .EnableCollision = true };

		const nlohmann::json json = SceneSerializer::SerializeScene(scene);
		Scene loaded;
		std::string error;
		REQUIRE_MESSAGE(SceneSerializer::DeserializeScene(loaded, json, error), error);
		Entity copy = loaded.GetEntityByUUID(entity.GetUUID());
		REQUIRE(copy);

		for (const ComponentInfo& info : ComponentRegistry::GetAll())
		{
			INFO("component: " << info.Name);
			REQUIRE(info.Has(copy));
			CHECK(JsonNear(info.Serialize(copy), info.Serialize(entity)));
		}
		CHECK(glm::degrees(copy.GetComponent<CameraComponent>().Camera.GetPerspectiveVerticalFov()) == doctest::Approx(70.0f));
		CHECK(copy.GetComponent<ParticleEmitterComponent>() == entity.GetComponent<ParticleEmitterComponent>());
		CHECK(copy.GetComponent<RigidBodyComponent>().Type == RigidBodyType::Kinematic);
		CHECK(Near(glm::degrees(copy.GetTransform().GetRotationEuler()), { 10.0f, 20.0f, 30.0f }, 1e-3f));
		CHECK(copy.GetComponent<JointComponent>() == entity.GetComponent<JointComponent>());
		CHECK(copy.GetComponent<UIRectComponent>() == entity.GetComponent<UIRectComponent>());
		CHECK(copy.GetComponent<UIImageComponent>() == entity.GetComponent<UIImageComponent>());
		CHECK(copy.GetComponent<UITextComponent>() == entity.GetComponent<UITextComponent>());
		CHECK(copy.GetComponent<UIButtonComponent>() == entity.GetComponent<UIButtonComponent>());
		CHECK(copy.GetComponent<UISliderComponent>() == entity.GetComponent<UISliderComponent>());
		CHECK(copy.GetComponent<UICheckboxComponent>() == entity.GetComponent<UICheckboxComponent>());
	}

	TEST_CASE("Component JSON is strict about unknown fields, types and enum values")
	{
		Scene scene;
		Entity entity = scene.CreateEntity();
		std::string error;

		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "RigidBody", { { "Mas", 2.0 } }, error));
		CHECK(error.find("unknown field 'Mas'") != std::string::npos);
		CHECK(error.find("Mass") != std::string::npos);
		// A failed add leaves no component behind.
		CHECK_FALSE(entity.HasComponent<RigidBodyComponent>());

		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "RigidBody", { { "Type", "Floating" } }, error));
		CHECK(error.find("Dynamic") != std::string::npos);

		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Transform", { { "Translation", { 1, 2 } } }, error));
		CHECK(error.find("3 numbers") != std::string::npos);

		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Mesh", { { "CastShadows", 1 } }, error));
		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Banana", nlohmann::json::object(), error));
		CHECK(error.find("unknown component") != std::string::npos);

		// Patches merge: untouched fields keep their values.
		REQUIRE(ComponentRegistry::AddOrPatch(entity, "RigidBody", { { "Type", "Dynamic" }, { "Mass", 5 } }, error));
		REQUIRE(ComponentRegistry::AddOrPatch(entity, "RigidBody", { { "Friction", 0.9 } }, error));
		const auto& body = entity.GetComponent<RigidBodyComponent>();
		CHECK(body.Type == RigidBodyType::Dynamic);
		CHECK(body.Mass == 5.0f);
		CHECK(body.Friction == doctest::Approx(0.9f));

		// A failed patch does not partially apply.
		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "RigidBody", { { "Mass", 9 }, { "Bogus", 1 } }, error));
		CHECK(body.Mass == 5.0f);
	}

	TEST_CASE("Scene files save and load, preserving hierarchy order and settings")
	{
		BasaltTest::TempProject project("SceneFiles");
		Ref<Scene> scene = CreateRef<Scene>("Level1");
		Entity a = scene->CreateEntity("A");
		Entity b = scene->CreateEntity("B");
		Entity c = scene->CreateChildEntity(a, "C");
		Entity d = scene->CreateChildEntity(a, "D");
		scene->GetRendererSettings().Tonemap = Tonemapper::AgX;
		scene->GetRendererSettings().FSR1Enabled = true;
		scene->GetRendererSettings().FSR1Sharpness = 1.5f;
		scene->GetPhysicsSettings().Gravity = { 0.0f, -3.0f, 0.0f };

		const auto path = project.GetDirectory() / "Assets/Scenes/Level1.bscene";
		std::string error;
		REQUIRE(SceneSerializer::SaveScene(*scene, path, error));

		Ref<Scene> loaded = SceneSerializer::LoadScene(path, error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->GetName() == "Level1");
		CHECK(loaded->GetRendererSettings().Tonemap == Tonemapper::AgX);
		CHECK(loaded->GetRendererSettings().FSR1Enabled);
		CHECK(loaded->GetRendererSettings().FSR1Sharpness == 1.5f);
		CHECK(Scene::Copy(loaded)->GetRendererSettings().FSR1Sharpness == 1.5f);
		CHECK(loaded->GetPhysicsSettings().Gravity.y == -3.0f);

		const std::vector<Entity> ordered = loaded->GetAllEntitiesOrdered();
		REQUIRE(ordered.size() == 4);
		CHECK(ordered[0].GetUUID() == a.GetUUID());
		CHECK(ordered[1].GetUUID() == c.GetUUID());
		CHECK(ordered[2].GetUUID() == d.GetUUID());
		CHECK(ordered[3].GetUUID() == b.GetUUID());
	}

	TEST_CASE("Loading reports malformed scenes with context")
	{
		BasaltTest::TempProject project("BadScenes");
		std::string error;

		CHECK_FALSE(SceneSerializer::LoadScene(project.GetDirectory() / "missing.bscene", error));
		CHECK(error.find("cannot read") != std::string::npos);

		project.WriteFile("bad.bscene", "{ not json");
		CHECK_FALSE(SceneSerializer::LoadScene(project.GetDirectory() / "bad.bscene", error));
		CHECK(error.find("not valid JSON") != std::string::npos);

		project.WriteFile("badparent.bscene", R"({"Entities":[{"ID":5,"Name":"Orphan","Parent":99}]})");
		CHECK_FALSE(SceneSerializer::LoadScene(project.GetDirectory() / "badparent.bscene", error));
		CHECK(error.find("unknown parent") != std::string::npos);

		project.WriteFile("badcomponent.bscene", R"({"Entities":[{"ID":5,"Name":"Box","Components":{"BoxCollider":{"HalfExtents":"big"}}}]})");
		CHECK_FALSE(SceneSerializer::LoadScene(project.GetDirectory() / "badcomponent.bscene", error));
		CHECK(error.find("Box") != std::string::npos);
		CHECK(error.find("HalfExtents") != std::string::npos);

		project.WriteFile("future.bscene", R"({"Version":999})");
		CHECK_FALSE(SceneSerializer::LoadScene(project.GetDirectory() / "future.bscene", error));
	}

	TEST_CASE("Scenes from before named physics layers load when their layer settings convert losslessly")
	{
		BasaltTest::TempProject project("LegacyLayers");
		std::string error;

		// What every earlier build wrote for a RigidBody: an index and a full 32-bit or 16-bit mask.
		project.WriteFile("legacy.bscene", R"({"Entities":[
			{"ID":5,"Name":"Floor","Components":{"RigidBody":{"Type":"Static","Layer":0,"CollisionMask":4294967295}}},
			{"ID":6,"Name":"Crate","Components":{"RigidBody":{"Type":"Dynamic","Layer":3,"CollisionMask":65535}}}]})");
		const uint64_t since = Log::GetHistory().GetTotalCount();
		const Ref<Scene> legacy = SceneSerializer::LoadScene(project.GetDirectory() / "legacy.bscene", error);
		REQUIRE_MESSAGE(legacy, error);
		CHECK(legacy->GetEntityByUUID(5).GetComponent<RigidBodyComponent>().Layer == "Default");
		CHECK(legacy->GetEntityByUUID(6).GetComponent<RigidBodyComponent>().Layer == "Default");
		// Saving writes the new format.
		const nlohmann::json saved = SceneSerializer::SerializeScene(*legacy);
		CHECK(saved.dump().find("CollisionMask") == std::string::npos);
		CHECK(BasaltTest::CountLogMessages(since, "before named physics layers") <= 1);

		// A mask that excluded layers has no equivalent without project layers.
		project.WriteFile("masked.bscene", R"({"Entities":[{"ID":5,"Name":"Ghost","Components":{"RigidBody":{"Layer":1,"CollisionMask":3}}}]})");
		CHECK_FALSE(SceneSerializer::LoadScene(project.GetDirectory() / "masked.bscene", error));
		CHECK(error.find("Ghost") != std::string::npos);
		CHECK(error.find("collision matrix") != std::string::npos);

		project.WriteFile("fraction.bscene", R"({"Entities":[{"ID":5,"Name":"Odd","Components":{"RigidBody":{"Layer":1.5}}}]})");
		CHECK_FALSE(SceneSerializer::LoadScene(project.GetDirectory() / "fraction.bscene", error));
		CHECK(error.find("layer name") != std::string::npos);
	}

	TEST_CASE("Prefabs save and instantiate with fresh UUIDs")
	{
		BasaltTest::TempProject project("Prefabs");
		Scene scene;
		Entity root = scene.CreateEntity("Enemy");
		root.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		Entity weapon = scene.CreateChildEntity(root, "Weapon");
		weapon.GetTransform().Translation = { 0.5f, 0.0f, 0.0f };

		std::string error;
		REQUIRE(SceneSerializer::SavePrefab(root, project.GetDirectory() / "Assets/Prefabs/Enemy.bprefab", error));

		Entity first = SceneSerializer::InstantiatePrefab(scene, project.GetDirectory() / "Assets/Prefabs/Enemy.bprefab", "Assets/Prefabs/Enemy.bprefab", {}, error);
		Entity second = SceneSerializer::InstantiatePrefab(scene, project.GetDirectory() / "Assets/Prefabs/Enemy.bprefab", "Assets/Prefabs/Enemy.bprefab", root, error);
		REQUIRE_MESSAGE(first, error);
		REQUIRE(second);
		CHECK(first.GetUUID() != root.GetUUID());
		CHECK(first.GetUUID() != second.GetUUID());
		CHECK(first.GetName() == "Enemy");
		CHECK(first.GetComponent<PrefabComponent>().Prefab == "Assets/Prefabs/Enemy.bprefab");
		CHECK(first.GetComponent<RigidBodyComponent>().Type == RigidBodyType::Dynamic);
		REQUIRE(first.GetChildren().size() == 1);
		CHECK(first.GetChildren()[0].GetTransform().Translation.x == 0.5f);
		CHECK(second.GetParent() == root);
		CHECK(scene.GetEntityCount() == 6);

		CHECK_FALSE(SceneSerializer::InstantiatePrefab(scene, project.GetDirectory() / "nope.bprefab", "nope.bprefab", {}, error));
		CHECK(scene.GetEntityCount() == 6);
	}

	TEST_CASE("Joint entity references read numbers and Lua's signed IDs")
	{
		Scene scene;
		Entity entity = scene.CreateEntity("Door");
		std::string error;
		REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(entity, "Joint", { { "Type", "Slider" }, { "ConnectedEntity", 42 } }, error), error);
		CHECK(entity.GetComponent<JointComponent>().Type == JointType::Slider);
		CHECK(entity.GetComponent<JointComponent>().ConnectedEntity == 42);

		// UUIDs above INT64_MAX reach Lua as negative integers and must come back unchanged.
		const uint64_t large = 0xF000000000000001ull;
		REQUIRE(ComponentRegistry::AddOrPatch(entity, "Joint", { { "ConnectedEntity", static_cast<int64_t>(large) } }, error));
		CHECK(static_cast<uint64_t>(entity.GetComponent<JointComponent>().ConnectedEntity) == large);
		CHECK(ComponentRegistry::Find("Joint")->Serialize(entity)["ConnectedEntity"].get<uint64_t>() == large);

		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Joint", { { "ConnectedEntity", "Frame" } }, error));
		CHECK(error.find("ConnectedEntity") != std::string::npos);
		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Joint", { { "MotorMode", "Turbo" } }, error));
	}

	TEST_CASE("Per-axis joint fields read three values and reject anything else unchanged")
	{
		Scene scene;
		Entity entity = scene.CreateEntity("Hip");
		std::string error;
		REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(entity, "Joint", { { "Type", "SixDOF" }, { "FreeLinearAxes", { false, true, false } }, { "AngularMotorMode", { "Position", "Velocity", "Off" } } }, error), error);
		const JointComponent& joint = entity.GetComponent<JointComponent>();
		CHECK(joint.FreeLinearAxes == glm::bvec3(false, true, false));
		CHECK(joint.AngularMotorMode == std::array{ JointMotorMode::Position, JointMotorMode::Velocity, JointMotorMode::Off });
		const nlohmann::json written = ComponentRegistry::Find("Joint")->Serialize(entity);
		CHECK(written["FreeLinearAxes"] == nlohmann::json({ false, true, false }));
		CHECK(written["AngularMotorMode"] == nlohmann::json({ "Position", "Velocity", "Off" }));

		// A bad element rejects the whole patch: the first elements are not applied either.
		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Joint", { { "AngularMotorMode", { "Off", "Off", "Turbo" } } }, error));
		CHECK(error.find("Turbo") != std::string::npos);
		CHECK(joint.AngularMotorMode[0] == JointMotorMode::Position);
		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Joint", { { "LinearMotorMode", "Velocity" } }, error));
		CHECK(error.find("array of 3 strings") != std::string::npos);
		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Joint", { { "LinearMotorMode", { "Off", "Off" } } }, error));
		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Joint", { { "FreeLinearAxes", { 1, 0, 0 } } }, error));
		CHECK(error.find("array of 3 booleans") != std::string::npos);
		CHECK(joint.FreeLinearAxes == glm::bvec3(false, true, false));
		CHECK(ComponentRegistry::Find("Joint")->EnumOptions.at("LinearMotorMode") == std::vector<std::string>{ "Off", "Velocity", "Position" });
	}

	TEST_CASE("Entity reference fields are collected from the component definitions")
	{
		for (const ComponentInfo& info : ComponentRegistry::GetAll())
		{
			INFO("component: " << info.Name);
			if (info.Name == "Joint")
			{
				CHECK(info.EntityFields == std::vector<std::string>{ "BodyEntity", "ConnectedEntity" });
				CHECK(info.RemapEntityReferences);
			}
			else if (info.Name == "UIButton" || info.Name == "UISlider" || info.Name == "UICheckbox")
			{
				CHECK(info.EntityFields == std::vector<std::string>{ "Target" });
				CHECK(info.RemapEntityReferences);
			}
			else
			{
				CHECK(info.EntityFields.empty());
				CHECK_FALSE(info.RemapEntityReferences);
			}
		}
	}

	TEST_CASE("The joint field rule covers every Joint field the registry reads")
	{
		const ComponentInfo* info = ComponentRegistry::Find("Joint");
		REQUIRE(info);
		CHECK(GetJointFieldNames() == info->Fields);

		// A default joint of any type ignores nothing, with or without limits, except UseLimits itself on the
		// types that have no limits.
		for (int type = 0; type <= static_cast<int>(JointType::SixDOF); type++)
		{
			for (bool useLimits : { false, true })
			{
				JointComponent joint;
				joint.Type = static_cast<JointType>(type);
				joint.UseLimits = useLimits;
				const bool noLimits = joint.Type == JointType::Fixed || joint.Type == JointType::Point;
				INFO("type " << type << ", UseLimits " << useLimits);
				CHECK(GetIgnoredJointFields(joint) == (useLimits && noLimits ? std::vector<std::string>{ "UseLimits" } : std::vector<std::string>{}));
				// Fields every joint uses.
				for (const char* field : { "Type", "BodyEntity", "ConnectedEntity", "Anchor", "BreakForce", "EnableCollision" })
					CHECK(JointFieldApplies(joint, field));
			}
		}

		JointComponent hinge;
		hinge.MotorMode = JointMotorMode::Velocity;
		hinge.LimitMin = -10.0f;
		hinge.AngularLimitMax = { 5.0f, 0.0f, 0.0f };
		CHECK(GetIgnoredJointFields(hinge) == std::vector<std::string>{ "LimitMin", "AngularLimitMax" });
		hinge.UseLimits = true;
		CHECK(GetIgnoredJointFields(hinge) == std::vector<std::string>{ "AngularLimitMax" });
		// A distance joint's spring works without limits (a bungee); a hinge's does not.
		JointComponent rope;
		rope.Type = JointType::Distance;
		rope.LimitSpringFrequency = 2.0f;
		CHECK(GetIgnoredJointFields(rope).empty());
		hinge.UseLimits = false;
		hinge.LimitSpringFrequency = 2.0f;
		CHECK(GetIgnoredJointFields(hinge) == std::vector<std::string>{ "LimitMin", "LimitSpringFrequency", "AngularLimitMax" });

		// Six-DOF joints use per-axis motors and free axes; hinges and sliders use the single motor. The motor
		// force and spring are shared.
		JointComponent poweredHinge;
		poweredHinge.FreeLinearAxes.x = true;
		poweredHinge.AngularMotorMode[0] = JointMotorMode::Position;
		poweredHinge.MotorSpringFrequency = 10.0f;
		CHECK(GetIgnoredJointFields(poweredHinge) == std::vector<std::string>{ "FreeLinearAxes", "AngularMotorMode" });
		JointComponent ragdoll;
		ragdoll.Type = JointType::SixDOF;
		ragdoll.MotorMode = JointMotorMode::Position;
		ragdoll.FreeLinearAxes.y = true;
		ragdoll.AngularMotorTarget = { 10.0f, 0.0f, 0.0f };
		ragdoll.MotorSpringDamping = 0.5f;
		CHECK(GetIgnoredJointFields(ragdoll) == std::vector<std::string>{ "MotorMode" });
		// A hinge with its own MotorMaxTorque no longer reads MotorMaxForce.
		JointComponent torqueHinge;
		torqueHinge.MotorMaxForce = 50.0f;
		CHECK(GetIgnoredJointFields(torqueHinge).empty());
		torqueHinge.MotorMaxTorque = 20.0f;
		CHECK(GetIgnoredJointFields(torqueHinge) == std::vector<std::string>{ "MotorMaxForce" });
		// A negative torque cap falls back to MotorMaxForce (with a warning), so MotorMaxForce applies again.
		torqueHinge.MotorMaxTorque = -5.0f;
		CHECK(GetIgnoredJointFields(torqueHinge).empty());
		// A rotation motor makes a six-DOF joint without limits hold torque, so BreakTorque applies.
		ragdoll.BreakTorque = 10.0f;
		CHECK_FALSE(JointFieldApplies(ragdoll, "BreakTorque"));
		ragdoll.AngularMotorMode[2] = JointMotorMode::Velocity;
		CHECK(JointFieldApplies(ragdoll, "BreakTorque"));
	}

	TEST_CASE("Which joint fields apply, for every type, UseLimits and six-DOF rotation motor")
	{
		// One character per combination, in the order: type (Fixed..SixDOF), UseLimits off/on, rotation motor
		// off/on. Pins the rule so restructuring it cannot change which fields the inspector shows.
		const std::map<std::string, std::string> expected = {
			{ "Type", "1111111111111111111111111111" },
			{ "BodyEntity", "1111111111111111111111111111" },
			{ "ConnectedEntity", "1111111111111111111111111111" },
			{ "Anchor", "1111111111111111111111111111" },
			{ "ConnectedAnchor", "0000000000000000111100000000" },
			{ "Axis", "0000000011111111000011111111" },
			{ "SecondaryAxis", "0000000000000000000000001111" },
			{ "UseLimits", "0000000011111111111111111111" },
			{ "LimitMin", "0000000000110011001100000000" },
			{ "LimitMax", "0000000000110011001100110000" },
			{ "LimitSpringFrequency", "0000000000110011111100000011" },
			{ "LimitSpringDamping", "0000000000110011111100000011" },
			{ "LinearLimitMin", "0000000000000000000000000011" },
			{ "LinearLimitMax", "0000000000000000000000000011" },
			{ "AngularLimitMin", "0000000000000000000000000011" },
			{ "AngularLimitMax", "0000000000000000000000000011" },
			{ "FreeLinearAxes", "0000000000000000000000001111" },
			{ "MotorMode", "0000000011111111000000000000" },
			{ "MotorTarget", "0000000011111111000000000000" },
			{ "LinearMotorMode", "0000000000000000000000001111" },
			{ "AngularMotorMode", "0000000000000000000000001111" },
			{ "LinearMotorTarget", "0000000000000000000000001111" },
			{ "AngularMotorTarget", "0000000000000000000000001111" },
			{ "MotorMaxForce", "0000000011111111000000001111" },
			{ "MotorMaxTorque", "0000000011110000000000001111" },
			{ "MotorSpringFrequency", "0000000011111111000000001111" },
			{ "MotorSpringDamping", "0000000011111111000000001111" },
			{ "BreakForce", "1111111111111111111111111111" },
			{ "BreakTorque", "1111000011111111000000110111" },
			{ "EnableCollision", "1111111111111111111111111111" },
		};
		for (const std::string& field : GetJointFieldNames())
		{
			std::string actual;
			for (const JointType type : { JointType::Fixed, JointType::Point, JointType::Hinge, JointType::Slider, JointType::Distance, JointType::Cone, JointType::SixDOF })
			{
				for (const bool limits : { false, true })
				{
					for (const bool motor : { false, true })
					{
						JointComponent joint;
						joint.Type = type;
						joint.UseLimits = limits;
						if (motor)
							joint.AngularMotorMode[1] = JointMotorMode::Velocity;
						actual += JointFieldApplies(joint, field) ? '1' : '0';
					}
				}
			}
			INFO("field: " << field);
			auto it = expected.find(field);
			REQUIRE(it != expected.end());
			CHECK(it->second == actual);
		}
	}

	TEST_CASE("Only the fields a live joint can update in place avoid a rebuild")
	{
		// A different valid value for every field; the list must cover them all.
		const std::map<std::string, std::function<void(JointComponent&)>> change = {
			{ "Type", [](JointComponent& j) { j.Type = JointType::Slider; } },
			{ "BodyEntity", [](JointComponent& j) { j.BodyEntity = UUID(7); } },
			{ "ConnectedEntity", [](JointComponent& j) { j.ConnectedEntity = UUID(8); } },
			{ "Anchor", [](JointComponent& j) { j.Anchor.x += 1.0f; } },
			{ "ConnectedAnchor", [](JointComponent& j) { j.ConnectedAnchor.x += 1.0f; } },
			{ "Axis", [](JointComponent& j) { j.Axis = { 1.0f, 0.0f, 0.0f }; } },
			{ "SecondaryAxis", [](JointComponent& j) { j.SecondaryAxis = { 0.0f, 0.0f, 1.0f }; } },
			{ "UseLimits", [](JointComponent& j) { j.UseLimits = !j.UseLimits; } },
			{ "LimitMin", [](JointComponent& j) { j.LimitMin -= 1.0f; } },
			{ "LimitMax", [](JointComponent& j) { j.LimitMax += 1.0f; } },
			{ "LimitSpringFrequency", [](JointComponent& j) { j.LimitSpringFrequency += 1.0f; } },
			{ "LimitSpringDamping", [](JointComponent& j) { j.LimitSpringDamping += 1.0f; } },
			{ "LinearLimitMin", [](JointComponent& j) { j.LinearLimitMin.x -= 1.0f; } },
			{ "LinearLimitMax", [](JointComponent& j) { j.LinearLimitMax.x += 1.0f; } },
			{ "AngularLimitMin", [](JointComponent& j) { j.AngularLimitMin.x -= 1.0f; } },
			{ "AngularLimitMax", [](JointComponent& j) { j.AngularLimitMax.x += 1.0f; } },
			{ "FreeLinearAxes", [](JointComponent& j) { j.FreeLinearAxes.x = !j.FreeLinearAxes.x; } },
			{ "MotorMode", [](JointComponent& j) { j.MotorMode = JointMotorMode::Position; } },
			{ "MotorTarget", [](JointComponent& j) { j.MotorTarget += 1.0f; } },
			{ "LinearMotorMode", [](JointComponent& j) { j.LinearMotorMode[0] = JointMotorMode::Position; } },
			{ "AngularMotorMode", [](JointComponent& j) { j.AngularMotorMode[0] = JointMotorMode::Position; } },
			{ "LinearMotorTarget", [](JointComponent& j) { j.LinearMotorTarget.x += 1.0f; } },
			{ "AngularMotorTarget", [](JointComponent& j) { j.AngularMotorTarget.x += 1.0f; } },
			{ "MotorMaxForce", [](JointComponent& j) { j.MotorMaxForce += 1.0f; } },
			{ "MotorMaxTorque", [](JointComponent& j) { j.MotorMaxTorque += 1.0f; } },
			{ "MotorSpringFrequency", [](JointComponent& j) { j.MotorSpringFrequency += 1.0f; } },
			{ "MotorSpringDamping", [](JointComponent& j) { j.MotorSpringDamping += 1.0f; } },
			{ "BreakForce", [](JointComponent& j) { j.BreakForce += 1.0f; } },
			{ "BreakTorque", [](JointComponent& j) { j.BreakTorque += 1.0f; } },
			{ "EnableCollision", [](JointComponent& j) { j.EnableCollision = !j.EnableCollision; } },
		};
		const std::set<std::string> rebuilds = { "Type", "BodyEntity", "ConnectedEntity", "Anchor", "ConnectedAnchor", "Axis", "SecondaryAxis", "UseLimits" };
		for (const std::string& field : GetJointFieldNames())
		{
			INFO("field: " << field);
			auto it = change.find(field);
			REQUIRE(it != change.end());
			const JointComponent built;
			JointComponent changed = built;
			it->second(changed);
			REQUIRE(changed != built);
			CHECK(JointNeedsRebuild(built, changed) == rebuilds.contains(field));
		}
	}

	TEST_CASE("Joints inside a duplicated tree or prefab connect the copies")
	{
		BasaltTest::TempProject project("JointPrefab");
		Scene scene;
		Entity anchor = scene.CreateEntity("Anchor");
		Entity chain = scene.CreateEntity("Chain");
		Entity link1 = scene.CreateChildEntity(chain, "Link1");
		Entity link2 = scene.CreateChildEntity(chain, "Link2");
		// Link1 hangs from an entity outside the tree, Link2 from Link1.
		link1.AddComponent<JointComponent>().ConnectedEntity = anchor.GetUUID();
		link2.AddComponent<JointComponent>().ConnectedEntity = link1.GetUUID();

		Entity copy = scene.DuplicateEntity(chain);
		REQUIRE(copy.GetChildren().size() == 2);
		Entity copy1 = copy.GetChildren()[0];
		Entity copy2 = copy.GetChildren()[1];
		CHECK(copy1.GetComponent<JointComponent>().ConnectedEntity == anchor.GetUUID());
		CHECK(copy2.GetComponent<JointComponent>().ConnectedEntity == copy1.GetUUID());
		// The originals are untouched.
		CHECK(link2.GetComponent<JointComponent>().ConnectedEntity == link1.GetUUID());

		std::string error;
		const std::filesystem::path path = project.GetDirectory() / "Assets/Prefabs/Chain.bprefab";
		REQUIRE(SceneSerializer::SavePrefab(chain, path, error));
		Entity instance = SceneSerializer::InstantiatePrefab(scene, path, "Assets/Prefabs/Chain.bprefab", {}, error);
		REQUIRE_MESSAGE(instance, error);
		REQUIRE(instance.GetChildren().size() == 2);
		Entity instance1 = instance.GetChildren()[0];
		Entity instance2 = instance.GetChildren()[1];
		CHECK(instance1.GetComponent<JointComponent>().ConnectedEntity == anchor.GetUUID());
		CHECK(instance2.GetComponent<JointComponent>().ConnectedEntity == instance1.GetUUID());
	}

	TEST_CASE("Joint entities inside a duplicated tree or prefab move the copied body")
	{
		BasaltTest::TempProject project("JointBodyPrefab");
		Scene scene;
		Entity anchor = scene.CreateEntity("Anchor");
		Entity rung = scene.CreateEntity("Rung");
		// Two joint entities under the rung, each holding one joint of the rung's body.
		Entity left = scene.CreateChildEntity(rung, "Left");
		Entity right = scene.CreateChildEntity(rung, "Right");
		for (Entity holder : { left, right })
		{
			auto& joint = holder.AddComponent<JointComponent>();
			joint.BodyEntity = rung.GetUUID();
			joint.ConnectedEntity = anchor.GetUUID();
		}

		Entity copy = scene.DuplicateEntity(rung);
		REQUIRE(copy.GetChildren().size() == 2);
		for (Entity holder : copy.GetChildren())
		{
			CHECK(holder.GetComponent<JointComponent>().BodyEntity == copy.GetUUID());
			CHECK(holder.GetComponent<JointComponent>().ConnectedEntity == anchor.GetUUID());
		}
		CHECK(left.GetComponent<JointComponent>().BodyEntity == rung.GetUUID());

		std::string error;
		const std::filesystem::path path = project.GetDirectory() / "Assets/Prefabs/Rung.bprefab";
		REQUIRE(SceneSerializer::SavePrefab(rung, path, error));
		Entity instance = SceneSerializer::InstantiatePrefab(scene, path, "Assets/Prefabs/Rung.bprefab", {}, error);
		REQUIRE_MESSAGE(instance, error);
		REQUIRE(instance.GetChildren().size() == 2);
		for (Entity holder : instance.GetChildren())
			CHECK(holder.GetComponent<JointComponent>().BodyEntity == instance.GetUUID());
	}

	TEST_CASE("Joints saved before BodyEntity, limit springs and six-DOF limits load with defaults")
	{
		Scene scene;
		Entity entity = scene.CreateEntity("Door");
		std::string error;
		// The full field set written by earlier builds.
		const nlohmann::json old = { { "Type", "Hinge" }, { "ConnectedEntity", 7 }, { "Anchor", { 0, 1, 0 } }, { "ConnectedAnchor", { 0, 0, 0 } }, { "Axis", { 0, 1, 0 } }, { "UseLimits", true }, { "LimitMin", -45 }, { "LimitMax", 90 }, { "MotorMode", "Off" }, { "MotorTarget", 0 }, { "MotorMaxForce", 1000 }, { "BreakForce", 0 }, { "BreakTorque", 0 }, { "EnableCollision", false } };
		REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(entity, "Joint", old, error), error);
		const JointComponent& joint = entity.GetComponent<JointComponent>();
		const JointComponent defaults;
		CHECK(joint.BodyEntity == 0);
		CHECK(joint.ConnectedEntity == 7);
		CHECK(joint.LimitMax == 90.0f);
		CHECK(joint.SecondaryAxis == defaults.SecondaryAxis);
		CHECK(joint.LimitSpringFrequency == 0.0f);
		CHECK(joint.LimitSpringDamping == 1.0f);
		CHECK(joint.LinearLimitMin == defaults.LinearLimitMin);
		CHECK(joint.AngularLimitMax == defaults.AngularLimitMax);
		CHECK(joint.FreeLinearAxes == defaults.FreeLinearAxes);
		CHECK(joint.AngularMotorMode == defaults.AngularMotorMode);
		CHECK(joint.MotorSpringFrequency == 2.0f);
		CHECK(joint.MotorSpringDamping == 1.0f);
		// MotorMaxForce keeps capping the torque too, as it did when it was the only limit.
		CHECK(joint.MotorMaxTorque == 0.0f);
		CHECK(JointFieldApplies(joint, "MotorMaxForce"));
	}

	TEST_CASE("A prefab with joints spawned during play builds them between the new copies")
	{
		BasaltTest::TempProject project("JointPrefabPlay");
		Scene scene;
		auto addBody = [](Entity entity, RigidBodyType type) {
			entity.AddComponent<RigidBodyComponent>().Type = type;
			entity.AddComponent<BoxColliderComponent>().HalfExtents = { 0.2f, 0.2f, 0.2f };
		};
		Entity anchor = scene.CreateEntity("Anchor");
		addBody(anchor, RigidBodyType::Static);
		Entity chain = scene.CreateEntity("Chain");
		Entity link1 = scene.CreateChildEntity(chain, "Link1");
		link1.GetTransform().Translation = { 0.0f, -1.0f, 0.0f };
		addBody(link1, RigidBodyType::Dynamic);
		Entity link2 = scene.CreateChildEntity(chain, "Link2");
		link2.GetTransform().Translation = { 0.0f, -2.0f, 0.0f };
		addBody(link2, RigidBodyType::Dynamic);
		auto& top = link1.AddComponent<JointComponent>();
		top.Type = JointType::Point;
		top.ConnectedEntity = anchor.GetUUID();
		top.Anchor = { 0.0f, 1.0f, 0.0f };
		auto& bottom = link2.AddComponent<JointComponent>();
		bottom.Type = JointType::Point;
		bottom.ConnectedEntity = link1.GetUUID();
		bottom.Anchor = { 0.0f, 1.0f, 0.0f };

		std::string error;
		const std::filesystem::path path = project.GetDirectory() / "Assets/Prefabs/Chain.bprefab";
		REQUIRE(SceneSerializer::SavePrefab(chain, path, error));
		scene.DestroyEntity(chain);

		scene.OnSimulationStart();
		Entity instance = SceneSerializer::InstantiatePrefab(scene, path, "Assets/Prefabs/Chain.bprefab", {}, error);
		REQUIRE_MESSAGE(instance, error);
		Entity copy1 = instance.GetChildren()[0];
		Entity copy2 = instance.GetChildren()[1];
		for (int i = 0; i < 60; i++)
			scene.OnUpdate(1.0f / 60.0f);
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		CHECK(physics.HasJoint(copy1));
		CHECK(physics.HasJoint(copy2));
		// The chain hangs from the anchor instead of falling.
		CHECK(copy2.GetTransform().Translation.y == doctest::Approx(-2.0f).epsilon(0.05));
		scene.OnSimulationStop();
	}

	TEST_CASE("The state hash does not depend on the UUIDs joints refer to")
	{
		auto build = []() {
			Scene scene;
			Entity frame = scene.CreateEntity("Frame");
			Entity door = scene.CreateEntity("Door");
			door.AddComponent<JointComponent>().ConnectedEntity = frame.GetUUID();
			return SceneSerializer::ComputeStateHash(scene);
		};
		CHECK(build() == build());
	}
}

TEST_SUITE("Scene loading")
{
	namespace {

		// Scenes A and B share the manager (same ID, so it is skipped in B while the persistent one lives); each
		// has its own level entity. B's manager has a child of its own that is skipped with it.
		void WriteSwitchProject(const BasaltTest::TempProject& project)
		{
			project.WriteFile("Assets/Scripts/Manager.lua", R"(
				local M = {}
				function M:OnCreate()
					Scene.DontDestroyOnLoad(self.Entity)
					ManagerCreates = (ManagerCreates or 0) + 1
					self.Ticks = 0
					Timer.Every(0.05, function() self.Ticks = self.Ticks + 1 end)
					Events.On("LevelStarted", function(name) LastLevel = name end)
				end
				return M
			)");
			project.WriteFile("Assets/Scripts/Level.lua", R"(
				local L = {}
				function L:OnCreate()
					LevelCreates = (LevelCreates or 0) + 1
					-- Bodies exist before OnCreate, as at play start.
					LevelSeesFloor = Physics.Raycast(Vec3(0, 5, 0), Vec3(0, -1, 0), 10) ~= nil
					Events.Emit("LevelStarted", Scene.GetName())
				end
				function L:OnDestroy() LevelDestroys = (LevelDestroys or 0) + 1 end
				return L
			)");
			project.WriteFile("Assets/Scenes/A.bscene", R"({
				"Format": "BasaltScene", "Version": 1, "Name": "A",
				"Renderer": { "Exposure": 3.0, "SSAOEnabled": false },
				"Physics": { "Gravity": [0, -20, 0] },
				"Entities": [
					{ "ID": 100, "Name": "Manager", "Parent": 0, "Components": { "Script": { "Script": "Assets/Scripts/Manager.lua" } } },
					{ "ID": 101, "Name": "ManagerChild", "Parent": 100, "Components": {} },
					{ "ID": 200, "Name": "LevelA", "Parent": 0, "Components": { "Script": { "Script": "Assets/Scripts/Level.lua" } } }
				]
			})");
			project.WriteFile("Assets/Scenes/B.bscene", R"({
				"Format": "BasaltScene", "Version": 1, "Name": "B",
				"Physics": { "Gravity": [0, -1, 0] },
				"Entities": [
					{ "ID": 102, "Name": "OtherManagerChild", "Parent": 100, "Components": {} },
					{ "ID": 100, "Name": "Manager", "Parent": 0, "Components": { "Script": { "Script": "Assets/Scripts/Manager.lua" } } },
					{ "ID": 300, "Name": "LevelB", "Parent": 0, "Components": {
						"Script": { "Script": "Assets/Scripts/Level.lua" },
						"RigidBody": {}, "BoxCollider": { "HalfExtents": [5, 0.5, 5] } } },
					{ "ID": 301, "Name": "LevelBChild", "Parent": 300, "Components": {} }
				]
			})");
		}

		using BasaltTest::Eval;

	}

	TEST_CASE("Scene.Load replaces the scene at the end of the frame and keeps persistent entities")
	{
		BasaltTest::TempProject project("SceneLoadSwitch");
		WriteSwitchProject(project);
		std::string error;
		Ref<Scene> scene = SceneSerializer::LoadScene(project.GetDirectory() / "Assets/Scenes/A.bscene", error);
		REQUIRE_MESSAGE(scene, error);
		CHECK(scene->GetPath() == "Assets/Scenes/A.bscene");
		scene->OnRuntimeStart();
		scene->OnUpdate(BasaltTest::Step);
		CHECK(Eval(*scene, "Scene.GetName() .. ' ' .. Scene.GetPath()") == "A Assets/Scenes/A.bscene");
		const UUID managerID = 100;
		CHECK(scene->IsPersistent(scene->GetEntityByUUID(101)));
		CHECK_FALSE(scene->IsPersistent(scene->GetEntityByUUID(200)));

		// Deferred: nothing changes until the frame ends. The last request of a frame wins.
		CHECK(Eval(*scene, "Scene.Load('Assets/Scenes/A.bscene') Scene.Load('Assets/Scenes/B.bscene')") == "");
		CHECK(scene->HasPendingLoad());
		CHECK(scene->FindEntityByName("LevelA"));
		const auto ticksBefore = scene->GetScriptEngine()->GetInstanceField(scene->GetEntityByUUID(managerID), "Ticks");
		scene->OnUpdate(BasaltTest::Step);

		CHECK_FALSE(scene->HasPendingLoad());
		CHECK(scene->GetName() == "B");
		CHECK(scene->GetPath() == "Assets/Scenes/B.bscene");
		CHECK_FALSE(scene->FindEntityByName("LevelA"));
		REQUIRE(scene->FindEntityByName("LevelB"));
		CHECK(scene->FindEntityByName("LevelBChild").GetParent() == scene->FindEntityByName("LevelB"));
		// The persistent manager and its own child are kept; the file's copy and its child are skipped.
		CHECK(scene->FindEntitiesByName("Manager").size() == 1);
		CHECK(scene->GetEntityByUUID(101));
		CHECK_FALSE(scene->GetEntityByUUID(102));
		CHECK(scene->GetEntityCount() == 4);
		CHECK(Eval(*scene, "ManagerCreates .. ' ' .. LevelCreates .. ' ' .. LevelDestroys .. ' ' .. LastLevel .. ' ' .. tostring(LevelSeesFloor)") == "1 2 1 B true");
		// Settings come from B; those it leaves out are defaults, not A's.
		CHECK(scene->GetRendererSettings().Exposure == 1.0f);
		CHECK(scene->GetRendererSettings().SSAOEnabled);
		CHECK(scene->GetPhysicsWorld()->GetGravity() == glm::vec3(0.0f, -1.0f, 0.0f));

		// The manager's state and timer carried over.
		for (int i = 0; i < 6; i++)
			scene->OnUpdate(BasaltTest::Step);
		const auto ticksAfter = scene->GetScriptEngine()->GetInstanceField(scene->GetEntityByUUID(managerID), "Ticks");
		REQUIRE(ticksBefore.has_value());
		REQUIRE(ticksAfter.has_value());
		CHECK(ticksAfter->get<int>() > ticksBefore->get<int>());
		CHECK(scene->GetScriptEngine()->GetErrors().empty());

		// Loading A again: the persistent manager stays the only one.
		CHECK(Eval(*scene, "Scene.Load('Assets/Scenes/A.bscene')") == "");
		scene->OnUpdate(BasaltTest::Step);
		CHECK(scene->FindEntitiesByName("Manager").size() == 1);
		CHECK(scene->FindEntityByName("LevelA"));
		CHECK_FALSE(scene->FindEntityByName("LevelB"));
		CHECK(scene->GetPhysicsWorld()->GetGravity() == glm::vec3(0.0f, -20.0f, 0.0f));
		CHECK(Eval(*scene, "ManagerCreates .. ' ' .. LastLevel") == "1 A");

		Entity manager = scene->GetEntityByUUID(managerID);
		scene->OnRuntimeStop();
		// Persistence lasts one play session.
		CHECK_FALSE(scene->IsPersistent(manager));
	}

	TEST_CASE("Scene.Load rejects bad paths and invalid files without touching the playing scene")
	{
		BasaltTest::TempProject project("SceneLoadErrors");
		WriteSwitchProject(project);
		project.WriteFile("Assets/Scenes/Broken.bscene", "{ not json");
		project.WriteFile("Assets/Scenes/BadEntity.bscene", R"({ "Format": "BasaltScene", "Version": 1, "Entities": [ { "ID": 1, "Name": "X", "Parent": 77 } ] })");
		project.WriteFile("Assets/Scenes/Notes.txt", "{}");
		std::string error;
		Ref<Scene> scene = SceneSerializer::LoadScene(project.GetDirectory() / "Assets/Scenes/A.bscene", error);
		REQUIRE(scene);

		// Only while playing.
		CHECK_FALSE(scene->RequestLoad("Assets/Scenes/B.bscene", error));
		CHECK(error.find("only be loaded while playing") != std::string::npos);

		scene->OnRuntimeStart();
		const std::string absolute = (project.GetDirectory() / "Assets/Scenes/B.bscene").generic_string();
		const std::pair<std::string, const char*> invalid[] = {
			{ "Assets/Scenes/Missing.bscene", "cannot read 'Assets/Scenes/Missing.bscene'" },
			{ "../Elsewhere/B.bscene", "must be a project-relative path inside the project" },
			{ "Assets/../../B.bscene", "must be a project-relative path inside the project" },
			{ absolute, "must be a project-relative path inside the project" },
			{ "", "must be a project-relative path" },
			{ "Assets/Scenes/Notes.txt", "is not a scene file (.bscene)" },
			{ "Assets/Scenes/Broken.bscene", "is not valid JSON" },
			{ "Assets/Scenes/BadEntity.bscene", "BadEntity.bscene: entity 'X' references unknown parent 77" },
		};
		for (const auto& [path, expected] : invalid)
		{
			CHECK_FALSE(scene->RequestLoad(path, error));
			CHECK_MESSAGE(error.find(expected) != std::string::npos, path, " -> ", error);
			CHECK_FALSE(scene->HasPendingLoad());
		}
		// The Lua error carries the same reason; a non-root cannot be made persistent.
		CHECK(Eval(*scene, "Scene.Load('Assets/Scenes/Missing.bscene')").find("Scene.Load: cannot read") != std::string::npos);
		CHECK(Eval(*scene, "Scene.DontDestroyOnLoad(Scene.FindEntityByName('ManagerChild'))").find("'ManagerChild' is not a root entity") != std::string::npos);
		CHECK_FALSE(scene->SetPersistent(scene->FindEntityByName("ManagerChild")));
		// Normalized paths inside the project are fine.
		CHECK(scene->RequestLoad("Assets/Scripts/../Scenes/B.bscene", error));
		scene->OnUpdate(BasaltTest::Step);
		CHECK(scene->GetPath() == "Assets/Scenes/B.bscene");
		CHECK(scene->FindEntityByName("LevelB"));
		scene->OnRuntimeStop();
	}

	TEST_CASE("Entities spawned by OnDestroy while a scene unloads are destroyed too")
	{
		BasaltTest::TempProject project("SceneLoadSpawnOnDestroy");
		WriteSwitchProject(project);
		project.WriteFile("Assets/Scripts/Spawner.lua", R"(
			local S = {}
			function S:OnDestroy()
				Destroyed = (Destroyed or 0) + 1
				local remains = Scene.CreateEntity("Remains")
				remains:AddComponent("Script", { Script = "Assets/Scripts/Spawner.lua" })
			end
			return S
		)");
		Scene scene;
		Entity spawner = scene.CreateEntity("Spawner");
		spawner.AddComponent<ScriptComponent>().Script = "Assets/Scripts/Spawner.lua";
		scene.OnRuntimeStart();
		std::string error;
		REQUIRE(scene.RequestLoad("Assets/Scenes/B.bscene", error));
		scene.OnUpdate(BasaltTest::Step);
		// Each unload pass spawns one more "Remains"; after MaxUnloadPasses the last one is kept, reported as a
		// script error that names it.
		CHECK(scene.FindEntitiesByName("Remains").size() == 1);
		REQUIRE(scene.GetScriptEngine()->GetErrors().size() == 1);
		CHECK(scene.GetScriptEngine()->GetErrors()[0].find("entities that OnDestroy keeps spawning while 'Untitled' unloads are kept in 'Assets/Scenes/B.bscene': 'Remains'") != std::string::npos);
		// The spawner and the remains of the first seven passes were destroyed.
		CHECK(Eval(scene, "Destroyed") == "8");
		CHECK(scene.FindEntityByName("LevelB"));
		scene.OnRuntimeStop();
	}

	TEST_CASE("A persistent entity parented under the level still survives a load")
	{
		BasaltTest::TempProject project("SceneLoadPersistentParent");
		WriteSwitchProject(project);
		std::string error;
		Ref<Scene> scene = SceneSerializer::LoadScene(project.GetDirectory() / "Assets/Scenes/A.bscene", error);
		REQUIRE(scene);
		scene->OnRuntimeStart();
		// Parent the persistent manager under level A, which the load destroys.
		CHECK(Eval(*scene, R"(
			local level = Scene.FindEntityByName("LevelA")
			level.Translation = Vec3(1, 2, 3)
			local manager = Scene.FindEntityByName("Manager")
			manager.Translation = Vec3(5, 0, 0)
			manager:SetParent(level)
			Scene.Load("Assets/Scenes/B.bscene")
		)") == "");
		// Still marked, under a level that is not.
		CHECK(scene->IsPersistent(scene->GetEntityByUUID(100)));
		scene->OnUpdate(BasaltTest::Step);
		Entity manager = scene->GetEntityByUUID(100);
		REQUIRE(manager);
		// Moved back to the root, where it was in the world, with its child.
		CHECK_FALSE(manager.GetParent());
		CHECK(manager.GetTransform().Translation == glm::vec3(5.0f, 0.0f, 0.0f));
		CHECK(scene->GetEntityByUUID(101));
		CHECK(scene->IsPersistent(manager));
		CHECK_FALSE(scene->FindEntityByName("LevelA"));
		CHECK(Eval(*scene, "ManagerCreates") == "1");
		scene->OnRuntimeStop();
	}

	TEST_CASE("Loads requested while a load is applied run at the end of the next update")
	{
		BasaltTest::TempProject project("SceneLoadChained");
		WriteSwitchProject(project);
		project.WriteFile("Assets/Scripts/Redirect.lua", R"(
			local R = {}
			function R:OnCreate() Scene.Load("Assets/Scenes/B.bscene") end
			return R
		)");
		project.WriteFile("Assets/Scripts/Leaving.lua", R"(
			local L = {}
			function L:OnDestroy() Scene.Load("Assets/Scenes/Splash.bscene") end
			return L
		)");
		project.WriteFile("Assets/Scenes/Splash.bscene", R"({ "Format": "BasaltScene", "Version": 1,
			"Entities": [ { "ID": 400, "Name": "Splash", "Parent": 0, "Components": { "Script": { "Script": "Assets/Scripts/Redirect.lua" } } } ] })");

		// A splash screen whose OnCreate moves on to the next scene.
		{
			Scene scene;
			scene.OnRuntimeStart();
			std::string error;
			REQUIRE(scene.RequestLoad("Assets/Scenes/Splash.bscene", error));
			scene.OnUpdate(BasaltTest::Step);
			CHECK(scene.GetName() == "Splash");
			CHECK(scene.HasPendingLoad());
			scene.OnUpdate(BasaltTest::Step);
			CHECK(scene.GetName() == "B");
			CHECK_FALSE(scene.HasPendingLoad());
			CHECK(scene.GetScriptEngine()->GetErrors().empty());
			scene.OnRuntimeStop();
		}
		// An OnDestroy that requests a load while the old scene unloads.
		{
			Scene scene;
			scene.CreateEntity("Leaving").AddComponent<ScriptComponent>().Script = "Assets/Scripts/Leaving.lua";
			scene.OnRuntimeStart();
			std::string error;
			REQUIRE(scene.RequestLoad("Assets/Scenes/A.bscene", error));
			scene.OnUpdate(BasaltTest::Step);
			CHECK(scene.GetName() == "A");
			CHECK(scene.HasPendingLoad());
			scene.OnUpdate(BasaltTest::Step);
			CHECK(scene.GetName() == "Splash");
			scene.OnRuntimeStop();
		}
	}

	TEST_CASE("A loaded scene starts its particles, cameras and scripts like a played one")
	{
		BasaltTest::TempProject project("SceneLoadStart");
		WriteSwitchProject(project);
		project.WriteFile("Assets/Scenes/Full.bscene", R"({ "Format": "BasaltScene", "Version": 1, "Name": "Named Differently",
			"Entities": [
				{ "ID": 500, "Name": "Sparks", "Parent": 0, "Components": { "ParticleEmitter": { "PlayOnStart": true } } },
				{ "ID": 501, "Name": "Idle", "Parent": 0, "Components": { "ParticleEmitter": { "PlayOnStart": false } } },
				{ "ID": 502, "Name": "Camera", "Parent": 0, "Components": { "Camera": {} } },
				{ "ID": 503, "Name": "Broken", "Parent": 0, "Components": { "Script": { "Script": "Assets/Scripts/Missing.lua" } } },
				{ "ID": 504, "Name": "Level", "Parent": 0, "Components": { "Script": { "Script": "Assets/Scripts/Level.lua" } } }
			] })");
		Scene scene;
		// Not 16:9 (the camera default), so a camera left at its default aspect would show.
		scene.OnViewportResize(800, 800);
		scene.OnRuntimeStart();
		std::string error;
		REQUIRE(scene.RequestLoad("Assets/Scenes/Full.bscene", error));
		scene.OnUpdate(BasaltTest::Step);

		// The file's "Name" wins over its file name; the path is the file's.
		CHECK(scene.GetName() == "Named Differently");
		CHECK(scene.GetPath() == "Assets/Scenes/Full.bscene");
		REQUIRE(scene.GetParticleSystem());
		CHECK(scene.GetParticleSystem()->IsPlaying(scene.FindEntityByName("Sparks")));
		CHECK_FALSE(scene.GetParticleSystem()->IsPlaying(scene.FindEntityByName("Idle")));
		CHECK(scene.FindEntityByName("Camera").GetComponent<CameraComponent>().Camera.GetAspectRatio() == doctest::Approx(1.0f));
		// A missing script is reported; the other new instances still start.
		REQUIRE(scene.GetScriptEngine()->GetErrors().size() == 1);
		CHECK(scene.GetScriptEngine()->GetErrors()[0].find("Missing.lua") != std::string::npos);
		CHECK(Eval(scene, "LevelCreates") == "1");
		scene.OnRuntimeStop();
	}

	TEST_CASE("Reloading a scene restarts it, deterministically")
	{
		BasaltTest::TempProject project("SceneLoadReload");
		WriteSwitchProject(project);
		project.WriteFile("Assets/Scripts/Ticker.lua", R"(
			local T = {}
			function T:OnCreate() self.Timer = Timer.Every(0.05, function() Ticks = (Ticks or 0) + 1 end) Tickers = (Tickers or {}) table.insert(Tickers, self.Timer) end
			return T
		)");
		project.WriteFile("Assets/Scenes/Ball.bscene", R"({ "Format": "BasaltScene", "Version": 1,
			"Entities": [
				{ "ID": 600, "Name": "Floor", "Parent": 0, "Components": { "RigidBody": {}, "BoxCollider": { "HalfExtents": [5, 0.5, 5] } } },
				{ "ID": 601, "Name": "Ball", "Parent": 0, "Components": { "Transform": { "Translation": [0, 3, 0] }, "RigidBody": { "Type": "Dynamic" }, "SphereCollider": {}, "Script": { "Script": "Assets/Scripts/Ticker.lua" } } }
			] })");

		auto run = [&](std::string& outHash) {
			std::string error;
			Ref<Scene> scene = SceneSerializer::LoadScene(project.GetDirectory() / "Assets/Scenes/Ball.bscene", error);
			REQUIRE(scene);
			scene->OnRuntimeStart();
			for (int reload = 0; reload < 2; reload++)
			{
				for (int i = 0; i < 30; i++)
					scene->OnUpdate(BasaltTest::Step);
				REQUIRE(scene->RequestLoad("Assets/Scenes/Ball.bscene", error));
				scene->OnUpdate(BasaltTest::Step);
			}
			for (int i = 0; i < 60; i++)
				scene->OnUpdate(BasaltTest::Step);
			// The same two entities each time; the old instances' timers ended with them.
			CHECK(scene->GetEntityCount() == 2);
			CHECK(Eval(*scene, "#Tickers .. ' ' .. tostring(Tickers[1]:IsActive()) .. tostring(Tickers[2]:IsActive()) .. tostring(Tickers[3]:IsActive())") == "3 falsefalsetrue");
			// The reloaded ball fell again and rests on the floor.
			const float height = scene->FindEntityByName("Ball").GetTransform().Translation.y;
			CHECK(height == doctest::Approx(1.0f).epsilon(0.05));
			CHECK(scene->GetScriptEngine()->GetErrors().empty());
			outHash = SceneSerializer::ComputeStateHash(*scene);
			scene->OnRuntimeStop();
		};
		std::string first;
		std::string second;
		run(first);
		run(second);
		CHECK(first == second);
	}

	TEST_CASE("Stopping play drops a pending load; a destroyed persistent entity is loaded again")
	{
		BasaltTest::TempProject project("SceneLoadStop");
		WriteSwitchProject(project);
		std::string error;
		Ref<Scene> scene = SceneSerializer::LoadScene(project.GetDirectory() / "Assets/Scenes/A.bscene", error);
		REQUIRE(scene);
		scene->OnRuntimeStart();
		REQUIRE(scene->RequestLoad("Assets/Scenes/B.bscene", error));
		scene->OnRuntimeStop();
		CHECK_FALSE(scene->HasPendingLoad());
		scene->OnRuntimeStart();
		scene->OnUpdate(BasaltTest::Step);
		CHECK(scene->GetName() == "A");

		// The persistent manager is destroyed; B's copy is then a new entity, not skipped.
		scene->DestroyEntity(scene->GetEntityByUUID(100));
		REQUIRE(scene->RequestLoad("Assets/Scenes/B.bscene", error));
		scene->OnUpdate(BasaltTest::Step);
		REQUIRE(scene->GetEntityByUUID(100));
		CHECK(scene->GetEntityByUUID(102));
		CHECK(Eval(*scene, "ManagerCreates") == "2");
		scene->OnRuntimeStop();
	}

	TEST_CASE("Saving records the scene's path; files outside the project keep an absolute one")
	{
		BasaltTest::TempProject project("SceneLoadPaths");
		Scene scene;
		std::string error;
		REQUIRE(SceneSerializer::SaveScene(scene, project.GetDirectory() / "Assets/Scenes/Saved.bscene", error));
		CHECK(scene.GetPath() == "Assets/Scenes/Saved.bscene");
		BasaltTest::TempDirectory outside("SceneLoadPathsOutside");
		const std::filesystem::path elsewhere = outside.GetPath() / "Elsewhere.bscene";
		REQUIRE(SceneSerializer::SaveScene(scene, elsewhere, error));
		CHECK(scene.GetPath() == elsewhere.lexically_normal().generic_string());
	}

	TEST_CASE("A persistent entity under another persistent entity stays where it is")
	{
		BasaltTest::TempProject project("SceneLoadNestedPersistent");
		WriteSwitchProject(project);
		std::string error;
		Ref<Scene> scene = SceneSerializer::LoadScene(project.GetDirectory() / "Assets/Scenes/A.bscene", error);
		REQUIRE(scene);
		scene->OnRuntimeStart();
		// A second persistent root, then the manager's child marked too after moving it under that root.
		CHECK(Eval(*scene, R"(
			local keeper = Scene.CreateEntity("Keeper")
			Scene.DontDestroyOnLoad(keeper)
			local child = Scene.FindEntityByName("ManagerChild")
			child:SetParent(keeper)
			Scene.Load("Assets/Scenes/B.bscene")
		)") == "");
		scene->OnUpdate(BasaltTest::Step);
		Entity child = scene->GetEntityByUUID(101);
		REQUIRE(child);
		CHECK(child.GetParent() == scene->FindEntityByName("Keeper"));
		scene->OnRuntimeStop();
	}
}

TEST_SUITE("Scene diff")
{
	TEST_CASE("DiffScenes matches entities by ID and reports field paths")
	{
		using json = nlohmann::json;
		const json from = { { "Format", "BasaltScene" }, { "Version", 1 }, { "Name", "A" }, { "Entities", { { { "ID", 1 }, { "Name", "One" }, { "Parent", 0 }, { "Components", { { "Transform", { { "Translation", { 0.0, 0.0, 0.0 } } } } } } }, { { "ID", 2 }, { "Name", "Two" }, { "Parent", 0 }, { "Components", json::object() } } } } };
		json to = from;
		to["Version"] = 2;
		to["Name"] = "B";
		to["Entities"][0]["Name"] = "Uno";
		to["Entities"][0]["Parent"] = 2;
		to["Entities"][0]["Components"]["Transform"]["Translation"] = { 0.0, 0.5, 0.0 };
		to["Entities"][1]["Components"]["Tag"] = { { "Tag", "x" } };
		std::string error;
		const std::optional<json> diff = DiffScenes(from, to, {}, error);
		REQUIRE(diff);
		CHECK((*diff)["equal"] == false);
		// Format and Version never count.
		CHECK((*diff)["settings"] == json::array({ { { "path", "Name" }, { "from", "A" }, { "to", "B" } } }));
		REQUIRE((*diff)["changed"].size() == 2);
		const json& one = (*diff)["changed"][0];
		CHECK(one["name"] == "Uno");
		CHECK(one["changes"] == json::array({ { { "path", "Name" }, { "from", "One" }, { "to", "Uno" } }, { { "path", "Parent" }, { "from", 0 }, { "to", 2 } }, { { "path", "Transform.Translation" }, { "from", { 0.0, 0.0, 0.0 } }, { "to", { 0.0, 0.5, 0.0 } } } }));
		CHECK((*diff)["changed"][1]["changes"] == json::array({ { { "path", "Tag" }, { "from", nullptr }, { "to", { { "Tag", "x" } } } } }));

		SceneDiffOptions loose;
		loose.Tolerance = 0.6;
		to["Name"] = "A";
		to["Entities"][0]["Name"] = "One";
		to["Entities"][0]["Parent"] = 0;
		to["Entities"][1]["Components"].erase("Tag");
		CHECK((*DiffScenes(from, to, loose, error))["equal"] == true);
		CHECK((*DiffScenes(from, to, {}, error))["equal"] == false);

		const std::string text = DescribeSceneDiff(*diff, 2);
		CHECK(text == "scene Name: \"A\" -> \"B\"\n'Uno' Name: \"One\" -> \"Uno\"\n(3 more)");
	}

	TEST_CASE("DiffScenes rejects input that is not a scene")
	{
		using json = nlohmann::json;
		std::string error;
		CHECK_FALSE(DiffScenes(json::array(), json::object(), {}, error));
		CHECK(error == "the first scene is not a JSON object");
		CHECK_FALSE(DiffScenes(json::object(), { { "Entities", 3 } }, {}, error));
		CHECK(error == "the second scene's Entities is not an array");
		CHECK_FALSE(DiffScenes({ { "Entities", { { { "Name", "x" } } } } }, json::object(), {}, error));
		CHECK(error == "entity #0 of the first scene has no numeric ID");
		CHECK_FALSE(DiffScenes(json::object(), { { "Entities", { { { "ID", 4 } }, { { "ID", 4 } } } } }, {}, error));
		CHECK(error == "the second scene has two entities with ID 4");
		// No entities at all is an empty scene.
		CHECK((*DiffScenes(json::object(), { { "Entities", nullptr } }, {}, error))["equal"] == true);
	}

	TEST_CASE("DiffScenes reports a different entity order, ignoring entities only one side has")
	{
		using json = nlohmann::json;
		const auto scene = [](std::initializer_list<int> ids) {
			json entities = json::array();
			for (int id : ids)
				entities.push_back({ { "ID", id }, { "Name", "E" + std::to_string(id) } });
			return json{ { "Entities", entities } };
		};
		std::string error;
		CHECK((*DiffScenes(scene({ 1, 2, 3 }), scene({ 1, 2, 3 }), {}, error))["reordered"] == false);
		CHECK((*DiffScenes(scene({ 1, 2, 3 }), scene({ 1, 3 }), {}, error))["reordered"] == false);
		CHECK((*DiffScenes(scene({ 1, 3 }), scene({ 4, 1, 2, 3 }), {}, error))["reordered"] == false);
		const json swapped = *DiffScenes(scene({ 1, 2, 3 }), scene({ 1, 3, 2 }), {}, error);
		CHECK(swapped["reordered"] == true);
		CHECK(swapped["equal"] == false);
		CHECK(DescribeSceneDiff(swapped) == "the entities are in a different order");
	}

	TEST_CASE("DescribeSceneDiff reads any JSON and cuts long values on character boundaries")
	{
		using json = nlohmann::json;
		CHECK(DescribeSceneDiff(json()).empty());
		CHECK(DescribeSceneDiff(json::array({ 1, 2 })).empty());
		CHECK(DescribeSceneDiff({ { "changed", { { { "name", 5 } }, 7 } }, { "added", { "x" } } }) == "added '' (null)");
		// The quote and 75 ASCII characters, then a two-byte character straddling the 77-byte cut.
		const std::string text = std::string(75, 'a') + "\xC3\xA9" + "tail";
		const std::string line = DescribeSceneDiff({ { "settings", { { { "path", "Name" }, { "from", text }, { "to", "b" } } } } });
		CHECK(line == "scene Name: \"" + std::string(75, 'a') + "... -> \"b\"");
	}

	TEST_CASE("JSON comparisons treat integers exactly and floats within the tolerance")
	{
		using json = nlohmann::json;
		// 2^53 + 1 is not representable as a double; IDs must still differ.
		CHECK_FALSE(JsonNearlyEqual(json(9007199254740993ull), json(9007199254740992ull), 1.0));
		CHECK(JsonNearlyEqual(json(1), json(1.05), 0.1));
		CHECK_FALSE(JsonNearlyEqual(json({ 1.0, 2.0 }), json({ 1.0 }), 1.0));
		CHECK_FALSE(JsonNearlyEqual(json({ { "a", 1 } }), json({ { "a", 1 }, { "b", 2 } }), 0.0));
		CHECK(JsonNearlyEqual(json("x"), json("x"), 0.0));
		CHECK_FALSE(JsonNearlyEqual(json("1"), json(1), 0.0));

		std::vector<std::string> mismatches;
		const json actual = { { "Color", { 1.0, 0.5, 0.25 } }, { "Intensity", 2.0 }, { "Extra", true }, { "Nested", { { "Inner", "a" } } } };
		MatchJsonSubset(actual, { { "Color", { 1.0, 0.5, 0.25 } }, { "Extra", true } }, 0.0, "Light", mismatches);
		CHECK(mismatches.empty());
		MatchJsonSubset(actual, { { "Color", { 1.0, 0.6, 0.25 } }, { "Intensity", 2.0 }, { "Missing", 1 }, { "Nested", { { "Inner", "b" } } }, { "Extra", { true } } }, 0.0, "Light", mismatches);
		CHECK(mismatches == std::vector<std::string>{ "Light.Color is [1.0,0.5,0.25], expected [1.0,0.6,0.25]", "Light.Extra is true, expected [true]", "Light.Missing is missing", "Light.Nested.Inner is \"a\", expected \"b\"" });
	}

	TEST_CASE("JsonContainsSubset agrees with MatchJsonSubset")
	{
		using json = nlohmann::json;
		const json id = json(std::numeric_limits<uint64_t>::max() - 1);
		struct Case
		{
			json Actual, Expected;
			double Tolerance;
			bool Contains;
		};
		const std::vector<Case> cases = {
			{ { { "a", 1 }, { "b", 2 } }, { { "a", 1 } }, 0.0, true },                                   // extra keys
			{ { { "a", 1 } }, { { "b", 1 } }, 0.0, false },                                              // a missing key
			{ { 1, 2 }, { 1 }, 0.0, false },                                                             // arrays of different length
			{ { { "a", 1 } }, 1, 0.0, false },                                                           // object against a scalar
			{ json::array({ { { "x", 1 }, { "y", 2 } } }), json::array({ { { "x", 1 } } }), 0.0, true }, // objects in arrays match as subsets
			{ 5, json::object(), 0.0, false },                                                           // {} against a non-object
			{ json::object(), json::object(), 0.0, true },
			{ id, id, 0.0, true },                                            // 64-bit integers exactly
			{ id, json(std::numeric_limits<uint64_t>::max()), 1.0e9, false }, // ...whatever the tolerance
			{ 1.0, 1.05, 0.1, true },                                         // floats within the tolerance
			{ 1, 1.05, 0.0, false },
			{ nullptr, nullptr, 0.0, true },
			{ "1", 1, 0.0, false },                                                       // string against a number
			{ { { "n", { 0.0, -1.0, 0.0 } } }, { { "n", { 0, -1, 0 } } }, 1.0e-4, true }, // integers in a pattern against floats
		};
		for (const Case& test : cases)
		{
			INFO(test.Actual.dump() << " contains " << test.Expected.dump());
			std::vector<std::string> mismatches;
			MatchJsonSubset(test.Actual, test.Expected, test.Tolerance, "", mismatches);
			CHECK(mismatches.empty() == test.Contains);
			CHECK(JsonContainsSubset(test.Actual, test.Expected, test.Tolerance) == test.Contains);
		}
	}
}

TEST_SUITE("Serialization")
{
	TEST_CASE("Every component field serializes as the type the registry declares")
	{
		using json = nlohmann::json;
		Scene scene;
		Entity entity = scene.CreateEntity("Everything");
		size_t fields = 0;
		for (const ComponentInfo& info : ComponentRegistry::GetAll())
		{
			INFO("component ", info.Name);
			info.Add(entity);
			const json data = info.Serialize(entity);
			fields += info.Fields.size();
			for (const std::string& field : info.Fields)
			{
				INFO("field ", field);
				REQUIRE(data.contains(field));
				const ComponentFieldType& type = info.FieldTypes.at(field);
				const auto options = info.EnumOptions.find(field);
				const std::vector<std::string>* enumOptions = options == info.EnumOptions.end() ? nullptr : &options->second;
				if (type.Count == 0)
				{
					CHECK(MatchesFieldKind(data[field], type.Kind, enumOptions));
					continue;
				}
				REQUIRE(data[field].is_array());
				CHECK(data[field].size() == type.Count);
				for (const json& element : data[field])
					CHECK(MatchesFieldKind(element, type.Kind, enumOptions));
			}
		}
		// Collecting the field lists works (an empty list would make every check above pass).
		CHECK(fields > 150);
	}
}
