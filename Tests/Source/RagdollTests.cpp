#include <doctest/doctest.h>

#include "AnimationTestUtils.h"
#include "TestUtils.h"

#include <Basalt/Physics/PhysicsWorld.h>
#include <Basalt/Physics/RagdollRules.h>
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
		int CollisionBegin = 0;
		int CollisionEnd = 0;
		bool SelfContact = false;
		void OnContact(Scene&, ContactEventType type, Entity first, Entity second, const std::optional<ContactInfo>&) override
		{
			SelfContact = SelfContact || first == second;
			if (type == ContactEventType::CollisionBegin)
				CollisionBegin++;
			if (type == ContactEventType::CollisionEnd)
				CollisionEnd++;
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
		scene.GetPhysicsSettings().Gravity = glm::vec3(0);
		Entity rig = Rig(scene, true);
		rig.AddComponent<CharacterControllerComponent>();
		rig.AddComponent<CapsuleColliderComponent>();
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		Simulate(scene, 1);
		REQUIRE(physics.IsRagdollActive(rig));
		CHECK_FALSE(physics.HasCharacter(rig));
		REQUIRE(physics.ActivateRagdoll(rig, { 70, 0, 0 }));
		Simulate(scene, 30);
		const glm::vec3 fallenRoot = BoneWorld(scene, rig, "RootJoint");
		CHECK(fallenRoot.x > 0.4f);
		rig.GetComponent<RagdollComponent>().RecoveryTime = 0.25f;
		rig.GetComponent<RagdollComponent>().Enabled = false;
		Simulate(scene, 2);
		CHECK(physics.HasRagdollPose(rig));
		CHECK_FALSE(physics.IsRagdollActive(rig));
		CHECK(physics.HasCharacter(rig));
		CHECK(rig.GetTransform().Translation.x == doctest::Approx(fallenRoot.x).epsilon(0.001));
		REQUIRE(physics.ActivateRagdoll(rig));
		rig.RemoveComponent<RagdollComponent>();
		CHECK_FALSE(physics.HasRagdollPose(rig));
		CHECK(physics.GetRagdollBodyCount(rig) == 0);
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
		const auto oldSource = scene.GetAnimationSystem()->GetPoseView(rig)->Source;
		AssetManager::Clear();
		Simulate(scene, 1);
		std::vector<std::pair<uint32_t, glm::mat4>> stale;
		float oldWeight = 0;
		CHECK_FALSE(physics.GetRagdollPose(rig, *oldSource, stale, oldWeight));
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

	TEST_CASE("Contact exit callbacks can recover remove or destroy an activating ragdoll")
	{
		ClearAssetCache clear;
		TempDirectory directory("RagdollCallbacks");
		const auto script = directory.GetPath() / "Cancel.lua";
		REQUIRE(FileSystem::WriteTextFile(script, R"(
			return { OnTriggerExit = function(self, other)
				if self.Entity:IsRagdollActive() then
					if CancelAction == 0 then Recovered = self.Entity:RecoverRagdoll("", 0.5)
					elseif CancelAction == 1 then self.Entity:RemoveComponent("Ragdoll")
					elseif CancelAction == 2 then self.Entity:RemoveComponent("Animator")
					else self.Entity:Destroy() end
				end
			end }
		)"));
		for (int action = 0; action < 4; action++)
		{
			INFO(action);
			Scene scene;
			scene.GetPhysicsSettings().Gravity = glm::vec3(0);
			Entity rig = Rig(scene);
			rig.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
			rig.AddComponent<SphereColliderComponent>();
			rig.AddComponent<ScriptComponent>().Script = script.string();
			Entity zone = scene.CreateEntity("Zone");
			zone.GetTransform().Translation = { 0, 4, 0 };
			zone.AddComponent<RigidBodyComponent>().IsTrigger = true;
			zone.AddComponent<BoxColliderComponent>();
			scene.OnRuntimeStart();
			Eval(scene, "CancelAction = " + std::to_string(action));
			scene.OnUpdate(Step);
			auto& physics = *scene.GetPhysicsWorld();
			CHECK_FALSE(physics.ActivateRagdoll(rig));
			CHECK_FALSE(physics.IsRagdollActive(rig));
			if (action == 0)
			{
				CHECK(Eval(scene, "return Recovered") == "true");
				CHECK_FALSE(rig.GetComponent<RagdollComponent>().Enabled);
			}
			else
				CHECK_FALSE(physics.HasRagdollPose(rig));
			scene.OnRuntimeStop();
		}
	}

	TEST_CASE("Get-up matches the incoming clip root and keeps the frozen pose continuous")
	{
		ClearAssetCache clear;
		Scene scene;
		scene.GetPhysicsSettings().Gravity = glm::vec3(0);
		Entity parent = scene.CreateEntity("Parent");
		parent.GetTransform().Rotation = AxisAngle({ 0, 1, 0 }, 35);
		parent.GetTransform().Scale = glm::vec3(2);
		Entity rig = Rig(scene);
		REQUIRE(scene.SetParent(rig, parent, false));
		scene.OnSimulationStart();
		auto& animation = *scene.GetAnimationSystem();
		auto& physics = *scene.GetPhysicsWorld();
		REQUIRE(animation.Play(rig, "Step"));
		REQUIRE(animation.Seek(rig, 1));
		animation.Pause(rig, true);
		const glm::vec3 animated = BoneWorld(scene, rig, "RootJoint");
		REQUIRE(physics.ActivateRagdoll(rig));
		Simulate(scene, 1);
		const glm::vec3 fallen = BoneWorld(scene, rig, "RootJoint");
		CHECK(Near(fallen, animated));
		REQUIRE(physics.RecoverRagdoll(rig, "Bend", 0.5f));
		CHECK(Near(BoneWorld(scene, rig, "RootJoint"), fallen));
		Simulate(scene, 15);
		const auto pose = physics.GetRagdollPose(rig);
		REQUIRE(pose);
		CHECK(pose->Weight == doctest::Approx(0.5f).epsilon(0.001));
		// A caller adjusting the ground height moves the entire frozen pose along with its animation.
		rig.GetTransform().Translation.y += 2;
		Simulate(scene, 1);
		CHECK(BoneWorld(scene, rig, "RootJoint").y == doctest::Approx(fallen.y + 4).epsilon(0.002));
		Simulate(scene, 30);
		CHECK_FALSE(physics.HasRagdollPose(rig));
		const glm::vec3 recovered = BoneWorld(scene, rig, "RootJoint");
		CHECK(recovered.x == doctest::Approx(fallen.x).epsilon(0.001));
		CHECK(recovered.z == doctest::Approx(fallen.z).epsilon(0.001));
		REQUIRE(physics.ActivateRagdoll(rig));
		REQUIRE(physics.RecoverRagdoll(rig, {}, 0));
		CHECK_FALSE(physics.HasRagdollPose(rig));
		scene.OnSimulationStop();
	}

	TEST_CASE("Activation transfers angular motion and rebuilds preserve physical position and spin")
	{
		ClearAssetCache clear;
		Scene scene;
		scene.GetPhysicsSettings().Gravity = glm::vec3(0);
		Entity rig = Rig(scene);
		rig.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		rig.AddComponent<SphereColliderComponent>();
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		physics.SetLinearVelocity(rig, { 3, 0, 0 });
		physics.SetAngularVelocity(rig, { 0, 0, 1 });
		REQUIRE(physics.ActivateRagdoll(rig));
		Simulate(scene, 10);
		const glm::mat4 before = BoneWorldTransform(scene, rig, "RootJoint");
		CHECK(std::abs(before[0][1]) > 0.1f);
		CHECK(glm::vec3(before[3]).x > 0.3f);
		for (int change = 0; change < 3; change++)
		{
			const glm::mat4 old = BoneWorldTransform(scene, rig, "RootJoint");
			if (change == 0)
				rig.GetComponent<RagdollComponent>().Mass = 90;
			else if (change == 1)
				rig.GetTransform().Scale = glm::vec3(2);
			else
				AssetManager::Clear();
			Simulate(scene, 1);
			const glm::mat4 after = BoneWorldTransform(scene, rig, "RootJoint");
			CHECK(after[3].x > old[3].x + 0.01f);
			CHECK(glm::length(glm::vec3(after[3] - old[3])) < 0.2f);
			CHECK(std::abs(after[0][1] / glm::length(glm::vec3(after[0]))) > 0.1f);
		}
		const glm::vec3 fall = BoneWorld(scene, rig, "RootJoint");
		REQUIRE(physics.RecoverRagdoll(rig, "Bend", 0));
		Simulate(scene, 1);
		CHECK(physics.HasBody(rig));
		CHECK(rig.GetTransform().Translation.x == doctest::Approx(fall.x).epsilon(0.001));
		scene.OnSimulationStop();
	}

	TEST_CASE("Invalid structural rigs wait for edits or reload instead of rebuilding every frame")
	{
		ClearAssetCache clear;
		Scene scene;
		Entity rig = Rig(scene, true);
		rig.GetComponent<RagdollComponent>().RootJoint = "Missing";
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		const uint64_t attempts = physics.GetRagdollBuildCount();
		Simulate(scene, 30);
		CHECK(physics.GetRagdollBuildCount() == attempts);
		AssetManager::Clear();
		Simulate(scene, 1);
		CHECK(physics.GetRagdollBuildCount() == attempts + 1);
		rig.GetComponent<RagdollComponent>().RootJoint.clear();
		Simulate(scene, 1);
		REQUIRE(physics.IsRagdollActive(rig));
		rig.RemoveComponent<AnimatorComponent>();
		CHECK(physics.GetBodyCount() == 0);
		CHECK_FALSE(physics.GetRagdollPose(rig));
		scene.OnSimulationStop();
	}

	TEST_CASE("Sleeping ragdoll contacts survive until separation and stop emits no contact exits")
	{
		ClearAssetCache clear;
		Scene scene;
		Contacts contacts;
		scene.SetEventListener(&contacts);
		Floor(scene);
		Entity rig = Rig(scene, true);
		scene.OnSimulationStart();
		Simulate(scene, 600);
		const auto pose = scene.GetPhysicsWorld()->GetRagdollPose(rig);
		REQUIRE(pose);
		REQUIRE(pose->Sleeping);
		CHECK(contacts.CollisionBegin - contacts.CollisionEnd == 1);
		const int begins = contacts.CollisionBegin, ends = contacts.CollisionEnd;
		Simulate(scene, 120);
		CHECK(contacts.CollisionBegin == begins);
		CHECK(contacts.CollisionEnd == ends);
		scene.OnSimulationStop();
		CHECK(contacts.CollisionEnd == ends);
		scene.SetEventListener(nullptr);
	}

	TEST_CASE("Recovery edits and model reload keep their weight and pose instead of snapping to animation")
	{
		ClearAssetCache clear;
		Scene scene;
		Entity rig = Rig(scene, true);
		scene.OnSimulationStart();
		Simulate(scene, 30);
		auto& physics = *scene.GetPhysicsWorld();
		REQUIRE(physics.RecoverRagdoll(rig, {}, 1));
		Simulate(scene, 10);
		const glm::vec3 position = BoneWorld(scene, rig, "RootJoint");
		rig.GetComponent<RagdollComponent>().Mass = 100;
		AssetManager::Clear();
		Simulate(scene, 1);
		const auto pose = physics.GetRagdollPose(rig);
		REQUIRE(pose);
		CHECK(pose->Weight == doctest::Approx(1 - 11 * Step));
		CHECK(glm::length(BoneWorld(scene, rig, "RootJoint") - position) < 0.1f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Ragdoll rejects overflowing impulses and invalid recovery transforms without losing the body")
	{
		ClearAssetCache clear;
		Scene scene;
		Entity rig = Rig(scene);
		rig.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		rig.AddComponent<SphereColliderComponent>();
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		CHECK_FALSE(physics.ActivateRagdoll(rig, glm::vec3(std::numeric_limits<float>::max())));
		CHECK(physics.HasBody(rig));
		REQUIRE(physics.ActivateRagdoll(rig));
		const glm::vec3 original = rig.GetTransform().Translation;
		rig.GetTransform().Scale = glm::vec3(0);
		CHECK_FALSE(physics.RecoverRagdoll(rig, {}, 0.5f));
		CHECK(physics.IsRagdollActive(rig));
		CHECK(rig.GetTransform().Translation == original);
		rig.GetTransform().Scale = glm::vec3(1);
		CHECK_FALSE(physics.ActivateRagdoll(rig, glm::vec3(std::numeric_limits<float>::max())));
		CHECK(physics.IsRagdollActive(rig));
		CHECK(Math::IsFinite(BoneWorldTransform(scene, rig, "RootJoint")));
		scene.OnSimulationStop();
	}

	TEST_CASE("Non-adjacent overlapping ragdoll parts never emit entity self contacts")
	{
		ClearAssetCache clear;
		TempDirectory directory("RagdollSelfContacts");
		auto model = ReadJson(FixturePath("TwoJoint.gltf"));
		model["nodes"][0]["children"] = { 3 };
		model["nodes"].push_back({ { "name", "Grandchild" }, { "translation", { 0, 1, 0 } } });
		model["skins"][0]["joints"].push_back(3);
		model["skins"][0].erase("inverseBindMatrices");
		const float values[] = { 0, -0.8f, 0, 0, -0.8f, 0, 0, -0.8f, 0 };
		model["buffers"].push_back({ { "uri", "data:application/octet-stream;base64," + Base64(values, sizeof(values)) }, { "byteLength", sizeof(values) } });
		model["bufferViews"].push_back({ { "buffer", model["buffers"].size() - 1 }, { "byteLength", sizeof(values) } });
		model["accessors"].push_back({ { "bufferView", model["bufferViews"].size() - 1 }, { "componentType", 5126 }, { "count", 3 }, { "type", "VEC3" } });
		json clip;
		clip["name"] = "Fold";
		clip["samplers"] = json::array({ { { "input", 7 }, { "output", model["accessors"].size() - 1 }, { "interpolation", "STEP" } } });
		clip["channels"] = json::array({ { { "sampler", 0 }, { "target", { { "node", 3 }, { "path", "translation" } } } } });
		model["animations"].push_back(std::move(clip));
		const auto file = directory.GetPath() / "Fold.gltf";
		REQUIRE(FileSystem::WriteTextFile(file, model.dump()));
		Scene scene;
		Contacts contacts;
		scene.SetEventListener(&contacts);
		scene.GetPhysicsSettings().Gravity = glm::vec3(0);
		Entity rig = Rig(scene);
		rig.GetComponent<AnimatorComponent>().Mesh = file.string();
		scene.OnSimulationStart();
		REQUIRE(scene.GetAnimationSystem()->Play(rig, "Fold"));
		scene.GetAnimationSystem()->Pause(rig, true);
		REQUIRE(scene.GetPhysicsWorld()->ActivateRagdoll(rig));
		CHECK(scene.GetPhysicsWorld()->GetRagdollBodyCount(rig) == 3);
		Simulate(scene, 5);
		CHECK_FALSE(contacts.SelfContact);
		CHECK(contacts.CollisionBegin == 0);
		scene.OnSimulationStop();
		scene.SetEventListener(nullptr);
	}

	TEST_CASE("Ragdoll rejects disconnected and matrix skeletons plus oversized generated capsules")
	{
		ClearAssetCache clear;
		TempDirectory directory("RagdollUnsupportedSkeletons");
		for (int mode = 0; mode < 3; mode++)
		{
			INFO(mode);
			auto model = ReadJson(FixturePath("TwoJoint.gltf"));
			if (mode == 0)
			{
				model["nodes"].push_back({ { "name", "OtherRoot" }, { "translation", { 5, 0, 0 } } });
				model["skins"][0]["joints"].push_back(3);
				model["skins"][0].erase("inverseBindMatrices");
			}
			else if (mode == 1)
			{
				model["nodes"][0].erase("translation");
				model["animations"].erase(size_t(0));
				model["nodes"][0]["matrix"] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1 };
			}
			const auto file = directory.GetPath() / (std::to_string(mode) + ".gltf");
			REQUIRE(FileSystem::WriteTextFile(file, model.dump()));
			Scene scene;
			Entity rig = Rig(scene);
			rig.GetComponent<AnimatorComponent>().Mesh = file.string();
			if (mode == 2)
			{
				rig.GetTransform().Scale = glm::vec3(1000);
				rig.GetComponent<RagdollComponent>().Bones = { { .Joint = "Tip", .Radius = 100 } };
			}
			scene.OnSimulationStart();
			CHECK_FALSE(scene.GetPhysicsWorld()->ActivateRagdoll(rig));
			CHECK(scene.GetPhysicsWorld()->GetBodyCount() == 0);
			scene.OnSimulationStop();
		}
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
		CHECK(Eval(scene, "return pcall(function() Scene.FindEntityByName('Ragdoll'):ActivateRagdoll('bad') end)").starts_with("false"));
		scene.CreateEntity("Plain");
		CHECK(Eval(scene, "return pcall(function() Scene.FindEntityByName('Plain'):ActivateRagdoll() end)").starts_with("false"));
		scene.OnRuntimeStop();
		CHECK(rig.GetComponent<RagdollComponent>().Enabled == false);
	}
}
