#include <doctest/doctest.h>

#include <Basalt/Core/Log.h>
#include <Basalt/Physics/PhysicsWorld.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>
#include <Basalt/Scene/SceneSerializer.h>

#include "TestUtils.h"

#include <glm/gtc/epsilon.hpp>

using namespace Basalt;

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
		entity.GetComponent<MaterialComponent>().AlbedoColor = { 0.1f, 0.2f, 0.3f, 0.4f };
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
				  .MotorMode = JointMotorMode::Velocity,
				  .MotorTarget = 12.0f,
				  .MotorMaxForce = 250.0f,
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
		CHECK(copy.GetComponent<RigidBodyComponent>().Type == RigidBodyType::Kinematic);
		CHECK(Near(glm::degrees(copy.GetTransform().GetRotationEuler()), { 10.0f, 20.0f, 30.0f }, 1e-3f));
		CHECK(copy.GetComponent<JointComponent>() == entity.GetComponent<JointComponent>());
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
		scene->GetPhysicsSettings().Gravity = { 0.0f, -3.0f, 0.0f };

		const auto path = project.GetDirectory() / "Assets/Scenes/Level1.bscene";
		std::string error;
		REQUIRE(SceneSerializer::SaveScene(*scene, path, error));

		Ref<Scene> loaded = SceneSerializer::LoadScene(path, error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->GetName() == "Level1");
		CHECK(loaded->GetRendererSettings().Tonemap == Tonemapper::AgX);
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
		uint64_t next = 0;
		int warnings = 0;
		for (const LogMessage& message : Log::GetHistory().GetMessagesSince(since, next))
			warnings += message.Text.find("before named physics layers") != std::string::npos ? 1 : 0;
		CHECK(warnings <= 1);

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
			else
			{
				CHECK(info.EntityFields.empty());
				CHECK_FALSE(info.RemapEntityReferences);
			}
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
