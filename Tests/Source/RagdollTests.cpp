#include <doctest/doctest.h>

#include "AnimationTestUtils.h"
#include "TestUtils.h"

#include <Basalt/Physics/PhysicsWorld.h>
#include <Basalt/Physics/Ragdoll.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/SceneSerializer.h>
#include <Basalt/Scene/SceneEventListener.h>

#include <limits>

using namespace Basalt;
using namespace BasaltTest;
using namespace AnimationTest;
using json = nlohmann::json;

namespace {
	Entity Rig(Scene& scene, bool enabled = false)
	{
		Entity entity = scene.CreateEntity("Ragdoll");
		auto& animator = entity.AddComponent<AnimatorComponent>();
		animator.Mesh = FixturePath("TwoJoint.gltf").string();
		animator.PlayOnStart = false;
		entity.GetTransform().Translation = { 0, 4, 0 };
		entity.AddComponent<RagdollComponent>().Enabled = enabled;
		return entity;
	}
	Entity Floor(Scene& scene)
	{
		Entity floor = scene.CreateEntity("Floor");
		floor.GetTransform().Translation.y = -0.5f;
		floor.AddComponent<RigidBodyComponent>();
		floor.AddComponent<BoxColliderComponent>().HalfExtents = { 20, 0.5f, 20 };
		return floor;
	}
	struct Contacts final : SceneEventListener
	{
		int Begin = 0;
		int End = 0;
		bool SelfContact = false;
		void OnContact(Scene&, ContactEventType type, Entity first, Entity second, const std::optional<ContactInfo>&) override
		{
			SelfContact = SelfContact || first == second;
			if (type == ContactEventType::TriggerEnter)
				Begin++;
			if (type == ContactEventType::TriggerExit)
				End++;
		}
		void OnAnimationEvent(Scene&, Entity, const std::string&, const std::string&) override {}
		void OnScriptError(Scene&, const std::string&) override {}
	};
	void Simulate(Scene& scene, int frames)
	{
		for (int i = 0; i < frames; i++)
			scene.OnUpdate(Step);
	}
}

TEST_SUITE("Ragdolls")
{
	TEST_CASE("Ragdoll registry validates overrides atomically and copies configuration")
	{
		auto original = CreateRef<Scene>();
		Scene& scene = *original;
		Entity entity = Rig(scene);
		const auto* info = ComponentRegistry::Find("Ragdoll");
		REQUIRE(info);
		CHECK_FALSE(info->SupportsPropertyAnimation);
		std::string error;
		const json valid = { { "Enabled", true }, { "Bones", { { { "Joint", "Tip" }, { "Radius", 0.1 }, { "Length", 0.4 }, { "SwingAngle", 20 }, { "TwistMin", -10 } } } } };
		REQUIRE(info->Deserialize(entity, valid, error));
		const auto before = info->Serialize(entity);
		for (const json& invalid : { json{ { "Mass", 0 } }, json{ { "RadiusRatio", 1 } }, json{ { "RecoveryTime", -1 } }, json{ { "Layer", "" } }, json{ { "Bones", { { { "Joint", "Tip" }, { "Radius", -1 } } } } }, json{ { "Bones", { { { "Joint", "Tip" }, { "SwingAngle", 181 } } } } }, json{ { "Bones", { { { "Joint", "Tip" }, { "TwistMin", 1 } } } } }, json{ { "Bones", { { { "Joint", "Tip" } }, { { "Joint", "Tip" } } } } } })
		{
			CHECK_FALSE(info->Deserialize(entity, invalid, error));
			CHECK_FALSE(error.empty());
			CHECK(info->Serialize(entity) == before);
		}
		auto copied = Scene::Copy(original);
		REQUIRE(copied);
		CHECK(copied->GetEntityByUUID(entity.GetUUID()).GetComponent<RagdollComponent>() == entity.GetComponent<RagdollComponent>());
		const auto duplicate = scene.DuplicateEntity(entity);
		CHECK(duplicate.GetComponent<RagdollComponent>() == entity.GetComponent<RagdollComponent>());
		auto& component = entity.GetComponent<RagdollComponent>();
		component.Mass = std::numeric_limits<float>::quiet_NaN();
		CHECK_FALSE(ValidateRagdoll(component).empty());
		component.Mass = 70;
		component.Bones.resize(RagdollComponent::MaxBones + 1);
		CHECK_FALSE(ValidateRagdoll(component).empty());
	}

	TEST_CASE("Ragdoll starts from the shown pose and falls onto collision geometry")
	{
		ClearAssetCache clear;
		Scene scene;
		Floor(scene);
		Entity rig = Rig(scene);
		Entity attachment = scene.CreateEntity("Marker");
		auto& bone = attachment.AddComponent<BoneAttachmentComponent>();
		bone.Target = rig.GetUUID();
		bone.Joint = "Tip";
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		auto& animation = *scene.GetAnimationSystem();
		animation.Update(0);
		const glm::vec3 initial = BoneWorld(scene, rig, "RootJoint");
		REQUIRE(physics.ActivateRagdoll(rig));
		CHECK(physics.GetRagdollBodyCount(rig) == 2);
		const auto capsules = physics.GetRagdollCapsules(rig);
		REQUIRE(capsules.size() == 2);
		CHECK(capsules[0].Radius == doctest::Approx(0.15f));
		CHECK(glm::length(capsules[0].End - capsules[0].Start) == doctest::Approx(0.7f));
		animation.LateUpdate(0);
		CHECK(Near(BoneWorld(scene, rig, "RootJoint"), initial));
		Simulate(scene, 30);
		CHECK(BoneWorld(scene, rig, "RootJoint").y < initial.y - 0.5f);
		CHECK(glm::length(BoneWorld(scene, rig, "Tip") - BoneWorld(scene, rig, "RootJoint")) == doctest::Approx(1).epsilon(0.02));
		CHECK(Near(glm::vec3(scene.GetWorldTransform(attachment)[3]), BoneWorld(scene, rig, "Tip")));
		// A callback that samples a different animation keeps the physical overlay until the next physics update.
		const auto physical = *animation.GetPose(rig);
		REQUIRE(animation.Play(rig, "Bend"));
		CHECK(NearPose(*animation.GetPose(rig), physical));
		Simulate(scene, 330);
		const float rootY = BoneWorld(scene, rig, "RootJoint").y;
		CHECK(rootY > -0.1f);
		CHECK(rootY < 2.0f);
		CHECK(physics.IsRagdollActive(rig));
		scene.OnSimulationStop();
	}

	TEST_CASE("Ragdoll total impulse and inherited body velocity move the skeleton")
	{
		ClearAssetCache clear;
		Scene scene;
		scene.GetPhysicsSettings().Gravity = glm::vec3(0);
		Entity rig = Rig(scene);
		rig.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		rig.AddComponent<CapsuleColliderComponent>();
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		physics.SetLinearVelocity(rig, { 2, 0, 0 });
		REQUIRE(physics.ActivateRagdoll(rig, { 70, 0, 0 }));
		CHECK_FALSE(physics.HasBody(rig));
		Simulate(scene, 60);
		CHECK(BoneWorld(scene, rig, "RootJoint").x == doctest::Approx(3).epsilon(0.06));
		REQUIRE(physics.ActivateRagdoll(rig, { 70, 0, 0 }));
		const float x = BoneWorld(scene, rig, "RootJoint").x;
		Simulate(scene, 60);
		CHECK(BoneWorld(scene, rig, "RootJoint").x - x == doctest::Approx(4).epsilon(0.06));
		scene.OnSimulationStop();
	}

	TEST_CASE("Ragdoll recovery freezes simulation and blends to a chosen clip at the fallen location")
	{
		ClearAssetCache clear;
		Scene scene;
		Entity rig = Rig(scene);
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		REQUIRE(physics.ActivateRagdoll(rig, { 70, 0, 0 }));
		Simulate(scene, 30);
		const glm::vec3 fallen = BoneWorld(scene, rig, "RootJoint");
		CHECK_FALSE(physics.RecoverRagdoll(rig, "Missing", 0.5f));
		CHECK(physics.IsRagdollActive(rig));
		REQUIRE(physics.RecoverRagdoll(rig, "Bend", 0.5f));
		CHECK_FALSE(physics.IsRagdollActive(rig));
		CHECK(physics.GetRagdollBodyCount(rig) == 0);
		CHECK(physics.HasRagdollPose(rig));
		scene.GetAnimationSystem()->LateUpdate(0);
		CHECK(Near(BoneWorld(scene, rig, "RootJoint"), fallen));
		CHECK(rig.GetTransform().Translation.x == doctest::Approx(fallen.x).epsilon(1e-4));
		Simulate(scene, 15);
		CHECK(BoneWorld(scene, rig, "RootJoint").y > fallen.y);
		Simulate(scene, 20);
		CHECK_FALSE(physics.HasRagdollPose(rig));
		CHECK(BoneWorld(scene, rig, "RootJoint").y == doctest::Approx(4));
		scene.OnSimulationStop();
	}

	TEST_CASE("Ragdoll component edits remove bodies and restore a character after recovery")
	{
		ClearAssetCache clear;
		Scene scene;
		Entity rig = Rig(scene, true);
		rig.AddComponent<CharacterControllerComponent>();
		rig.AddComponent<CapsuleColliderComponent>();
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		Simulate(scene, 1);
		REQUIRE(physics.IsRagdollActive(rig));
		CHECK_FALSE(physics.HasCharacter(rig));
		rig.GetComponent<RagdollComponent>().RecoveryTime = 0.25f;
		rig.GetComponent<RagdollComponent>().Enabled = false;
		Simulate(scene, 2);
		CHECK(physics.HasRagdollPose(rig));
		CHECK_FALSE(physics.IsRagdollActive(rig));
		CHECK(physics.HasCharacter(rig));
		REQUIRE(physics.ActivateRagdoll(rig));
		rig.RemoveComponent<RagdollComponent>();
		Simulate(scene, 2);
		CHECK_FALSE(physics.HasRagdollPose(rig));
		CHECK(physics.HasCharacter(rig));
		rig.AddComponent<RagdollComponent>().Enabled = true;
		Simulate(scene, 1);
		REQUIRE(physics.IsRagdollActive(rig));
		scene.DestroyEntity(rig);
		Simulate(scene, 1);
		CHECK(physics.GetCharacterCount() == 0);
		scene.OnSimulationStop();
	}

	TEST_CASE("Ragdoll rejects invalid skeletons settings transforms and foreign entities")
	{
		ClearAssetCache clear;
		Scene scene, other;
		Entity rig = Rig(scene);
		Entity foreign = Rig(other);
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		CHECK_FALSE(physics.ActivateRagdoll(foreign));
		CHECK_FALSE(physics.ActivateRagdoll(scene.CreateEntity("Plain")));
		CHECK_FALSE(physics.ActivateRagdoll(rig, { std::numeric_limits<float>::infinity(), 0, 0 }));
		for (const glm::vec3& scale : { glm::vec3(0), glm::vec3(-1, 1, 1), glm::vec3(1, 2, 1) })
		{
			rig.GetTransform().Scale = scale;
			CHECK_FALSE(physics.ActivateRagdoll(rig));
		}
		rig.GetTransform().Scale = glm::vec3(1);
		auto& config = rig.GetComponent<RagdollComponent>();
		config.RootJoint = "Missing";
		CHECK_FALSE(physics.ActivateRagdoll(rig));
		config.RootJoint.clear();
		config.Bones.push_back({ .Joint = "Missing" });
		CHECK_FALSE(physics.ActivateRagdoll(rig));
		config.Bones.clear();
		config.Layer = "Missing";
		CHECK_FALSE(physics.ActivateRagdoll(rig));
		config.Layer = "Default";
		config.RootJoint = "Tip";
		REQUIRE(physics.ActivateRagdoll(rig));
		CHECK(physics.GetRagdollBodyCount(rig) == 1);
		CHECK_FALSE(physics.RecoverRagdoll(rig, {}, -1));
		CHECK_FALSE(physics.RecoverRagdoll(rig, {}, std::numeric_limits<float>::quiet_NaN()));
		CHECK(physics.IsRagdollActive(rig));
		scene.OnSimulationStop();
	}

	TEST_CASE("Ragdoll contacts aggregate every limb and queries report the owner once")
	{
		ClearAssetCache clear;
		Scene scene;
		Contacts contacts;
		scene.SetEventListener(&contacts);
		Entity rig = Rig(scene, true);
		Entity zone = scene.CreateEntity("Zone");
		zone.GetTransform().Translation = { 0, 4, 0 };
		zone.AddComponent<RigidBodyComponent>().IsTrigger = true;
		zone.AddComponent<BoxColliderComponent>().HalfExtents = { 3, 2, 3 };
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		Simulate(scene, 1);
		CHECK(contacts.Begin == 1);
		PhysicsQueryFilter filter;
		filter.IgnoreEntity = zone.GetUUID();
		CHECK(physics.OverlapSphere({ 0, 4, 0 }, 5, filter) == std::vector<UUID>{ rig.GetUUID() });
		filter.IgnoreEntity = rig.GetUUID();
		CHECK(physics.OverlapSphere({ 0, 4, 0 }, 5, filter).empty());
		// The lower part leaves first; the owner must remain inside until its final part leaves.
		bool sawPartial = false;
		for (int i = 0; i < 100; i++)
		{
			Simulate(scene, 1);
			const float root = BoneWorld(scene, rig, "RootJoint").y;
			const float tip = BoneWorld(scene, rig, "Tip").y;
			if (root + 1.0f < 2 && tip + 0.5f > 2)
			{
				sawPartial = true;
				CHECK(contacts.End == 0);
			}
		}
		CHECK(sawPartial);
		CHECK(contacts.Begin == 1);
		CHECK(contacts.End == 1);
		CHECK_FALSE(contacts.SelfContact);
		scene.OnSimulationStop();
		scene.SetEventListener(nullptr);
	}

	TEST_CASE("Ragdoll teleports scale changes reloads and repeated toggles retain no orphan bodies")
	{
		ClearAssetCache clear;
		Scene scene;
		scene.GetPhysicsSettings().Gravity = glm::vec3(0);
		Entity rig = Rig(scene, true);
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		Simulate(scene, 1);
		const glm::vec3 old = BoneWorld(scene, rig, "Tip");
		rig.GetTransform().Translation.x += 10;
		Simulate(scene, 1);
		CHECK(Near(BoneWorld(scene, rig, "Tip"), old + glm::vec3(10, 0, 0)));
		rig.GetTransform().Scale = glm::vec3(2);
		Simulate(scene, 1);
		CHECK(physics.GetRagdollBodyCount(rig) == 2);
		CHECK(glm::length(BoneWorld(scene, rig, "Tip") - BoneWorld(scene, rig, "RootJoint")) == doctest::Approx(2));
		AssetManager::Clear();
		Simulate(scene, 1);
		CHECK(physics.GetBodyCount() == 2);
		for (int i = 0; i < 10; i++)
		{
			REQUIRE(physics.RecoverRagdoll(rig, {}, 0));
			Simulate(scene, 1);
			CHECK(physics.GetBodyCount() == 0);
			REQUIRE(physics.ActivateRagdoll(rig));
			CHECK(physics.GetBodyCount() == 2);
		}
		rig.RemoveComponent<AnimatorComponent>();
		Simulate(scene, 1);
		CHECK(physics.GetBodyCount() == 0);
		scene.OnSimulationStop();
	}

	TEST_CASE("Ragdoll limits constrain relative rotations under collisions and uniform parent transforms")
	{
		ClearAssetCache clear;
		Scene scene;
		Floor(scene);
		Entity parent = scene.CreateEntity("Parent");
		parent.GetTransform().Rotation = AxisAngle({ 0, 0, 1 }, 35);
		parent.GetTransform().Scale = glm::vec3(1.5f);
		Entity rig = Rig(scene, true);
		REQUIRE(scene.SetParent(rig, parent, false));
		rig.GetComponent<RagdollComponent>().Bones = { { .Joint = "Tip", .Radius = 0.1f, .Length = 0.4f, .SwingAngle = 0, .TwistMin = 0, .TwistMax = 0 } };
		scene.OnSimulationStart();
		for (int i = 0; i < 180; i++)
		{
			Simulate(scene, 1);
			CHECK(scene.GetPhysicsWorld()->IsRagdollActive(rig));
			glm::vec3 p, s;
			glm::quat root, tip;
			REQUIRE(Math::DecomposeTransform(BoneWorldTransform(scene, rig, "RootJoint"), p, root, s));
			REQUIRE(Math::DecomposeTransform(BoneWorldTransform(scene, rig, "Tip"), p, tip, s));
			CHECK(NearRotation(root, tip, 0.001f));
		}
		scene.OnSimulationStop();
	}

	TEST_CASE("Ragdoll takes over a full biped with IK springs layers and root motion")
	{
		ClearAssetCache clear;
		Scene scene;
		Floor(scene);
		Entity rig = Rig(scene, true);
		auto& animator = rig.GetComponent<AnimatorComponent>();
		animator.Mesh = BipedPath().string();
		animator.Clip = "Lean";
		animator.RootMotion = true;
		animator.RootJoint = "Hips";
		rig.AddComponent<InverseKinematicsComponent>().Chains.push_back({ .Name = "Head", .Type = IKChainType::LookAt, .Joint = "Head" });
		rig.AddComponent<SpringBoneComponent>().Chains.push_back({ .Name = "Arm", .Joint = "RightUpperArm" });
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		REQUIRE(physics.IsRagdollActive(rig));
		CHECK(physics.GetRagdollBodyCount(rig) > 10);
		const auto source = LoadBiped();
		const glm::vec3 start = BoneWorld(scene, rig, "Hips");
		for (int i = 0; i < 60; i++)
		{
			Simulate(scene, 1);
			std::vector<std::pair<uint32_t, glm::mat4>> joints;
			float weight = 0;
			const auto view = scene.GetAnimationSystem()->GetPoseView(rig);
			REQUIRE(view);
			REQUIRE(physics.GetRagdollPose(rig, *view->Source, joints, weight));
			for (const auto& [node, matrix] : joints)
				CHECK(Near(glm::vec3(view->Globals[node][3]), glm::vec3(matrix[3]), 0.002f));
		}
		CHECK(BoneWorld(scene, rig, "Hips").y < start.y - 1);
		REQUIRE(physics.RecoverRagdoll(rig, "Lean", 0.25f));
		Simulate(scene, 20);
		CHECK_FALSE(physics.HasRagdollPose(rig));
		CHECK(scene.GetAnimationSystem()->GetPose(rig)->size() == source->Nodes.size());
		scene.OnSimulationStop();
	}

	TEST_CASE("Ragdoll state hashes see physical joints while saves retain only configuration")
	{
		ClearAssetCache clear;
		Scene scene;
		Entity rig = Rig(scene, true);
		scene.OnSimulationStart();
		Simulate(scene, 1);
		const std::string firstHash = SceneSerializer::ComputeStateHash(scene);
		const glm::vec3 entityPosition = rig.GetTransform().Translation;
		Simulate(scene, 30);
		CHECK(rig.GetTransform().Translation == entityPosition);
		CHECK(SceneSerializer::ComputeStateHash(scene) != firstHash);
		CHECK(SceneSerializer::ComputeStateHash(scene) == SceneSerializer::ComputeStateHash(scene));
		const auto saved = SceneSerializer::SerializeScene(scene);
		CHECK_FALSE(saved["Entities"][0]["Components"]["Ragdoll"].contains("Runtime"));
		scene.OnSimulationStop();
	}

	TEST_CASE("Ragdoll Lua calls work without exposing runtime bodies in serialized data")
	{
		ClearAssetCache clear;
		Scene scene;
		Entity rig = Rig(scene);
		scene.OnRuntimeStart();
		CHECK(Eval(scene, "return Scene.FindEntityByName('Ragdoll'):ActivateRagdoll(Vec3(1, 0, 0))") == "true");
		CHECK(Eval(scene, "return Scene.FindEntityByName('Ragdoll'):IsRagdollActive()") == "true");
		CHECK(Eval(scene, "return Scene.FindEntityByName('Ragdoll'):RecoverRagdoll('Bend', 0.1)") == "true");
		CHECK(Eval(scene, "return Scene.FindEntityByName('Ragdoll'):IsRagdollActive()") == "false");
		const auto* info = ComponentRegistry::Find("Ragdoll");
		REQUIRE(info);
		CHECK(info->Serialize(rig).size() == 7);
		scene.OnRuntimeStop();
		CHECK(rig.GetComponent<RagdollComponent>().Enabled == false);
	}
}
