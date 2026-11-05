#include <doctest/doctest.h>

#include "AnimationTestUtils.h"
#include "TestUtils.h"

#include <Basalt/Animation/AnimatedModel.h>
#include <Basalt/Physics/PhysicsWorld.h>
#include <Basalt/Physics/RagdollRules.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/SceneSerializer.h>
#include <Basalt/Scene/SceneEventListener.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <functional>
#include <limits>
#include <optional>

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
		std::function<void(ContactEventType)> Callback;
		// When set, receives where the last collision ended.
		std::optional<ContactInfo>* Parted = nullptr;
		void OnContact(Scene&, ContactEventType type, Entity first, Entity second, const std::optional<ContactInfo>& contact) override
		{
			SelfContact = SelfContact || first == second;
			if (Parted && type == ContactEventType::CollisionEnd)
				*Parted = contact;
			if (type == ContactEventType::CollisionBegin)
				CollisionBegin++;
			if (type == ContactEventType::CollisionEnd)
				CollisionEnd++;
			if (type == ContactEventType::TriggerEnter)
				Begin++;
			if (type == ContactEventType::TriggerExit)
				End++;
			if (Callback)
				Callback(type);
		}
		void OnAnimationEvent(Scene&, Entity, const std::string&, const std::string&, const std::string&) override {}
		void OnScriptError(Scene&, const std::string&) override {}
	};
	Entity Zone(Scene& scene)
	{
		Entity zone = scene.CreateEntity("Zone");
		zone.GetTransform().Translation = { 0, 4, 0 };
		zone.AddComponent<RigidBodyComponent>().IsTrigger = true;
		zone.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(100);
		return zone;
	}
	void AddVectorClip(json& model, const char* name, uint32_t node, const char* channel, const float (&values)[9])
	{
		model["buffers"].push_back({ { "uri", "data:application/octet-stream;base64," + Base64(values, sizeof(values)) }, { "byteLength", sizeof(values) } });
		model["bufferViews"].push_back({ { "buffer", model["buffers"].size() - 1 }, { "byteLength", sizeof(values) } });
		model["accessors"].push_back({ { "bufferView", model["bufferViews"].size() - 1 }, { "componentType", 5126 }, { "count", 3 }, { "type", "VEC3" } });
		model["animations"].push_back({ { "name", name }, { "samplers", json::array({ { { "input", 7 }, { "output", model["accessors"].size() - 1 }, { "interpolation", "STEP" } } }) }, { "channels", json::array({ { { "sampler", 0 }, { "target", { { "node", node }, { "path", channel } } } } }) } });
	}
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
		CHECK(std::ranges::find(info->LiveFields, "Enabled") == info->LiveFields.end());
		std::string error;
		const json valid = { { "Enabled", true }, { "Bones", { { { "Joint", "Tip" }, { "Radius", 0.1 }, { "Length", 0.4 }, { "SwingAngle", 20 }, { "TwistMin", -10 } } } } };
		REQUIRE(info->Deserialize(entity, valid, error));
		const auto before = info->Serialize(entity);
		for (const json& invalid : { json{ { "Mass", 0 } }, json{ { "RadiusRatio", 1 } }, json{ { "RecoveryTime", -1 } }, json{ { "Layer", "" } }, json{ { "Bones", { { { "Joint", "Tip" }, { "Radius", -1 } } } } }, json{ { "Bones", { { { "Joint", "Tip" }, { "SwingAngle", 181 } } } } }, json{ { "Bones", { { { "Joint", "Tip" }, { "TwistMin", 1 } } } } }, json{ { "Bones", { { { "Joint", "Tip" } }, { { "Joint", "Tip" } } } } }, json{ { "MotorStrength", 1.5 } }, json{ { "MotorStrength", -0.1 } }, json{ { "MotorFrequency", 0 } }, json{ { "MotorFrequency", 31 } }, json{ { "MotorDamping", -1 } }, json{ { "MotorDamping", 11 } }, json{ { "MotorTorque", -1 } }, json{ { "MotorTorque", 2000000 } }, json{ { "BalanceStrength", 1.5 } }, json{ { "BalanceStrength", -0.1 } }, json{ { "BalanceFrequency", 0 } }, json{ { "BalanceFrequency", 31 } }, json{ { "BalanceDamping", -1 } }, json{ { "BalanceDamping", 11 } }, json{ { "BalanceMaxTilt", -1 } }, json{ { "BalanceMaxTilt", 181 } }, json{ { "Bones", { { { "Joint", "Tip" }, { "MotorStrength", 1.5 } } } } }, json{ { "Bones", { { { "Joint", "Tip" }, { "MotorStrength", -1 } } } } } })
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
		component.MotorTorque = std::numeric_limits<float>::infinity();
		CHECK_FALSE(ValidateRagdoll(component).empty());
		component.MotorTorque = 500;
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
		CHECK(BoneWorld(scene, rig, "RootJoint").y == doctest::Approx((fallen.y + 4) / 2).epsilon(0.002));
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
		CHECK(physics.GetBodyCount() == 0);
		CHECK(physics.OverlapSphere({ 0, 4, 0 }, 10).empty());
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
		Contacts contacts;
		Scene scene;
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
			Simulate(scene, 1);
			if (action < 3)
			{
				CHECK(physics.HasBody(rig));
				CHECK(physics.GetBodyCount() == 2);
			}
			else
				CHECK(physics.GetBodyCount() == 1);
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
		scene.GetPhysicsSettings().FixedTimestep = Step;
		Entity rig = Rig(scene);
		rig.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		rig.AddComponent<SphereColliderComponent>();
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		physics.SetLinearVelocity(rig, { 3, 0, 0 });
		physics.SetAngularVelocity(rig, { 0, 0, 1 });
		REQUIRE(physics.ActivateRagdoll(rig));
		Simulate(scene, 1);
		const glm::mat4 first = BoneWorldTransform(scene, rig, "RootJoint");
		CHECK(first[3].x == doctest::Approx(3 * Step).epsilon(0.005));
		CHECK(std::atan2(first[0].y, first[0].x) / Step == doctest::Approx(1).epsilon(0.005));
		Simulate(scene, 9);
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
			const float angularSpeed = (std::atan2(after[0].y, after[0].x) - std::atan2(old[0].y, old[0].x)) / Step;
			CHECK(angularSpeed > 0.9f);
			CHECK(angularSpeed < 1.1f);
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
		Contacts contacts;
		Scene scene;
		scene.SetEventListener(&contacts);
		Floor(scene);
		Entity rig = Rig(scene, true);
		scene.OnSimulationStart();
		Simulate(scene, 240);
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
		Contacts contacts;
		Scene scene;
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
		CHECK(Eval(scene, "return Scene.FindEntityByName('Ragdoll'):IsRagdollBalanced()") == "false");
		CHECK(Eval(scene, "return Scene.FindEntityByName('Ragdoll'):RecoverRagdoll('Bend', 0.1)") == "true");
		CHECK(Eval(scene, "return Scene.FindEntityByName('Ragdoll'):IsRagdollActive()") == "false");
		const auto* info = ComponentRegistry::Find("Ragdoll");
		REQUIRE(info);
		CHECK(info->Serialize(rig).size() == 16);
		CHECK(Eval(scene, "return pcall(function() Scene.FindEntityByName('Ragdoll'):ActivateRagdoll('bad') end)").starts_with("false"));
		scene.CreateEntity("Plain");
		CHECK(Eval(scene, "return pcall(function() Scene.FindEntityByName('Plain'):ActivateRagdoll() end)").starts_with("false"));
		scene.OnRuntimeStop();
		CHECK(rig.GetComponent<RagdollComponent>().Enabled == false);
	}
	TEST_CASE("Component removal finishes before Lua exit callbacks remove that same component again")
	{
		ClearAssetCache clear;
		TempDirectory directory("RagdollRemoveSignal");
		const auto file = directory.GetPath() / "Exit.lua";
		REQUIRE(FileSystem::WriteTextFile(file, R"(
			return { OnTriggerExit = function(self, other)
				Exits = (Exits or 0) + 1
				if self.Entity:HasComponent(RemoveName) then self.Entity:RemoveComponent(RemoveName) end
			end }
		)"));
		for (const char* name : { "Ragdoll", "Animator" })
		{
			INFO(name);
			Scene scene;
			scene.GetPhysicsSettings().Gravity = glm::vec3(0);
			Entity rig = Rig(scene, true);
			rig.AddComponent<ScriptComponent>().Script = file.string();
			Zone(scene);
			scene.OnRuntimeStart();
			Eval(scene, std::string("RemoveName = '") + name + "'");
			Simulate(scene, 1);
			CHECK(Eval(scene, "return Exits or 0") == "0");
			Eval(scene, "Scene.FindEntityByName('Ragdoll'):RemoveComponent(RemoveName)");
			CHECK(scene.GetPhysicsWorld()->GetBodyCount() == 1); // Only the zone remains, before any update.
			CHECK(Eval(scene, "return Exits or 0") == "0");
			Simulate(scene, 1);
			CHECK(Eval(scene, "return Exits") == "1");
			CHECK(Eval(scene, "return Scene.FindEntityByName('Ragdoll'):HasComponent(RemoveName)") == "false");
			scene.OnRuntimeStop();
		}
	}

	TEST_CASE("Sleeping kinematic trigger contacts persist until the body moves out")
	{
		Contacts contacts;
		Scene scene;
		scene.GetPhysicsSettings().Gravity = glm::vec3(0);
		scene.SetEventListener(&contacts);
		Entity zone = Zone(scene);
		zone.GetComponent<BoxColliderComponent>().HalfExtents = glm::vec3(2);
		Entity body = scene.CreateEntity("Kinematic");
		body.GetTransform().Translation = { 0, 4, 0 };
		body.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		body.AddComponent<BoxColliderComponent>();
		scene.OnSimulationStart();
		Simulate(scene, 180);
		REQUIRE(scene.GetPhysicsWorld()->IsBodySleeping(body));
		CHECK(contacts.Begin == 1);
		CHECK(contacts.End == 0);
		body.GetTransform().Translation.x = 10;
		Simulate(scene, 2);
		CHECK(contacts.End == 1);
		scene.OnSimulationStop();
	}

	TEST_CASE("Rest scale failures cache while transient world scale failures retry")
	{
		ClearAssetCache clear;
		TempDirectory directory("RagdollRestScale");
		auto model = ReadJson(FixturePath("TwoJoint.gltf"));
		model["nodes"][0]["scale"] = { 1, 1, 1.001 };
		const auto file = directory.GetPath() / "Scaled.gltf";
		REQUIRE(FileSystem::WriteTextFile(file, model.dump()));
		Scene scene;
		Entity rig = Rig(scene, true);
		rig.GetComponent<AnimatorComponent>().Mesh = file.string();
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		const uint64_t builds = physics.GetRagdollBuildCount();
		Simulate(scene, 5);
		CHECK(physics.GetRagdollBuildCount() == builds);
		CHECK_FALSE(physics.IsRagdollActive(rig));
		rig.GetComponent<AnimatorComponent>().Mesh = FixturePath("TwoJoint.gltf").string();
		rig.GetTransform().Scale = { 1, 2, 1 };
		Simulate(scene, 1);
		CHECK_FALSE(physics.IsRagdollActive(rig));
		rig.GetTransform().Scale = glm::vec3(1);
		Simulate(scene, 1);
		CHECK(physics.IsRagdollActive(rig));
		scene.OnSimulationStop();
	}

	TEST_CASE("Disabling together with structural settings or a reload still blends recovery")
	{
		ClearAssetCache clear;
		for (bool reload : { false, true })
		{
			INFO(reload);
			Scene scene;
			Entity rig = Rig(scene, true);
			scene.OnSimulationStart();
			Simulate(scene, 30);
			auto& physics = *scene.GetPhysicsWorld();
			const glm::vec3 fallen = BoneWorld(scene, rig, "RootJoint");
			rig.GetComponent<RagdollComponent>().Enabled = false;
			rig.GetComponent<RagdollComponent>().Mass = 100;
			if (reload)
				AssetManager::Clear();
			Simulate(scene, 1);
			REQUIRE(physics.GetRagdollPose(rig));
			CHECK_FALSE(physics.IsRagdollActive(rig));
			CHECK(physics.GetRagdollPose(rig)->Weight == doctest::Approx(1));
			CHECK(Near(BoneWorld(scene, rig, "RootJoint"), fallen, 0.001f));
			scene.OnSimulationStop();
		}
	}

	TEST_CASE("Unsupported live world scale disables active and recovering rigs with a diagnostic")
	{
		ClearAssetCache clear;
		for (bool recover : { false, true })
		{
			Scene scene;
			Entity rig = Rig(scene, true);
			scene.OnSimulationStart();
			auto& physics = *scene.GetPhysicsWorld();
			if (recover)
				REQUIRE(physics.RecoverRagdoll(rig, {}, 1));
			rig.GetTransform().Scale = recover ? glm::vec3(0) : glm::vec3(1, 2, 1);
			const size_t errors = Log::GetHistory().GetErrorCount();
			Simulate(scene, 1);
			CHECK_FALSE(physics.HasRagdollPose(rig));
			CHECK(physics.GetBodyCount() == 0);
			CHECK_FALSE(rig.GetComponent<RagdollComponent>().Enabled);
			CHECK(Log::GetHistory().GetErrorCount() == errors + 1);
			Simulate(scene, 1);
			CHECK(Log::GetHistory().GetErrorCount() == errors + 1);
			scene.OnSimulationStop();
		}
	}

	TEST_CASE("Recovery callbacks preserve replacement recoveries and sample relocated poses")
	{
		ClearAssetCache clear;
		for (bool immediate : { false, true })
		{
			INFO(immediate);
			Contacts contacts;
			Scene scene;
			scene.GetPhysicsSettings().Gravity = glm::vec3(0);
			scene.SetEventListener(&contacts);
			Entity rig = Rig(scene, true);
			Zone(scene);
			scene.OnSimulationStart();
			auto& physics = *scene.GetPhysicsWorld();
			REQUIRE(physics.ActivateRagdoll(rig, { 70, 0, 0 }));
			Simulate(scene, 30);
			const glm::vec3 fallen = BoneWorld(scene, rig, "RootJoint");
			bool replaced = false;
			contacts.Callback = [&](ContactEventType type) {
				if (type != ContactEventType::TriggerExit || replaced)
					return;
				replaced = true;
				CHECK(Near(BoneWorld(scene, rig, "RootJoint"), fallen, 0.001f));
				REQUIRE(physics.ActivateRagdoll(rig));
				REQUIRE(physics.RecoverRagdoll(rig, {}, 1));
			};
			REQUIRE(physics.RecoverRagdoll(rig, {}, immediate ? 0 : 0.5f));
			REQUIRE(replaced);
			REQUIRE(physics.GetRagdollPose(rig));
			CHECK(physics.GetRagdollPose(rig)->Weight == doctest::Approx(1));
			CHECK(Near(BoneWorld(scene, rig, "RootJoint"), fallen, 0.001f));
			scene.OnSimulationStop();
		}
	}

	TEST_CASE("Unrepresentable physical overlays disable bodies safely even when exit callbacks destroy playbacks")
	{
		ClearAssetCache clear;
		TempDirectory directory("RagdollRejectOverlay");
		auto model = ReadJson(FixturePath("TwoJoint.gltf"));
		AddVectorClip(model, "Scale", 2, "scale", { 1, 1, 1, 2, 1, 1, 2, 1, 1 });
		const auto file = directory.GetPath() / "Scale.gltf";
		REQUIRE(FileSystem::WriteTextFile(file, model.dump()));
		for (int action = 0; action < 3; action++)
		{
			INFO(action);
			Contacts contacts;
			Scene scene;
			scene.GetPhysicsSettings().Gravity = glm::vec3(0);
			scene.SetEventListener(&contacts);
			Entity rig = Rig(scene);
			rig.GetComponent<AnimatorComponent>().Mesh = file.string();
			rig.GetComponent<RagdollComponent>().RootJoint = "Tip";
			rig.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
			rig.AddComponent<SphereColliderComponent>();
			Zone(scene);
			scene.OnRuntimeStart();
			auto& animation = *scene.GetAnimationSystem();
			auto& physics = *scene.GetPhysicsWorld();
			REQUIRE(animation.Play(rig, "Scale"));
			animation.Pause(rig, true);
			physics.SetAngularVelocity(rig, { 0, 0, 1 });
			REQUIRE(physics.ActivateRagdoll(rig));
			Simulate(scene, 30);
			REQUIRE(contacts.Begin > 0);
			contacts.Callback = [&](ContactEventType type) {
				if (type != ContactEventType::TriggerExit)
					return;
				if (action == 1)
					rig.RemoveComponent<AnimatorComponent>();
				else if (action == 2)
					scene.DestroyEntity(rig);
			};
			REQUIRE(animation.Seek(rig, 1));
			// External control dispatches immediately (no Scene update's deferred entity destruction guard).
			animation.RefreshRagdollPoses();
			CHECK_FALSE(physics.IsRagdollActive(rig));
			CHECK(physics.GetBodyCount() == 1);
			if (action == 0)
				CHECK_FALSE(rig.GetComponent<RagdollComponent>().Enabled);
			if (action == 2)
				CHECK_FALSE(rig);
			scene.OnRuntimeStop();
		}
	}

	TEST_CASE("Rejected get-up placement preserves the previous animation playback")
	{
		ClearAssetCache clear;
		TempDirectory directory("RagdollGetUpPreflight");
		auto model = ReadJson(FixturePath("TwoJoint.gltf"));
		AddVectorClip(model, "Huge", 2, "translation", { 3e38f, 0, 0, 3e38f, 0, 0, 3e38f, 0, 0 });
		const auto file = directory.GetPath() / "Huge.gltf";
		REQUIRE(FileSystem::WriteTextFile(file, model.dump()));
		Scene scene;
		scene.GetPhysicsSettings().Gravity = glm::vec3(0);
		Entity rig = Rig(scene);
		rig.GetComponent<AnimatorComponent>().Mesh = file.string();
		rig.GetTransform().Scale = glm::vec3(2);
		scene.OnSimulationStart();
		auto& animation = *scene.GetAnimationSystem();
		auto& physics = *scene.GetPhysicsWorld();
		REQUIRE(animation.Play(rig, "Bend"));
		REQUIRE(animation.Seek(rig, 0.5f));
		animation.Pause(rig, true);
		std::vector<glm::mat4> before, after;
		const auto source = animation.GetPoseView(rig)->Source;
		REQUIRE(animation.GetAnimatedPose(rig, *source, before));
		REQUIRE(physics.ActivateRagdoll(rig));
		CHECK_FALSE(physics.RecoverRagdoll(rig, "Huge", 0.5f));
		CHECK(physics.IsRagdollActive(rig));
		REQUIRE(animation.GetAnimatedPose(rig, *source, after));
		CHECK(NearPose(before, after));
		scene.OnSimulationStop();
	}
	TEST_CASE("Replacing the skeleton remaps matching joints and seeds newly named roots and limbs")
	{
		ClearAssetCache clear;
		TempDirectory directory("RagdollNewSkeleton");
		auto model = ReadJson(FixturePath("TwoJoint.gltf"));
		model["nodes"][2]["name"] = "NewRoot";
		model["nodes"][0]["children"] = { 3 };
		model["nodes"].push_back({ { "name", "NewLimb" }, { "translation", { 0, 0.5, 0 } } });
		model["skins"][0]["joints"].push_back(3);
		model["skins"][0].erase("inverseBindMatrices");
		const auto file = directory.GetPath() / "NewSkeleton.gltf";
		REQUIRE(FileSystem::WriteTextFile(file, model.dump()));
		for (bool recovery : { false, true })
		{
			Scene scene;
			scene.GetPhysicsSettings().Gravity = glm::vec3(0);
			Entity rig = Rig(scene, true);
			scene.OnSimulationStart();
			auto& physics = *scene.GetPhysicsWorld();
			REQUIRE(physics.ActivateRagdoll(rig, { 70, 0, 0 }));
			Simulate(scene, 30);
			if (recovery)
				REQUIRE(physics.RecoverRagdoll(rig, {}, 1));
			const glm::vec3 root = BoneWorld(scene, rig, "RootJoint");
			const glm::vec3 tip = BoneWorld(scene, rig, "Tip");
			rig.GetComponent<AnimatorComponent>().Mesh = file.string();
			Simulate(scene, 1);
			REQUIRE(physics.GetRagdollPose(rig));
			CHECK(physics.GetRagdollPose(rig)->Joints.size() == 3);
			CHECK(Near(BoneWorld(scene, rig, "NewRoot"), root, 0.025f));
			CHECK(Near(BoneWorld(scene, rig, "Tip"), tip, 0.025f));
			CHECK(glm::length(BoneWorld(scene, rig, "NewLimb") - BoneWorld(scene, rig, "Tip")) == doctest::Approx(0.5f).epsilon(0.01));
			if (recovery)
				CHECK(physics.GetRagdollPose(rig)->Weight == doctest::Approx(1 - Step));
			else
			{
				CHECK(physics.GetBodyCount() == 3);
				CHECK(BoneWorld(scene, rig, "NewRoot").x > root.x + 0.01f);
			}
			scene.OnSimulationStop();
		}
	}

	TEST_CASE("Activation from OnUpdate respects deferred destruction requested by contact exit")
	{
		ClearAssetCache clear;
		TempDirectory directory("RagdollDeferredDestroy");
		const auto file = directory.GetPath() / "Destroy.lua";
		REQUIRE(FileSystem::WriteTextFile(file, R"(
			return {
				OnUpdate = function(self)
					if ActivateNow then ActivateNow = false; ActivationResult = self.Entity:ActivateRagdoll() end
				end,
				OnTriggerExit = function(self) self.Entity:Destroy() end
			}
		)"));
		Scene scene;
		scene.GetPhysicsSettings().Gravity = glm::vec3(0);
		Entity rig = Rig(scene);
		rig.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		rig.AddComponent<SphereColliderComponent>();
		rig.AddComponent<ScriptComponent>().Script = file.string();
		Zone(scene);
		scene.OnRuntimeStart();
		Simulate(scene, 1);
		Eval(scene, "ActivateNow = true");
		Simulate(scene, 1);
		CHECK(Eval(scene, "return ActivationResult") == "false");
		CHECK_FALSE(rig);
		CHECK(scene.GetPhysicsWorld()->GetBodyCount() == 1);
		scene.OnRuntimeStop();
	}
}

namespace {
	// The tip's rotation relative to the root body in the physical pose, in degrees.
	float PhysicalBend(PhysicsWorld& physics, Entity rig)
	{
		const auto pose = physics.GetRagdollPose(rig);
		REQUIRE(pose);
		REQUIRE(pose->Joints.size() == 2);
		const glm::quat root = glm::quat_cast(glm::mat3(pose->Joints[0].second));
		const glm::quat tip = glm::quat_cast(glm::mat3(pose->Joints[1].second));
		return glm::degrees(glm::angle(glm::normalize(glm::conjugate(root) * tip)));
	}
	// A rig floating without gravity, activated on its rest pose, whose animation then bends the tip 30 degrees.
	struct PoweredRig
	{
		// `tipStrength`, when set, is the Tip joint's own MotorStrength.
		explicit PoweredRig(float strength, std::optional<float> tipStrength = std::nullopt)
		{
			Floor(scene);
			rig = Rig(scene);
			auto& ragdoll = rig.GetComponent<RagdollComponent>();
			ragdoll.MotorStrength = strength;
			if (tipStrength)
				ragdoll.Bones.push_back({ .Joint = "Tip", .MotorStrength = *tipStrength });
			scene.OnSimulationStart();
			physics = scene.GetPhysicsWorld();
			physics->SetGravity(glm::vec3(0));
			scene.GetAnimationSystem()->Update(0);
			REQUIRE(physics->ActivateRagdoll(rig));
			REQUIRE(scene.GetAnimationSystem()->Seek(rig, 1.0f / 3.0f));
		}
		~PoweredRig() { scene.OnSimulationStop(); }
		Scene scene;
		Entity rig;
		PhysicsWorld* physics = nullptr;
	};
	// The Biped with a rig from the chest up on a kinematic root; a character holds it unless `character` is false.
	Entity PartialBiped(Scene& scene, bool character = true)
	{
		Entity rig = scene.CreateEntity("Biped");
		auto& animator = rig.AddComponent<AnimatorComponent>();
		animator.Mesh = BipedPath().string();
		animator.PlayOnStart = false;
		auto& ragdoll = rig.AddComponent<RagdollComponent>();
		ragdoll.RootJoint = "Chest";
		ragdoll.KinematicRoot = true;
		if (character)
		{
			rig.AddComponent<CharacterControllerComponent>();
			auto& capsule = rig.AddComponent<CapsuleColliderComponent>();
			capsule.Offset = { 0, 0.9f, 0 };
			capsule.Radius = 0.5f;
			capsule.HalfHeight = 0.5f;
		}
		return rig;
	}
	// The Biped ragdolled whole on a floor with running motors, at `height`, standing as `rotation` turns it.
	struct BalancingBiped
	{
		explicit BalancingBiped(float balance, float height = 0, const glm::quat& rotation = glm::quat(1, 0, 0, 0), const std::optional<glm::vec3>& gravity = std::nullopt)
		{
			Entity floor = Floor(scene);
			if (gravity)
			{
				// The floor and the rig on its far side, as if the world had turned.
				scene.GetPhysicsSettings().Gravity = *gravity;
				floor.GetTransform().Translation = { -0.5f, 0, 0 };
				floor.GetComponent<BoxColliderComponent>().HalfExtents = { 0.5f, 20, 20 };
			}
			rig = scene.CreateEntity("Biped");
			rig.GetTransform().Translation.y = height;
			rig.GetTransform().Rotation = rotation;
			auto& animator = rig.AddComponent<AnimatorComponent>();
			animator.Mesh = BipedPath().string();
			animator.PlayOnStart = false;
			auto& ragdoll = rig.AddComponent<RagdollComponent>();
			ragdoll.MotorStrength = 1;
			ragdoll.BalanceStrength = balance;
			scene.OnSimulationStart();
			physics = scene.GetPhysicsWorld();
			scene.GetAnimationSystem()->Update(0);
			REQUIRE(physics->ActivateRagdoll(rig));
		}
		~BalancingBiped() { scene.OnSimulationStop(); }
		void Run(int frames) { Simulate(scene, frames); }
		glm::vec3 Bone(const char* joint) { return BoneWorld(scene, rig, joint); }
		glm::vec3 Hips() { return Bone("Hips"); }
		// 1 with the spine straight up.
		float Upright() { return glm::normalize(Bone("Chest") - Hips()).y; }
		Scene scene;
		Entity rig;
		PhysicsWorld* physics = nullptr;
	};
}

TEST_SUITE("Ragdolls")
{
	TEST_CASE("Powered ragdolls turn their joints toward the animated pose")
	{
		ClearAssetCache clear;
		PoweredRig powered(1.0f);
		PoweredRig passive(0.0f);
		const uint64_t builds = powered.physics->GetRagdollBuildCount();
		Simulate(powered.scene, 90);
		Simulate(passive.scene, 90);
		// The motors bend the tip to the animated 30 degrees; a passive rig keeps the pose it was activated on.
		CHECK(PhysicalBend(*powered.physics, powered.rig) == doctest::Approx(30.0f).epsilon(0.05));
		CHECK(PhysicalBend(*passive.physics, passive.rig) < 1.0f);
		CHECK(powered.physics->IsRagdollActive(powered.rig));
		CHECK(powered.physics->GetRagdollPose(powered.rig)->Powered);
		CHECK_FALSE(passive.physics->GetRagdollPose(passive.rig)->Powered);

		// Motor settings apply live, without rebuilding the rig.
		auto& component = powered.rig.GetComponent<RagdollComponent>();
		component.MotorFrequency = 2.0f;
		component.MotorDamping = 0.5f;
		component.MotorTorque = 50.0f;
		Simulate(powered.scene, 5);
		CHECK(powered.physics->GetRagdollBuildCount() == builds);
		CHECK(powered.physics->IsRagdollActive(powered.rig));

		// At strength 0 the motors let go: the bend no longer follows the animation back to straight.
		REQUIRE(powered.physics->SetRagdollMotorStrength(powered.rig, 0, 0));
		REQUIRE(powered.scene.GetAnimationSystem()->Seek(powered.rig, 0));
		Simulate(powered.scene, 60);
		CHECK(PhysicalBend(*powered.physics, powered.rig) > 20.0f);
		CHECK_FALSE(powered.physics->GetRagdollPose(powered.rig)->Powered);
		// A weak motor (tiny torque) barely turns it back.
		REQUIRE(powered.physics->SetRagdollMotorStrength(powered.rig, 1, 0));
		component.MotorTorque = 0.0001f;
		Simulate(powered.scene, 60);
		CHECK(PhysicalBend(*powered.physics, powered.rig) > 20.0f);
		component.MotorTorque = 500.0f;
		component.MotorFrequency = 8.0f;
		component.MotorDamping = 1.0f;
		Simulate(powered.scene, 90);
		CHECK(PhysicalBend(*powered.physics, powered.rig) < 2.0f);
		CHECK(powered.physics->GetRagdollBuildCount() == builds);
	}

	TEST_CASE("Ragdoll motor strength fades, survives recovery and stays passive at zero")
	{
		ClearAssetCache clear;
		// Motor settings without strength change nothing: a passive rig falls exactly alike with any of them.
		const auto fall = [](float frequency, float torque) {
			Scene scene;
			Floor(scene);
			Entity rig = Rig(scene);
			rig.GetComponent<RagdollComponent>().MotorFrequency = frequency;
			rig.GetComponent<RagdollComponent>().MotorTorque = torque;
			scene.OnSimulationStart();
			scene.GetAnimationSystem()->Update(0);
			REQUIRE(scene.GetPhysicsWorld()->ActivateRagdoll(rig, glm::vec3(3, 0, 1)));
			Simulate(scene, 120);
			const auto pose = scene.GetPhysicsWorld()->GetRagdollPose(rig);
			REQUIRE(pose);
			scene.OnSimulationStop();
			return pose->Joints;
		};
		CHECK(fall(8, 500) == fall(20, 900));

		Scene scene;
		Floor(scene);
		Entity rig = Rig(scene);
		auto& component = rig.GetComponent<RagdollComponent>();
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		scene.GetAnimationSystem()->Update(0);
		REQUIRE(physics.ActivateRagdoll(rig));
		// Scripts fade the strength; a changed MotorStrength applies at once and ends the fade.
		CHECK(physics.GetRagdollMotorStrength(rig) == 0.0f);
		REQUIRE(physics.SetRagdollMotorStrength(rig, 1, 0.5f));
		Simulate(scene, 15);
		CHECK(physics.GetRagdollMotorStrength(rig).value() == doctest::Approx(0.5f).epsilon(0.02));
		component.MotorStrength = 0.25f;
		Simulate(scene, 15);
		CHECK(physics.GetRagdollMotorStrength(rig) == 0.25f);
		// Gameplay state: it survives recovery and a new activation.
		REQUIRE(physics.SetRagdollMotorStrength(rig, 0.75f, 0));
		REQUIRE(physics.RecoverRagdoll(rig, {}, 0));
		Simulate(scene, 2);
		REQUIRE(physics.ActivateRagdoll(rig));
		CHECK(physics.GetRagdollMotorStrength(rig) == 0.75f);

		// Invalid values fail and are logged once; an entity without a Ragdoll has no strength.
		const auto since = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(physics.SetRagdollMotorStrength(rig, 2, 0));
		CHECK_FALSE(physics.SetRagdollMotorStrength(rig, 0.5f, -1));
		CHECK(CountLogMessages(since, "the strength must be between 0 and 1") == 1);
		CHECK(physics.GetRagdollMotorStrength(rig) == 0.75f);
		Entity plain = scene.CreateEntity("Plain");
		CHECK_FALSE(physics.GetRagdollMotorStrength(plain).has_value());
		CHECK_FALSE(physics.SetRagdollMotorStrength(plain, 1, 0));
		// Settings C++ wrote past validation are clamped and said so once (a NaN strength is 0: motors off).
		component.MotorFrequency = std::numeric_limits<float>::quiet_NaN();
		Simulate(scene, 3);
		CHECK(CountLogMessages(since, "MotorFrequency must be in 0.1..30; clamping") == 1);
		component.MotorFrequency = 8;
		component.MotorStrength = std::numeric_limits<float>::quiet_NaN();
		Simulate(scene, 3);
		CHECK(CountLogMessages(since, "MotorStrength must be finite; motors are off") == 1);
		CHECK(physics.GetRagdollMotorStrength(rig) == 0.0f);
		CHECK_FALSE(physics.GetRagdollPose(rig)->Powered);
		component.MotorStrength = 1.5f;
		Simulate(scene, 3);
		CHECK(CountLogMessages(since, "MotorStrength must be in 0..1; clamping") == 1);
		CHECK(physics.GetRagdollMotorStrength(rig) == 1.0f);
		component.MotorStrength = 0.75f;
		// The strength ends with the component.
		rig.RemoveComponent<RagdollComponent>();
		CHECK_FALSE(physics.GetRagdollMotorStrength(rig).has_value());
		scene.OnSimulationStop();
	}

	TEST_CASE("Lua fades and reads ragdoll motor strength")
	{
		ClearAssetCache clear;
		Scene scene;
		Entity rig = Rig(scene);
		rig.GetComponent<RagdollComponent>().MotorStrength = 0.5f;
		scene.CreateEntity("Plain");
		scene.OnRuntimeStart();
		CHECK(Eval(scene, "return Scene.FindEntityByName('Ragdoll'):GetRagdollMotorStrength()") == "0.5");
		CHECK(Eval(scene, "return Scene.FindEntityByName('Ragdoll'):SetRagdollMotorStrength(1, 1)") == "true");
		CHECK(Eval(scene, "return Scene.FindEntityByName('Ragdoll'):SetRagdollMotorStrength(-1)") == "false");
		Simulate(scene, 30);
		CHECK(std::stof(Eval(scene, "return Scene.FindEntityByName('Ragdoll'):GetRagdollMotorStrength()")) == doctest::Approx(0.75f).epsilon(0.03));
		CHECK(Eval(scene, "return pcall(function() Scene.FindEntityByName('Plain'):SetRagdollMotorStrength(1) end)").starts_with("false"));
		CHECK(Eval(scene, "return pcall(function() Scene.FindEntityByName('Plain'):GetRagdollMotorStrength() end)").starts_with("false"));
		CHECK(Eval(scene, "return pcall(function() Scene.FindEntityByName('Ragdoll'):SetRagdollMotorStrength(1, 'soon') end)").starts_with("false"));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Only structural Ragdoll settings rebuild a rig")
	{
		RagdollComponent built;
		built.Enabled = true;
		auto live = built;
		live.Enabled = false;
		live.RecoveryTime = 2;
		live.MotorStrength = 1;
		live.MotorFrequency = 3;
		live.MotorDamping = 0.2f;
		live.MotorTorque = 10;
		live.BalanceStrength = 1;
		live.BalanceFrequency = 2;
		live.BalanceDamping = 0.5f;
		live.BalanceMaxTilt = 20;
		CHECK_FALSE(RagdollNeedsRebuild(built, live));
		built.Bones.push_back({ .Joint = "Tip" });
		live.Bones.push_back({ .Joint = "Tip", .MotorStrength = 0.25f });
		CHECK_FALSE(RagdollNeedsRebuild(built, live));
		for (const auto& edit : std::vector<std::function<void(RagdollComponent&)>>{ [](RagdollComponent& c) { c.KinematicRoot = true; }, [](RagdollComponent& c) { c.Bones[0].Mass = 3; }, [](RagdollComponent& c) { c.Mass = 40; }, [](RagdollComponent& c) { c.RadiusRatio = 0.2f; }, [](RagdollComponent& c) { c.RootJoint = "Tip"; }, [](RagdollComponent& c) { c.Layer = "Debris"; }, [](RagdollComponent& c) { c.Bones.push_back({}); } })
		{
			auto structural = live;
			edit(structural);
			CHECK(RagdollNeedsRebuild(built, structural));
		}
	}

	TEST_CASE("Each bone scales the rig's motors, live and without a rebuild")
	{
		ClearAssetCache clear;
		PoweredRig rig(1.0f, 0.0f);
		const uint64_t builds = rig.physics->GetRagdollBuildCount();
		// A limp joint does not follow the animation, and a rig with no running joint is not powered.
		Simulate(rig.scene, 90);
		CHECK(PhysicalBend(*rig.physics, rig.rig) < 1.0f);
		CHECK_FALSE(rig.physics->GetRagdollPose(rig.rig)->Powered);
		// Its strength applies at once.
		auto& bone = rig.rig.GetComponent<RagdollComponent>().Bones[0];
		bone.MotorStrength = 1;
		Simulate(rig.scene, 90);
		CHECK(PhysicalBend(*rig.physics, rig.rig) == doctest::Approx(30.0f).epsilon(0.05));
		CHECK(rig.physics->GetRagdollPose(rig.rig)->Powered);
		// A weak joint in a strong rig cannot turn back; the rig's strength still scales it.
		REQUIRE(rig.scene.GetAnimationSystem()->Seek(rig.rig, 0));
		bone.MotorStrength = 0.0000001f;
		Simulate(rig.scene, 60);
		CHECK(PhysicalBend(*rig.physics, rig.rig) > 20.0f);
		bone.MotorStrength = 1;
		Simulate(rig.scene, 90);
		CHECK(PhysicalBend(*rig.physics, rig.rig) < 2.0f);
		REQUIRE(rig.scene.GetAnimationSystem()->Seek(rig.rig, 1.0f / 3.0f));
		bone.MotorStrength = 0;
		Simulate(rig.scene, 5);
		CHECK_FALSE(rig.physics->GetRagdollPose(rig.rig)->Powered);
		CHECK(rig.physics->GetRagdollBuildCount() == builds);

		// A value C++ wrote past validation is clamped and said so once; a NaN is the default, 1.
		const auto since = Log::GetHistory().GetTotalCount();
		bone.MotorStrength = std::numeric_limits<float>::quiet_NaN();
		Simulate(rig.scene, 90);
		CHECK(CountLogMessages(since, "Bones[0] 'Tip' MotorStrength must be in 0..1; clamping") == 1);
		CHECK(PhysicalBend(*rig.physics, rig.rig) == doctest::Approx(30.0f).epsilon(0.05));
	}

	TEST_CASE("A kinematic root walks with its character while the rig below it hangs")
	{
		ClearAssetCache clear;
		Scene scene;
		Floor(scene);
		Entity rig = PartialBiped(scene);
		// Motors hold the arms out, softly enough to lag behind the walk.
		auto& ragdoll = rig.GetComponent<RagdollComponent>();
		ragdoll.MotorStrength = 1;
		ragdoll.MotorFrequency = 1;
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		scene.GetAnimationSystem()->Update(0);
		Simulate(scene, 30);
		const glm::vec3 standing = scene.GetWorldTransform(rig)[3];
		REQUIRE(physics.ActivateRagdoll(rig));
		// The rig joins the character instead of replacing it.
		CHECK(physics.HasCharacter(rig));
		CHECK(physics.GetRagdollBodyCount(rig) == 8);
		CHECK(physics.IsRagdollActive(rig));
		CHECK(physics.HasRagdollPose(rig));
		CHECK_FALSE(physics.RagdollOwnsRoot(rig));
		const auto source = AssetManager::GetMesh(rig.GetComponent<AnimatorComponent>().Mesh);
		const auto animatedChest = [&] {
			std::vector<glm::mat4> animated;
			REQUIRE(scene.GetAnimationSystem()->GetAnimatedPose(rig, *source, animated));
			return glm::vec3(scene.GetWorldTransform(rig) * animated[Node(*source, "Chest")][3]);
		};
		// The root body itself is on the animation, not only what is drawn; the rig's hanging moves nothing of the character.
		const auto rootBody = [&] { return glm::vec3(scene.GetWorldTransform(rig) * physics.GetRagdollPose(rig)->Joints[0].second[3]); };
		Simulate(scene, 60);
		CHECK(Near(glm::vec3(scene.GetWorldTransform(rig)[3]), standing, 1e-3f));
		CHECK(Near(rootBody(), animatedChest(), 1e-3f));
		// Walking: the character covers what it would alone, the root stays exactly on the animation, and the arms hang
		// behind it rather than being carried rigidly.
		const float held = BoneWorld(scene, rig, "LeftHand").x - BoneWorld(scene, rig, "LeftUpperArm").x;
		physics.MoveCharacter(rig, { 3, 0, 0 });
		Simulate(scene, 10);
		// Half a meter walked: far more than one step's lag, which a rig carried with its entity would show.
		CHECK(BoneWorld(scene, rig, "LeftHand").x - BoneWorld(scene, rig, "LeftUpperArm").x < held - 0.1f);
		Simulate(scene, 50);
		CHECK(scene.GetWorldTransform(rig)[3].x == doctest::Approx(standing.x + 3).epsilon(0.05));
		CHECK(Near(BoneWorld(scene, rig, "Chest"), animatedChest(), 1e-3f));
		CHECK(Near(rootBody(), animatedChest(), 1e-3f));
		CHECK(glm::length(BoneWorld(scene, rig, "LeftHand") - BoneWorld(scene, rig, "LeftUpperArm")) < 0.61f);
		// A total impulse goes to the parts that can take it: all of it, not the root's share too.
		physics.MoveCharacter(rig, { 0, 0, 0 });
		Simulate(scene, 120);
		const glm::vec3 still = BoneWorld(scene, rig, "RightHand");
		REQUIRE(physics.ActivateRagdoll(rig, { 0, 0, 70 }));
		Simulate(scene, 3);
		CHECK(BoneWorld(scene, rig, "RightHand").z > still.z + 0.01f);
		CHECK(Near(BoneWorld(scene, rig, "Chest"), animatedChest(), 1e-3f));
		scene.OnSimulationStop();

		// All of it: a rig of a kinematic root and one limb gives the limb the whole impulse, not its share of two.
		const auto reach = [](const glm::vec3& impulse) {
			Scene twoJoints;
			twoJoints.GetPhysicsSettings().Gravity = glm::vec3(0);
			Entity entity = Rig(twoJoints);
			entity.GetComponent<RagdollComponent>().KinematicRoot = true;
			twoJoints.OnSimulationStart();
			auto& world = *twoJoints.GetPhysicsWorld();
			twoJoints.GetAnimationSystem()->Update(0);
			REQUIRE(world.ActivateRagdoll(entity));
			const float start = world.GetRagdollCapsules(entity)[1].End.x;
			REQUIRE(world.ActivateRagdoll(entity, impulse));
			Simulate(twoJoints, 2);
			const float moved = world.GetRagdollCapsules(entity)[1].End.x - start;
			twoJoints.OnSimulationStop();
			return moved;
		};
		CHECK(reach({ 35, 0, 0 }) == doctest::Approx(0.0435542f).scale(0).epsilon(0.005));
	}

	TEST_CASE("Limbs stay out of the bodies their rig keeps, and collide with everything else")
	{
		ClearAssetCache clear;
		enum class Holder
		{
			Character,
			ParentCharacter,
			DynamicBody
		};
		for (const Holder holder : { Holder::Character, Holder::ParentCharacter, Holder::DynamicBody })
		{
			CAPTURE(static_cast<int>(holder));
			Scene scene;
			Floor(scene);
			Entity rig = PartialBiped(scene, holder == Holder::Character);
			Entity owner = rig;
			if (holder == Holder::ParentCharacter)
			{
				owner = scene.CreateEntity("Controller");
				owner.AddComponent<CharacterControllerComponent>();
				auto& capsule = owner.AddComponent<CapsuleColliderComponent>();
				capsule.Offset = { 0, 0.9f, 0 };
				capsule.Radius = 0.5f;
				capsule.HalfHeight = 0.5f;
				scene.SetParent(rig, owner, false);
			}
			else if (holder == Holder::DynamicBody)
			{
				auto& body = rig.AddComponent<RigidBodyComponent>();
				body.Type = RigidBodyType::Dynamic;
				body.GravityFactor = 0;
				body.FixedRotation = true;
				rig.GetTransform().Translation.y = 0.1f;
				auto& capsule = rig.AddComponent<CapsuleColliderComponent>();
				capsule.Offset = { 0, 1, 0 };
				capsule.Radius = 0.5f;
				capsule.HalfHeight = 0.5f;
			}
			scene.OnSimulationStart();
			auto& physics = *scene.GetPhysicsWorld();
			scene.GetAnimationSystem()->Update(0);
			Simulate(scene, 10);
			const glm::vec3 start = scene.GetWorldTransform(owner)[3];
			REQUIRE(physics.ActivateRagdoll(rig));
			// Dropped limbs overlapping the capsule from the start push nothing.
			Simulate(scene, 90);
			CHECK(Near(glm::vec3(scene.GetWorldTransform(owner)[3]), start, 2e-3f));
			scene.OnSimulationStop();
		}

		// Other bodies do meet them: a static box stops a thrown hand.
		Scene scene;
		Entity rig = PartialBiped(scene);
		scene.GetPhysicsSettings().Gravity = glm::vec3(0);
		auto box = scene.CreateEntity("Box");
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		scene.GetAnimationSystem()->Update(0);
		box.GetTransform().Translation = BoneWorld(scene, rig, "LeftHand") + glm::vec3(0, 0, -0.3f);
		box.AddComponent<RigidBodyComponent>();
		box.AddComponent<BoxColliderComponent>().HalfExtents = { 0.1f, 0.1f, 0.1f };
		Simulate(scene, 1);
		REQUIRE(physics.ActivateRagdoll(rig, { 0, 0, -60 }));
		Simulate(scene, 60);
		CHECK(BoneWorld(scene, rig, "LeftHand").z > box.GetTransform().Translation.z);
		scene.OnSimulationStop();
	}

	TEST_CASE("A recovering partial rig leaves its entity alone and ends only its own contacts")
	{
		ClearAssetCache clear;
		Scene scene;
		Floor(scene);
		Entity rig = PartialBiped(scene);
		Zone(scene);
		Contacts listener;
		scene.SetEventListener(&listener);
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		scene.GetAnimationSystem()->Update(0);
		Simulate(scene, 30);
		CHECK(listener.Begin == 1);
		const glm::vec3 place = scene.GetWorldTransform(rig)[3];
		const glm::vec3 animatedHand = BoneWorld(scene, rig, "LeftHand");
		REQUIRE(physics.ActivateRagdoll(rig));
		Simulate(scene, 30);
		const glm::vec3 droppedHand = BoneWorld(scene, rig, "LeftHand");
		CHECK(glm::length(droppedHand - animatedHand) > 0.1f);
		// Recovering ends the limbs' overlap with the zone, but the character's body is still in it.
		REQUIRE(physics.RecoverRagdoll(rig, "", 0.25f));
		CHECK_FALSE(physics.IsRagdollActive(rig));
		CHECK(physics.HasRagdollPose(rig));
		CHECK_FALSE(physics.RagdollOwnsRoot(rig));
		CHECK(physics.HasCharacter(rig));
		CHECK(Near(glm::vec3(scene.GetWorldTransform(rig)[3]), place, 1e-4f));
		CHECK(Near(BoneWorld(scene, rig, "LeftHand"), droppedHand, 1e-3f));
		Simulate(scene, 7);
		CHECK(listener.End == 0);
		// Walking on while it blends: the blend follows the entity, and the root stays on the animation.
		physics.MoveCharacter(rig, { 2, 0, 0 });
		Simulate(scene, 3);
		CHECK(BoneWorld(scene, rig, "Chest").x == doctest::Approx(scene.GetWorldTransform(rig)[3].x).epsilon(1e-3));
		Simulate(scene, 10);
		CHECK_FALSE(physics.HasRagdollPose(rig));
		CHECK(Near(BoneWorld(scene, rig, "LeftHand") - glm::vec3(scene.GetWorldTransform(rig)[3]), animatedHand - place, 1e-3f));
		CHECK(listener.End == 0);
		scene.SetEventListener(nullptr);
		scene.OnSimulationStop();
	}

	TEST_CASE("Separation contacts of a partial rig come from the limb that parted")
	{
		ClearAssetCache clear;
		Scene scene;
		Entity rig = PartialBiped(scene, false);
		rig.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		auto& capsule = rig.AddComponent<CapsuleColliderComponent>();
		capsule.Offset = { 0, 0.5f, 0 };
		capsule.Radius = 0.1f;
		capsule.HalfHeight = 0.2f;
		Contacts listener;
		std::optional<ContactInfo> parted;
		scene.SetEventListener(&listener);
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		scene.GetPhysicsWorld()->SetGravity(glm::vec3(0));
		scene.GetAnimationSystem()->Update(0);
		// A box a hand's breadth under the left hand, far from the capsule by the hips.
		auto box = scene.CreateEntity("Box");
		box.GetTransform().Translation = BoneWorld(scene, rig, "LeftHand") + glm::vec3(0, -0.15f, 0);
		box.AddComponent<RigidBodyComponent>();
		box.AddComponent<BoxColliderComponent>().HalfExtents = { 0.1f, 0.05f, 0.1f };
		Simulate(scene, 1);
		listener.Parted = &parted;
		REQUIRE(physics.ActivateRagdoll(rig, { 0, -80, 0 }));
		Simulate(scene, 30);
		CHECK(listener.CollisionBegin == 1);
		REQUIRE(physics.ActivateRagdoll(rig, { 0, 200, 0 }));
		Simulate(scene, 30);
		CHECK(listener.CollisionEnd == 1);
		REQUIRE(parted);
		CHECK(glm::length(parted->Point - box.GetTransform().Translation) < 0.4f);
		scene.SetEventListener(nullptr);
		scene.OnSimulationStop();
	}

	TEST_CASE("Only a teleport carries a kinematic root's rig along")
	{
		const glm::quat none(1, 0, 0, 0);
		const glm::quat turned = glm::angleAxis(glm::radians(100.0f), glm::vec3(0, 1, 0));
		const glm::quat small = glm::angleAxis(glm::radians(80.0f), glm::vec3(0, 1, 0));
		const float step = 1.0f / 60.0f;
		// 30 m/s is half a meter per step here, and twice that per step at half the rate.
		CHECK_FALSE(IsRagdollTeleport({ 0.4f, 0, 0 }, none, none, step));
		CHECK(IsRagdollTeleport({ 0.6f, 0, 0 }, none, none, step));
		CHECK_FALSE(IsRagdollTeleport({ 0.6f, 0, 0 }, none, none, 2 * step));
		CHECK(IsRagdollTeleport({ 0, 0, 0 }, turned, none, step));
		CHECK_FALSE(IsRagdollTeleport({ 0, 0, 0 }, small, none, step));
		// The double cover: a quaternion and its negative are one rotation.
		CHECK_FALSE(IsRagdollTeleport({ 0, 0, 0 }, -none, none, step));

		ClearAssetCache clear;
		Scene scene;
		Floor(scene);
		Entity rig = PartialBiped(scene);
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		scene.GetAnimationSystem()->Update(0);
		Simulate(scene, 10);
		REQUIRE(physics.ActivateRagdoll(rig));
		Simulate(scene, 60);
		const glm::vec3 reach = BoneWorld(scene, rig, "LeftHand") - BoneWorld(scene, rig, "LeftUpperArm");
		rig.GetTransform().Translation.x += 15;
		Simulate(scene, 1);
		// The arms come along as they hung, not stretched across the jump and not flung.
		CHECK(Near(BoneWorld(scene, rig, "LeftHand") - BoneWorld(scene, rig, "LeftUpperArm"), reach, 0.05f));
		CHECK(Near(BoneWorld(scene, rig, "Chest"), glm::vec3(scene.GetWorldTransform(rig)[3]) + glm::vec3(0, 1.4f, 0), 0.02f));
		Simulate(scene, 30);
		CHECK(glm::length(BoneWorld(scene, rig, "LeftHand") - BoneWorld(scene, rig, "LeftUpperArm")) < 0.61f);
		// A half turn in one frame carries them too, turned with it, and a swing goes on the turned way.
		REQUIRE(physics.ActivateRagdoll(rig, { 0, 0, 280 }));
		Simulate(scene, 1);
		const glm::vec3 side = BoneWorld(scene, rig, "LeftHand") - BoneWorld(scene, rig, "Chest");
		rig.GetTransform().Rotation = glm::angleAxis(glm::pi<float>(), glm::vec3(0, 1, 0)) * rig.GetTransform().Rotation;
		Simulate(scene, 1);
		const glm::vec3 turnedSide = BoneWorld(scene, rig, "LeftHand") - BoneWorld(scene, rig, "Chest");
		CHECK(turnedSide.x == doctest::Approx(-side.x).epsilon(0.1));
		Simulate(scene, 1);
		CHECK((BoneWorld(scene, rig, "LeftHand") - BoneWorld(scene, rig, "Chest")).z < turnedSide.z);
		scene.OnSimulationStop();
	}

	TEST_CASE("Turning KinematicRoot on or off hands the entity's body over")
	{
		ClearAssetCache clear;
		Scene scene;
		Floor(scene);
		Entity rig = PartialBiped(scene);
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		scene.GetAnimationSystem()->Update(0);
		Simulate(scene, 10);
		REQUIRE(physics.ActivateRagdoll(rig));
		Simulate(scene, 10);
		auto& ragdoll = rig.GetComponent<RagdollComponent>();
		CHECK(physics.HasCharacter(rig));
		const uint64_t builds = physics.GetRagdollBuildCount();
		ragdoll.KinematicRoot = false;
		Simulate(scene, 2);
		CHECK(physics.GetRagdollBuildCount() == builds + 1);
		CHECK_FALSE(physics.HasCharacter(rig));
		CHECK(physics.RagdollOwnsRoot(rig));
		ragdoll.KinematicRoot = true;
		Simulate(scene, 2);
		CHECK(physics.GetRagdollBuildCount() == builds + 2);
		CHECK(physics.HasCharacter(rig));
		CHECK_FALSE(physics.RagdollOwnsRoot(rig));
		CHECK(physics.IsRagdollActive(rig));
		// Stopping the rig leaves the character it found.
		ragdoll.Enabled = false;
		Simulate(scene, 60);
		CHECK_FALSE(physics.HasRagdollPose(rig));
		CHECK(physics.HasCharacter(rig));
		scene.OnSimulationStop();
	}

	TEST_CASE("Partial rigs leave root motion, IK and springs outside them to the animation")
	{
		ClearAssetCache clear;
		TempDirectory directory("RagdollRootMotion");
		auto model = ReadJson(FixturePath("TwoJoint.gltf"));
		const float drift[9] = { 0, 0, 0, 1, 0, 0, 2, 0, 0 };
		AddVectorClip(model, "Drift", 2, "translation", drift);
		const auto path = directory.GetPath() / "Drift.gltf";
		REQUIRE(FileSystem::WriteTextFile(path, model.dump()));
		const auto walked = [&](bool kinematicRoot) {
			ClearAssetCache cache;
			Scene scene;
			Entity rig = Rig(scene, true);
			rig.GetComponent<RagdollComponent>().KinematicRoot = kinematicRoot;
			auto& animator = rig.GetComponent<AnimatorComponent>();
			animator.Mesh = path.string();
			animator.Clip = "Drift";
			animator.PlayOnStart = true;
			animator.RootMotion = true;
			animator.RootJoint = "RootJoint";
			scene.OnSimulationStart();
			const float start = scene.GetWorldTransform(rig)[3].x;
			Simulate(scene, 130);
			const float moved = scene.GetWorldTransform(rig)[3].x;
			scene.OnSimulationStop();
			return moved - start;
		};
		// The entity walks with the root under a kinematic root, and stays where it is under a ragdoll of the whole body.
		CHECK(walked(true) > 0.2f);
		CHECK(walked(false) == 0);

		// The legs keep their IK as its target moves, and an arm the rig owns is the rig's, whatever chains say about it.
		Scene scene;
		Floor(scene);
		Entity rig = PartialBiped(scene);
		auto target = scene.CreateEntity("Target");
		target.GetTransform().Translation = { 0.1f, 0.4f, 0.2f };
		auto& ik = rig.AddComponent<InverseKinematicsComponent>();
		ik.Target = target.GetUUID();
		ik.Chains.push_back({ .Name = "Leg", .Type = IKChainType::TwoBone, .Joint = "LeftFoot", .FollowTarget = true });
		auto& springs = rig.AddComponent<SpringBoneComponent>();
		springs.Chains.push_back({ .Name = "RightLeg", .Joint = "RightUpperLeg", .Stiffness = 0, .GravityFactor = 0 });
		springs.Chains.push_back({ .Name = "Arm", .Joint = "RightUpperArm", .Stiffness = 0 });
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		scene.GetAnimationSystem()->Update(0);
		Simulate(scene, 10);
		REQUIRE(physics.ActivateRagdoll(rig));
		rig.GetComponent<SpringBoneComponent>().Wind = { 0, 0, 20 };
		target.GetTransform().Translation.z = 0.1f;
		Simulate(scene, 60);
		CHECK(Near(BoneWorld(scene, rig, "LeftFoot"), glm::vec3(scene.GetWorldTransform(target)[3]), 1e-2f));
		// The wind swings the leg's chain, which is outside the rig.
		CHECK(BoneWorld(scene, rig, "RightFoot").z > 0.2f);
		const auto pose = physics.GetRagdollPose(rig);
		REQUIRE(pose);
		const auto source = LoadBiped();
		const uint32_t hand = Node(*source, "RightHand");
		const auto physical = std::ranges::find(pose->Joints, hand, &std::pair<uint32_t, glm::mat4>::first);
		REQUIRE(physical != pose->Joints.end());
		CHECK(Near(BoneWorld(scene, rig, "RightHand"), glm::vec3(scene.GetWorldTransform(rig) * physical->second[3]), 2e-2f));
		scene.OnSimulationStop();
	}

	TEST_CASE("A chain below a partial rig hangs from the physical joint")
	{
		ClearAssetCache clear;
		TempDirectory directory("RagdollPartialHair");
		// Hair on the rig's tip, not a skin joint, so the rig leaves it to the spring.
		auto model = ReadJson(FixturePath("TwoJoint.gltf"));
		const auto tip = model["skins"][0]["joints"][1].get<size_t>();
		const size_t hair = model["nodes"].size();
		model["nodes"].push_back({ { "name", "Hair" }, { "translation", { 0.1f, 0, 0 } }, { "children", { hair + 1 } } });
		model["nodes"].push_back({ { "name", "HairTip" }, { "translation", { 0.3f, 0, 0 } } });
		model["nodes"][tip]["children"] = json::array({ hair });
		const auto path = directory.GetPath() / "Hair.gltf";
		REQUIRE(FileSystem::WriteTextFile(path, model.dump()));
		const auto hanging = [&] {
			Scene scene;
			Entity rig = Rig(scene);
			rig.GetComponent<AnimatorComponent>().Mesh = path.string();
			auto& ragdoll = rig.GetComponent<RagdollComponent>();
			ragdoll.KinematicRoot = true;
			rig.AddComponent<SpringBoneComponent>().Chains.push_back({ .Name = "Hair", .Joint = "Hair", .Stiffness = 0, .Damping = 8 });
			scene.OnSimulationStart();
			auto& physics = *scene.GetPhysicsWorld();
			scene.GetAnimationSystem()->Update(0);
			REQUIRE(physics.ActivateRagdoll(rig));
			Simulate(scene, 240);
			const glm::vec3 direction = glm::normalize(BoneWorld(scene, rig, "HairTip") - BoneWorld(scene, rig, "Hair"));
			scene.OnSimulationStop();
			return direction;
		};
		// Whatever way the limp tip ended up, the hair hangs straight down from where it is.
		CHECK(Near(hanging(), glm::vec3(0, -1, 0), 0.02f));
	}

	TEST_CASE("Balance keeps a powered biped standing and gives way with its motors")
	{
		ClearAssetCache clear;
		BalancingBiped balanced(1);
		CHECK_FALSE(balanced.physics->IsRagdollBalanced(balanced.rig));
		balanced.Run(300);
		CHECK(balanced.physics->IsRagdollBalanced(balanced.rig));
		CHECK(balanced.Hips().y > 0.95f);
		CHECK(balanced.Upright() > 0.98f);
		// Motors alone cannot hold a body up: without balance it folds.
		BalancingBiped unbalanced(0);
		unbalanced.Run(300);
		CHECK_FALSE(unbalanced.physics->IsRagdollBalanced(unbalanced.rig));
		CHECK(unbalanced.Hips().y < 0.3f);

		// Balance is settled without rebuilding, and a fade of the motors takes it along.
		const uint64_t builds = balanced.physics->GetRagdollBuildCount();
		auto& ragdoll = balanced.rig.GetComponent<RagdollComponent>();
		ragdoll.BalanceFrequency = 2;
		ragdoll.BalanceDamping = 0.8f;
		ragdoll.BalanceMaxTilt = 50;
		balanced.Run(60);
		CHECK(balanced.physics->GetRagdollBuildCount() == builds);
		CHECK(balanced.Hips().y > 0.9f);
		// Several steps in a frame apply it for each of them, or the rig would sink at low frame rates.
		for (int i = 0; i < 40; i++)
			balanced.scene.OnUpdate(3.0f * Step);
		CHECK(balanced.Hips().y > 0.9f);
		// A weak motor strength scales balance too: far too little to stand.
		REQUIRE(balanced.physics->SetRagdollMotorStrength(balanced.rig, 0.01f, 0));
		balanced.Run(180);
		CHECK(balanced.Hips().y < 0.9f);

		// At no strength at all there is no balance.
		BalancingBiped fading(1);
		fading.Run(60);
		REQUIRE(fading.physics->SetRagdollMotorStrength(fading.rig, 0, 0.25f));
		fading.Run(120);
		CHECK_FALSE(fading.physics->IsRagdollBalanced(fading.rig));
		CHECK(fading.Hips().y < 0.5f);
	}

	TEST_CASE("Balance leans on walls and floors as a body does")
	{
		ClearAssetCache clear;
		// Falling past a wall that brushes its leg: a wall holds nothing up.
		{
			BalancingBiped rig(1, 8);
			Contacts listener;
			rig.scene.SetEventListener(&listener);
			auto wall = rig.scene.CreateEntity("Wall");
			// Taller than the fall, with a face only the lower legs reach, as they sit a little forward.
			wall.GetTransform().Translation = { 0, 5, 1.08f };
			wall.AddComponent<RigidBodyComponent>();
			wall.AddComponent<BoxColliderComponent>().HalfExtents = { 2, 6, 1 };
			for (int i = 0; i < 60; i++)
			{
				rig.Run(1);
				CHECK_FALSE(rig.physics->IsRagdollBalanced(rig.rig));
			}
			CHECK(listener.CollisionBegin > 0);
			rig.scene.SetEventListener(nullptr);
		}
		// On a floor that moves: the rig goes with it, as feet with grip would.
		{
			BalancingBiped rig(1, 1);
			auto platform = rig.scene.CreateEntity("Platform");
			platform.GetTransform().Translation = { 0, 0.5f, 0 };
			platform.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
			platform.AddComponent<BoxColliderComponent>().HalfExtents = { 8, 0.5f, 8 };
			rig.Run(120);
			REQUIRE(rig.physics->IsRagdollBalanced(rig.rig));
			const float start = rig.Hips().x;
			for (int i = 0; i < 150; i++)
			{
				platform.GetTransform().Translation.x += 1.0f / 60.0f;
				rig.Run(1);
			}
			CHECK(rig.Hips().x - start == doctest::Approx(2.5f).scale(0).epsilon(0.25));
		}
		// Ground that tilts underfoot stops being ground: the contact stays, and what it faces now counts.
		{
			BalancingBiped rig(1, 1);
			auto platform = rig.scene.CreateEntity("Platform");
			platform.GetTransform().Translation = { 0, 0.5f, 0 };
			platform.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
			platform.AddComponent<BoxColliderComponent>().HalfExtents = { 8, 0.5f, 8 };
			rig.Run(120);
			REQUIRE(rig.physics->IsRagdollBalanced(rig.rig));
			for (int i = 1; i <= 70; i++)
			{
				platform.GetTransform().Rotation = glm::angleAxis(glm::radians(static_cast<float>(i)), glm::vec3(0, 0, 1));
				rig.Run(1);
			}
			CHECK_FALSE(rig.physics->IsRagdollBalanced(rig.rig));
		}
		// How steep a surface still holds a rig up: a ramp of 45 degrees does, one of 70 does not.
		for (const float degrees : { 45.0f, 70.0f })
		{
			CAPTURE(degrees);
			BalancingBiped rig(1, 3);
			auto ramp = rig.scene.CreateEntity("Ramp");
			ramp.GetTransform().Translation = rig.Bone("LeftFoot") + glm::vec3(0, -0.8f, 0);
			ramp.GetTransform().Rotation = glm::angleAxis(glm::radians(degrees), glm::vec3(0, 0, 1));
			ramp.AddComponent<RigidBodyComponent>();
			ramp.AddComponent<BoxColliderComponent>().HalfExtents = { 3, 0.1f, 3 };
			bool balanced = false;
			for (int i = 0; i < 40; i++)
			{
				rig.Run(1);
				balanced = balanced || rig.physics->IsRagdollBalanced(rig.rig);
			}
			CHECK(balanced == (degrees < 60));
		}
		// Whichever entity the contact lists first.
		for (const UUID floorId : { UUID(100), UUID(300) })
		{
			CAPTURE(static_cast<uint64_t>(floorId));
			Scene scene;
			Entity floor = scene.CreateEntityWithUUID(floorId, "Floor");
			floor.GetTransform().Translation.y = -0.5f;
			floor.AddComponent<RigidBodyComponent>();
			floor.AddComponent<BoxColliderComponent>().HalfExtents = { 20, 0.5f, 20 };
			Entity biped = scene.CreateEntityWithUUID(UUID(200), "Biped");
			auto& animator = biped.AddComponent<AnimatorComponent>();
			animator.Mesh = BipedPath().string();
			animator.PlayOnStart = false;
			auto& ragdoll = biped.AddComponent<RagdollComponent>();
			ragdoll.MotorStrength = 1;
			ragdoll.BalanceStrength = 1;
			scene.OnSimulationStart();
			scene.GetAnimationSystem()->Update(0);
			REQUIRE(scene.GetPhysicsWorld()->ActivateRagdoll(biped));
			Simulate(scene, 60);
			CHECK(scene.GetPhysicsWorld()->IsRagdollBalanced(biped));
			scene.OnSimulationStop();
		}
		// A fall ends on the feet without a leap: the lift is capped.
		{
			BalancingBiped rig(1, 3);
			float peak = 0;
			bool landed = false;
			for (int i = 0; i < 180; i++)
			{
				rig.Run(1);
				landed = landed || rig.physics->IsRagdollBalanced(rig.rig);
				if (landed)
					peak = std::max(peak, rig.Hips().y);
			}
			CHECK(landed);
			CHECK(peak < 1.4f);
		}
		// One part: nothing to turn it through but its own torque, and it stands on itself.
		{
			Scene scene;
			Floor(scene);
			Entity tip = Rig(scene);
			tip.GetTransform().Translation.y = -0.85f;
			auto& ragdoll = tip.GetComponent<RagdollComponent>();
			ragdoll.RootJoint = "Tip";
			ragdoll.MotorStrength = 1;
			ragdoll.BalanceStrength = 1;
			scene.OnSimulationStart();
			scene.GetAnimationSystem()->Update(0);
			REQUIRE(scene.GetPhysicsWorld()->ActivateRagdoll(tip));
			Simulate(scene, 240);
			CHECK(scene.GetPhysicsWorld()->IsRagdollBalanced(tip));
			const RagdollCapsule capsule = scene.GetPhysicsWorld()->GetRagdollCapsules(tip)[0];
			CHECK(glm::normalize(capsule.End - capsule.Start).y > 0.95f);
			scene.OnSimulationStop();
		}
		// It only pushes up: a hop leaves the ground.
		{
			BalancingBiped rig(1);
			rig.Run(120);
			const float standing = rig.Hips().y;
			REQUIRE(rig.physics->ActivateRagdoll(rig.rig, { 0, 245, 0 }));
			float peak = 0;
			for (int i = 0; i < 40; i++)
			{
				rig.Run(1);
				peak = std::max(peak, rig.Hips().y);
			}
			CHECK(peak > standing + 0.3f);
		}
		// A limit of 0 needs an exact upright that no simulated rig holds.
		{
			BalancingBiped rig(1);
			rig.Run(60);
			rig.rig.GetComponent<RagdollComponent>().BalanceMaxTilt = 0;
			rig.Run(180);
			CHECK_FALSE(rig.physics->IsRagdollBalanced(rig.rig));
			CHECK(rig.Hips().y < 0.5f);
		}
		// A limit of 180 never gives up: a rig thrown into the air and landing anyhow stays finite.
		{
			BalancingBiped rig(1);
			rig.rig.GetComponent<RagdollComponent>().BalanceMaxTilt = 180;
			rig.Run(60);
			REQUIRE(rig.physics->ActivateRagdoll(rig.rig, { 0, 700, 0 }));
			rig.Run(120);
			CHECK(Math::IsFinite(rig.Hips()));
		}
	}

	TEST_CASE("Balance recovers from a push, brakes it, and loses to what tilts it too far")
	{
		ClearAssetCache clear;
		BalancingBiped rig(1);
		rig.Run(120);
		const glm::vec3 before = rig.Hips();
		REQUIRE(rig.physics->ActivateRagdoll(rig.rig, { 150, 0, 0 }));
		rig.Run(240);
		CHECK(rig.physics->IsRagdollBalanced(rig.rig));
		CHECK(rig.Upright() > 0.98f);
		CHECK(rig.Hips().y > 0.95f);
		// Pushed, then brought to a stop by its feet.
		CHECK(rig.Hips().x > before.x + 0.1f);
		CHECK(rig.Hips().x < before.x + 1.5f);
		const glm::vec3 stopped = rig.Hips();
		rig.Run(60);
		CHECK(glm::length(rig.Hips() - stopped) < 0.1f);

		// A heavy blow at the chest tilts it past BalanceMaxTilt, and from there it falls.
		rig.rig.GetComponent<RagdollComponent>().BalanceMaxTilt = 20;
		auto ball = rig.scene.CreateEntity("Ball");
		ball.GetTransform().Translation = rig.Bone("Chest") + glm::vec3(-1.5f, 0.1f, 0);
		auto& body = ball.AddComponent<RigidBodyComponent>();
		body.Type = RigidBodyType::Dynamic;
		body.Mass = 400;
		body.GravityFactor = 0;
		ball.AddComponent<SphereColliderComponent>().Radius = 0.3f;
		rig.Run(1);
		rig.physics->SetLinearVelocity(ball, { 25, 0, 0 });
		rig.Run(300);
		CHECK_FALSE(rig.physics->IsRagdollBalanced(rig.rig));
		CHECK(rig.Hips().y < 0.5f);
	}

	TEST_CASE("Balance needs something below the center of mass to stand on")
	{
		ClearAssetCache clear;
		// In the air: no support, no balance. A trigger around it is no support either.
		{
			BalancingBiped rig(1, 6);
			Zone(rig.scene);
			for (int i = 0; i < 40; i++)
			{
				rig.Run(1);
				CHECK_FALSE(rig.physics->IsRagdollBalanced(rig.rig));
			}
			// Free fall from rest: nothing lifted it.
			CHECK(rig.Hips().y < 7 - 0.5f * 9.81f * (40.0f / 60.0f) * (40.0f / 60.0f) + 0.15f);
		}
		// Hanging by an arm on a ledge, its touching parts above the center of mass: that holds nothing up either.
		{
			BalancingBiped rig(1, 6);
			auto ledge = rig.scene.CreateEntity("Ledge");
			ledge.GetTransform().Translation = rig.Bone("LeftForeArm") + glm::vec3(0, -0.35f, 0);
			ledge.AddComponent<RigidBodyComponent>();
			ledge.AddComponent<BoxColliderComponent>().HalfExtents = { 0.25f, 0.05f, 0.25f };
			Contacts listener;
			rig.scene.SetEventListener(&listener);
			for (int i = 0; i < 60; i++)
			{
				rig.Run(1);
				CHECK_FALSE(rig.physics->IsRagdollBalanced(rig.rig));
			}
			CHECK(listener.CollisionBegin > 0);
			rig.scene.SetEventListener(nullptr);
		}
		// Without gravity nothing is up, which is said once.
		{
			BalancingBiped rig(1);
			rig.Run(60);
			REQUIRE(rig.physics->IsRagdollBalanced(rig.rig));
			const auto since = Log::GetHistory().GetTotalCount();
			rig.physics->SetGravity(glm::vec3(0));
			rig.Run(30);
			CHECK_FALSE(rig.physics->IsRagdollBalanced(rig.rig));
			CHECK(CountLogMessages(since, "balance has no gravity to stand against; it is off") == 1);
		}
	}

	TEST_CASE("Balance holds the animated uprightness, whichever way gravity makes up")
	{
		ClearAssetCache clear;
		// Up is against gravity: a world on its side, with a rig and floor turned to match, stands just the same.
		{
			const glm::quat side = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0, 0, -1));
			BalancingBiped rig(1, 0, side, glm::vec3(-9.81f, 0, 0));
			rig.Run(240);
			CHECK(rig.physics->IsRagdollBalanced(rig.rig));
			CHECK(glm::normalize(rig.Bone("Chest") - rig.Hips()).x > 0.98f);
		}
		// An entity leaning 10 degrees: the animation's up leans with it, so that lean is what balance holds.
		const glm::quat lean = glm::angleAxis(glm::radians(10.0f), glm::vec3(0, 0, 1));
		BalancingBiped rig(1, 0, lean);
		rig.Run(240);
		CHECK(rig.physics->IsRagdollBalanced(rig.rig));
		const glm::vec3 spine = glm::normalize(rig.Bone("Chest") - rig.Bone("Hips"));
		CHECK(glm::degrees(std::acos(std::clamp(glm::dot(spine, lean * glm::vec3(0, 1, 0)), -1.0f, 1.0f))) < 5.0f);
	}

	TEST_CASE("Balance settings are validated, warned about and clamped")
	{
		ClearAssetCache clear;
		Scene scene;
		Entity rig = PartialBiped(scene);
		auto& ragdoll = rig.GetComponent<RagdollComponent>();
		ragdoll.BalanceStrength = 1;
		const auto warned = [&](const char* text) { return std::ranges::count(GetAnimationWarnings(rig), std::string(text)); };
		CHECK(warned("Ragdoll BalanceStrength has no effect: a KinematicRoot rig cannot fall") == 1);
		ragdoll.KinematicRoot = false;
		CHECK(warned("Ragdoll BalanceStrength has no effect: a KinematicRoot rig cannot fall") == 0);
		ragdoll.MotorTorque = 0;
		CHECK(warned("Ragdoll BalanceStrength has no effect: balance needs running motors, and MotorTorque is 0") == 1);
		ragdoll.MotorTorque = 500;
		CHECK(warned("Ragdoll BalanceStrength has no effect while the motor strength is 0: set MotorStrength, or raise it from a script") == 1);
		ragdoll.MotorStrength = 1;
		CHECK(warned("Ragdoll BalanceStrength has no effect while the motor strength is 0: set MotorStrength, or raise it from a script") == 0);
		ragdoll.BalanceMaxTilt = 0;
		CHECK(warned("Ragdoll BalanceStrength has no effect: BalanceMaxTilt is 0") == 1);
		ragdoll.BalanceMaxTilt = 60;
		ragdoll.Bones.push_back({ .Joint = "Chest", .MotorStrength = 0.5f });
		CHECK(warned("Ragdoll Bones: 'Chest' is the rig's root, with no joint above it for MotorStrength to scale") == 1);
		// With no RootJoint the root is the skeleton's.
		ragdoll.RootJoint.clear();
		CHECK(warned("Ragdoll Bones: 'Chest' is the rig's root, with no joint above it for MotorStrength to scale") == 0);
		ragdoll.Bones[0].Joint = "Hips";
		CHECK(warned("Ragdoll Bones: 'Hips' is the rig's root, with no joint above it for MotorStrength to scale") == 1);
		ragdoll.RootJoint = "Chest";
		ragdoll.Bones.clear();
		// A root with nothing below it, in the inspector before any activation fails.
		ragdoll.KinematicRoot = true;
		ragdoll.RootJoint = "Head";
		CHECK(warned("Ragdoll KinematicRoot leaves nothing to simulate: no skin joint hangs from the root") == 1);
		ragdoll.RootJoint = "Chest";
		ragdoll.KinematicRoot = false;
		// A chain on the rig's joints waits for it; one outside it, or on its root, does not.
		ragdoll.KinematicRoot = true;
		auto& springs = rig.AddComponent<SpringBoneComponent>();
		springs.Chains.push_back({ .Name = "Arm", .Joint = "LeftUpperArm" });
		springs.Chains.push_back({ .Name = "Leg", .Joint = "LeftUpperLeg" });
		springs.Chains.push_back({ .Name = "Root", .Joint = "Chest", .ChainLength = 1 });
		CHECK(warned("SpringBone chain 'Arm': its joints belong to the Ragdoll, so it does not swing while the rig is active") == 1);
		CHECK(warned("SpringBone chain 'Leg': its joints belong to the Ragdoll, so it does not swing while the rig is active") == 0);
		CHECK(warned("SpringBone chain 'Root': its joints belong to the Ragdoll, so it does not swing while the rig is active") == 0);
		rig.RemoveComponent<SpringBoneComponent>();
		ragdoll.KinematicRoot = false;

		// A KinematicRoot rig never balances, whatever its settings.
		{
			Scene partial;
			Floor(partial);
			Entity entity = PartialBiped(partial);
			auto& settings = entity.GetComponent<RagdollComponent>();
			settings.MotorStrength = 1;
			settings.BalanceStrength = 1;
			partial.OnSimulationStart();
			partial.GetAnimationSystem()->Update(0);
			Simulate(partial, 5);
			REQUIRE(partial.GetPhysicsWorld()->ActivateRagdoll(entity));
			Simulate(partial, 30);
			CHECK_FALSE(partial.GetPhysicsWorld()->IsRagdollBalanced(entity));
			partial.OnSimulationStop();
		}

		// Values C++ wrote past validation are clamped and said so once.
		BalancingBiped biped(1);
		auto& settings = biped.rig.GetComponent<RagdollComponent>();
		const auto since = Log::GetHistory().GetTotalCount();
		settings.BalanceStrength = std::numeric_limits<float>::quiet_NaN();
		settings.BalanceFrequency = 1000;
		settings.BalanceDamping = -1;
		settings.BalanceMaxTilt = 400;
		biped.Run(30);
		CHECK(CountLogMessages(since, "BalanceStrength must be in 0..1; clamping") == 1);
		CHECK(CountLogMessages(since, "BalanceFrequency must be in 0.1..30; clamping") == 1);
		CHECK(CountLogMessages(since, "BalanceDamping must be in 0..10; clamping") == 1);
		CHECK(CountLogMessages(since, "BalanceMaxTilt must be in 0..180; clamping") == 1);
		CHECK(Math::IsFinite(biped.Hips()));
	}

	TEST_CASE("Removing the body or character a rig keeps leaves the limbs' contacts alone")
	{
		ClearAssetCache clear;
		for (const bool character : { true, false })
		{
			CAPTURE(character);
			Scene scene;
			scene.GetPhysicsSettings().Gravity = glm::vec3(0);
			Entity rig = PartialBiped(scene, character);
			if (!character)
			{
				rig.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
				auto& capsule = rig.AddComponent<CapsuleColliderComponent>();
				capsule.Radius = 0.3f;
				capsule.HalfHeight = 0.5f;
			}
			Contacts listener;
			scene.SetEventListener(&listener);
			scene.OnSimulationStart();
			auto& physics = *scene.GetPhysicsWorld();
			scene.GetAnimationSystem()->Update(0);
			// A trigger around the hand alone.
			auto zone = scene.CreateEntity("HandZone");
			zone.GetTransform().Translation = BoneWorld(scene, rig, "LeftHand");
			zone.AddComponent<RigidBodyComponent>().IsTrigger = true;
			zone.AddComponent<BoxColliderComponent>().HalfExtents = { 0.1f, 0.1f, 0.1f };
			REQUIRE(physics.ActivateRagdoll(rig));
			Simulate(scene, 10);
			REQUIRE(listener.Begin == 1);
			// A collider edit rebuilds what the entity keeps, which touched nothing.
			scene.GetRegistry().patch<CapsuleColliderComponent>(rig, [](CapsuleColliderComponent& capsule) { capsule.Radius = 0.25f; });
			Simulate(scene, 10);
			CHECK(listener.End == 0);
			// The hand's real exit is still reported.
			zone.GetTransform().Translation.y += 5;
			Simulate(scene, 10);
			CHECK(listener.End == 1);
			scene.SetEventListener(nullptr);
			scene.OnSimulationStop();
		}
	}

	TEST_CASE("A rebuilt character does not stand on its own rig")
	{
		ClearAssetCache clear;
		Scene scene;
		Entity rig = PartialBiped(scene);
		rig.GetComponent<RagdollComponent>().RootJoint = "Hips";
		rig.GetTransform().Translation.y = 500;
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		physics.SetGravity({ 0, -2, 0 });
		scene.GetAnimationSystem()->Update(0);
		Simulate(scene, 2);
		REQUIRE(physics.ActivateRagdoll(rig));
		Simulate(scene, 90);
		// In the air, wanting to jump: a rebuild must not count the limbs under the capsule as ground.
		const float falling = physics.GetLinearVelocity(rig).y;
		REQUIRE(falling < -2.5f);
		scene.GetRegistry().patch<CapsuleColliderComponent>(rig, [](CapsuleColliderComponent& capsule) { capsule.HalfHeight = 0.45f; });
		physics.MoveCharacter(rig, { 0, 5, 0 });
		Simulate(scene, 1);
		CHECK_FALSE(physics.IsCharacterGrounded(rig));
		CHECK(physics.GetLinearVelocity(rig).y < falling + 0.1f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Continuous bodies pass through what they must not collide with")
	{
		ClearAssetCache clear;
		// The body a rig keeps, at a speed only Jolt's continuous collision sees, inside the rig's limbs.
		{
			Scene scene;
			scene.GetPhysicsSettings().Gravity = glm::vec3(0);
			Entity rig = PartialBiped(scene, false);
			auto& body = rig.AddComponent<RigidBodyComponent>();
			body.Type = RigidBodyType::Dynamic;
			body.Continuous = true;
			body.FixedRotation = true;
			auto& capsule = rig.AddComponent<CapsuleColliderComponent>();
			capsule.Radius = 0.1f;
			capsule.HalfHeight = 0.1f;
			scene.OnSimulationStart();
			auto& physics = *scene.GetPhysicsWorld();
			scene.GetAnimationSystem()->Update(0);
			REQUIRE(physics.ActivateRagdoll(rig));
			physics.SetLinearVelocity(rig, { 0, 120, 0 });
			Simulate(scene, 30);
			CHECK(scene.GetWorldTransform(rig)[3].y == doctest::Approx(60).epsilon(0.02));
			scene.OnSimulationStop();
		}
		// A joint without collision, as for any other pair.
		{
			Scene scene;
			scene.GetPhysicsSettings().Gravity = glm::vec3(0);
			const auto add = [&](const char* name, const glm::vec3& at) {
				Entity entity = scene.CreateEntity(name);
				entity.GetTransform().Translation = at;
				auto& body = entity.AddComponent<RigidBodyComponent>();
				body.Type = RigidBodyType::Dynamic;
				body.Continuous = true;
				entity.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.2f);
				return entity;
			};
			Entity shot = add("Shot", { 0, 0, 0 });
			Entity wall = add("Wall", { 3, 0, 0 });
			auto& joint = shot.AddComponent<JointComponent>();
			// Free along the shot's path, so nothing but a collision could stop it.
			joint.Type = JointType::Slider;
			joint.ConnectedEntity = wall.GetUUID();
			joint.Axis = { 1, 0, 0 };
			scene.OnSimulationStart();
			Simulate(scene, 1);
			scene.GetPhysicsWorld()->SetLinearVelocity(shot, { 240, 0, 0 });
			Simulate(scene, 1);
			CHECK(scene.GetWorldTransform(shot)[3].x > 3);
			scene.OnSimulationStop();
		}
	}

	TEST_CASE("A jump in the animated root carries the rig instead of flinging it")
	{
		ClearAssetCache clear;
		TempDirectory directory("RagdollRootJump");
		auto model = ReadJson(FixturePath("TwoJoint.gltf"));
		const float jump[9] = { 0, 0, 0, 1, 0, 0, 1.3f, 0, 0 };
		AddVectorClip(model, "Jump", 2, "translation", jump);
		const auto path = directory.GetPath() / "Jump.gltf";
		REQUIRE(FileSystem::WriteTextFile(path, model.dump()));
		Scene scene;
		scene.GetPhysicsSettings().Gravity = glm::vec3(0);
		Entity rig = Rig(scene);
		rig.GetComponent<RagdollComponent>().KinematicRoot = true;
		auto& animator = rig.GetComponent<AnimatorComponent>();
		animator.Mesh = path.string();
		animator.Clip = "Jump";
		animator.PlayOnStart = true;
		animator.Speed = 0;
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		auto& animation = *scene.GetAnimationSystem();
		animation.Update(0);
		REQUIRE(physics.ActivateRagdoll(rig));
		Simulate(scene, 2);
		const auto tip = [&] { return glm::vec3(physics.GetRagdollPose(rig)->Joints[1].second[3]) + glm::vec3(scene.GetWorldTransform(rig)[3]); };
		const glm::vec3 start = tip();
		REQUIRE(animation.Seek(rig, 1));
		Simulate(scene, 1);
		// One meter in one step is 60 m/s: the tip came along, and it stays there.
		const glm::vec3 landed = tip();
		CHECK(landed.x == doctest::Approx(start.x + 1).epsilon(0.01));
		Simulate(scene, 10);
		CHECK(glm::length(tip() - landed) < 0.02f);
		// 0.3 m in one step is a fast move, not a jump: the root takes it and the tip lags behind.
		REQUIRE(animation.Seek(rig, 2));
		Simulate(scene, 1);
		CHECK(tip().x < landed.x + 0.25f);
		scene.OnSimulationStop();
	}

	TEST_CASE("A frame's steps share the root's animated move, and a frame without one keeps the limbs on it")
	{
		ClearAssetCache clear;
		TempDirectory directory("RagdollRootSteps");
		auto model = ReadJson(FixturePath("TwoJoint.gltf"));
		const float jump[9] = { 0, 0, 0, 1.35f, 0, 0, 1.35f, 0, 0 };
		AddVectorClip(model, "Move", 2, "translation", jump);
		const auto path = directory.GetPath() / "Move.gltf";
		REQUIRE(FileSystem::WriteTextFile(path, model.dump()));
		Scene scene;
		scene.GetPhysicsSettings().Gravity = glm::vec3(0);
		Entity rig = Rig(scene);
		rig.GetComponent<RagdollComponent>().KinematicRoot = true;
		auto& animator = rig.GetComponent<AnimatorComponent>();
		animator.Mesh = path.string();
		animator.Clip = "Move";
		animator.PlayOnStart = true;
		animator.Speed = 0;
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		auto& animation = *scene.GetAnimationSystem();
		animation.Update(0);
		REQUIRE(physics.ActivateRagdoll(rig));
		Simulate(scene, 2);
		const auto reach = [&] {
			const auto pose = physics.GetRagdollPose(rig);
			return glm::vec3(pose->Joints[1].second[3]) - glm::vec3(pose->Joints[0].second[3]);
		};
		const glm::vec3 straight = reach();
		// 1.35 m over a three-step frame is 27 m/s for each step, under the limit of 30 only when shared evenly. A skewed share
		// would be a jump, carrying the tip rigidly.
		REQUIRE(animation.Seek(rig, 1));
		scene.OnUpdate(3.0f * Step);
		CHECK(glm::length(reach() - straight) > 0.01f);
		CHECK(glm::vec3(physics.GetRagdollPose(rig)->Joints[0].second[3]).x == doctest::Approx(1.35f).scale(0).epsilon(0.01));
		scene.OnSimulationStop();

		// No step at all: the entity moved on and the root body did not, and the limbs still hang from the animated root.
		Scene partial;
		partial.GetPhysicsSettings().Gravity = glm::vec3(0);
		Entity biped = PartialBiped(partial, false);
		partial.OnSimulationStart();
		partial.GetAnimationSystem()->Update(0);
		REQUIRE(partial.GetPhysicsWorld()->ActivateRagdoll(biped));
		Simulate(partial, 2);
		const glm::vec3 offset = BoneWorld(partial, biped, "LeftUpperArm") - BoneWorld(partial, biped, "Chest");
		biped.GetTransform().Translation.x += 0.2f;
		partial.OnUpdate(0.001f);
		CHECK(Near(BoneWorld(partial, biped, "LeftUpperArm") - BoneWorld(partial, biped, "Chest"), offset, 1e-3f));
		CHECK(BoneWorld(partial, biped, "Chest").x == doctest::Approx(0.2f).epsilon(1e-3));
		partial.OnSimulationStop();
	}

	TEST_CASE("A rig on a character's child moves with it and stays out of its capsule")
	{
		ClearAssetCache clear;
		const auto run = [](bool capsule) {
			Scene scene;
			Floor(scene);
			Entity controller = scene.CreateEntity("Controller");
			controller.AddComponent<CharacterControllerComponent>();
			auto& shape = controller.AddComponent<CapsuleColliderComponent>();
			shape.Offset = { 0, 0.9f, 0 };
			shape.Radius = capsule ? 0.7f : 0.05f;
			shape.HalfHeight = 0.5f;
			Entity rig = PartialBiped(scene, false);
			scene.SetParent(rig, controller, false);
			scene.OnSimulationStart();
			auto& physics = *scene.GetPhysicsWorld();
			scene.GetAnimationSystem()->Update(0);
			Simulate(scene, 10);
			physics.MoveCharacter(controller, { 4, 0, 0 });
			Simulate(scene, 20);
			REQUIRE(physics.ActivateRagdoll(rig));
			// The hand was moving with the character: just activated, it has not stayed behind.
			const glm::vec3 hand = BoneWorld(scene, rig, "LeftHand") - BoneWorld(scene, rig, "Chest");
			Simulate(scene, 2);
			const glm::vec3 after = BoneWorld(scene, rig, "LeftHand") - BoneWorld(scene, rig, "Chest");
			CHECK(after.x == doctest::Approx(hand.x).epsilon(0.02));
			physics.MoveCharacter(controller, { 0, 0, 0 });
			Simulate(scene, 120);
			const glm::vec3 hanging = BoneWorld(scene, rig, "LeftHand") - BoneWorld(scene, rig, "Chest");
			scene.OnSimulationStop();
			return hanging;
		};
		// A capsule around the limbs changes nothing: they neither hit it nor push it.
		CHECK(Near(run(true), run(false), 0.02f));

		// Moved under a controller while active, the rig joins it.
		Scene scene;
		Floor(scene);
		Entity controller = scene.CreateEntity("Controller");
		controller.AddComponent<CharacterControllerComponent>();
		auto& capsule = controller.AddComponent<CapsuleColliderComponent>();
		capsule.Offset = { 0, 0.9f, 0 };
		capsule.Radius = 0.7f;
		capsule.HalfHeight = 0.5f;
		Entity rig = PartialBiped(scene, false);
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		scene.GetAnimationSystem()->Update(0);
		rig.GetTransform().Translation.x = 5;
		Simulate(scene, 2);
		REQUIRE(physics.ActivateRagdoll(rig));
		Simulate(scene, 2);
		const glm::vec3 free = BoneWorld(scene, rig, "LeftHand") - BoneWorld(scene, rig, "Chest");
		scene.SetParent(rig, controller, false);
		rig.GetTransform().Translation.x = 0;
		Simulate(scene, 2);
		CHECK(Near(BoneWorld(scene, rig, "LeftHand") - BoneWorld(scene, rig, "Chest"), free, 0.05f));
		scene.OnSimulationStop();
	}

	TEST_CASE("A character that keeps a rig leaves a zone as one entity")
	{
		ClearAssetCache clear;
		Scene scene;
		Floor(scene);
		Entity rig = PartialBiped(scene);
		auto zone = scene.CreateEntity("Zone");
		zone.GetTransform().Translation = { 0, 1, 0 };
		zone.AddComponent<RigidBodyComponent>().IsTrigger = true;
		zone.AddComponent<BoxColliderComponent>().HalfExtents = { 2, 2, 2 };
		Contacts listener;
		scene.SetEventListener(&listener);
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		scene.GetAnimationSystem()->Update(0);
		Simulate(scene, 30);
		REQUIRE(listener.Begin == 1);
		REQUIRE(physics.ActivateRagdoll(rig));
		Simulate(scene, 10);
		CHECK(listener.Begin == 1);
		// Its body and its limbs are one entity in the zone: leaving is one exit, when the last of them has left.
		physics.MoveCharacter(rig, { 4, 0, 0 });
		Simulate(scene, 120);
		CHECK(listener.End == 1);
		scene.SetEventListener(nullptr);
		scene.OnSimulationStop();
	}

	TEST_CASE("Contact callbacks can destroy a partial rig as it stops")
	{
		ClearAssetCache clear;
		for (const bool recover : { true, false })
		{
			CAPTURE(recover);
			Scene scene;
			scene.GetPhysicsSettings().Gravity = glm::vec3(0);
			Entity rig = PartialBiped(scene);
			Contacts listener;
			scene.SetEventListener(&listener);
			scene.OnSimulationStart();
			auto& physics = *scene.GetPhysicsWorld();
			scene.GetAnimationSystem()->Update(0);
			auto zone = scene.CreateEntity("HandZone");
			zone.GetTransform().Translation = BoneWorld(scene, rig, "LeftHand");
			zone.AddComponent<RigidBodyComponent>().IsTrigger = true;
			zone.AddComponent<BoxColliderComponent>().HalfExtents = { 0.1f, 0.1f, 0.1f };
			REQUIRE(physics.ActivateRagdoll(rig));
			Simulate(scene, 3);
			REQUIRE(listener.Begin == 1);
			bool destroyed = false;
			listener.Callback = [&](ContactEventType type) {
				if (type == ContactEventType::TriggerExit && !destroyed)
				{
					destroyed = true;
					scene.DestroyEntity(rig);
				}
			};
			if (recover)
				physics.RecoverRagdoll(rig, "", 0.2f);
			else
				rig.RemoveComponent<RagdollComponent>();
			Simulate(scene, 3);
			CHECK(destroyed);
			CHECK_FALSE(rig);
			listener.Callback = nullptr;
			scene.SetEventListener(nullptr);
			scene.OnSimulationStop();
		}
	}

	TEST_CASE("A joint that cannot be decomposed leaves no rig half driven")
	{
		ClearAssetCache clear;
		TempDirectory directory("RagdollSheared");
		// A third joint, turned 45 degrees, below a Tip whose clip scales it unevenly: only that joint shears.
		auto model = ReadJson(FixturePath("TwoJoint.gltf"));
		const size_t end = model["nodes"].size();
		model["nodes"].push_back({ { "name", "End" }, { "translation", { 0, 1, 0 } }, { "rotation", { 0, 0, 0.3826834f, 0.9238795f } } });
		model["nodes"][0]["children"] = json::array({ end });
		model["skins"][0]["joints"].push_back(end);
		model["skins"][0].erase("inverseBindMatrices");
		const float scale[9] = { 1, 1, 1, 2, 1, 1, 2, 1, 1 };
		AddVectorClip(model, "Stretch", 0, "scale", scale);
		const auto path = directory.GetPath() / "Sheared.gltf";
		REQUIRE(FileSystem::WriteTextFile(path, model.dump()));

		Scene scene;
		scene.GetPhysicsSettings().Gravity = glm::vec3(0);
		Entity rig = Rig(scene);
		auto& animator = rig.GetComponent<AnimatorComponent>();
		animator.Mesh = path.string();
		animator.Clip = "Stretch";
		animator.PlayOnStart = true;
		animator.Speed = 0;
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		auto& animation = *scene.GetAnimationSystem();
		animation.Update(0);
		REQUIRE(physics.ActivateRagdoll(rig));
		Simulate(scene, 3);
		// The first powered frame already finds the sheared pose: Tip's motor must not stay on with End's unreadable.
		REQUIRE(animation.Seek(rig, 1));
		Simulate(scene, 1);
		const auto since = Log::GetHistory().GetTotalCount();
		REQUIRE(physics.SetRagdollMotorStrength(rig, 1, 0));
		Simulate(scene, 5);
		CHECK(CountLogMessages(since, "the animated joint 'End' cannot be decomposed; motors are off") == 1);
		CHECK_FALSE(physics.GetRagdollPose(rig)->Powered);
		scene.OnSimulationStop();

		// A rig that follows its root needs only the root: a limb that shears stops no more than the motors.
		const auto partialRig = [](Scene& into, const std::filesystem::path& mesh, const char* clip) {
			Entity entity = Rig(into);
			auto& ragdoll = entity.GetComponent<RagdollComponent>();
			ragdoll.KinematicRoot = true;
			ragdoll.RootJoint = "Tip";
			auto& driven = entity.GetComponent<AnimatorComponent>();
			driven.Mesh = mesh.string();
			driven.Clip = clip;
			driven.PlayOnStart = true;
			driven.Speed = 0;
			return entity;
		};
		{
			Scene partial;
			partial.GetPhysicsSettings().Gravity = glm::vec3(0);
			Entity kinematic = partialRig(partial, path, "Stretch");
			partial.OnSimulationStart();
			partial.GetAnimationSystem()->Update(0);
			REQUIRE(partial.GetPhysicsWorld()->ActivateRagdoll(kinematic));
			Simulate(partial, 3);
			REQUIRE(partial.GetAnimationSystem()->Seek(kinematic, 1));
			Simulate(partial, 2);
			const glm::vec3 before = partial.GetPhysicsWorld()->GetRagdollCapsules(kinematic)[0].Start;
			kinematic.GetTransform().Translation.x += 0.1f;
			Simulate(partial, 2);
			CHECK(partial.GetPhysicsWorld()->GetRagdollCapsules(kinematic)[0].Start.x == doctest::Approx(before.x + 0.1f).scale(0).epsilon(1e-3));
			partial.OnSimulationStop();
		}

		// A root that shears itself holds still and says so; when it can follow again it catches up, however far that is.
		model["nodes"][0]["rotation"] = { 0, 0, 0.3826834f, 0.9238795f };
		AddVectorClip(model, "StretchParent", 2, "scale", scale);
		const auto parentPath = directory.GetPath() / "ShearedRoot.gltf";
		REQUIRE(FileSystem::WriteTextFile(parentPath, model.dump()));
		Scene partial;
		partial.GetPhysicsSettings().Gravity = glm::vec3(0);
		Entity kinematic = partialRig(partial, parentPath, "StretchParent");
		partial.OnSimulationStart();
		partial.GetAnimationSystem()->Update(0);
		REQUIRE(partial.GetPhysicsWorld()->ActivateRagdoll(kinematic));
		Simulate(partial, 3);
		const auto later = Log::GetHistory().GetTotalCount();
		const glm::vec3 held = partial.GetPhysicsWorld()->GetRagdollCapsules(kinematic)[0].Start;
		REQUIRE(partial.GetAnimationSystem()->Seek(kinematic, 1));
		kinematic.GetTransform().Translation.x += 5;
		Simulate(partial, 5);
		CHECK(CountLogMessages(later, "the animated root 'Tip' cannot be decomposed; the root holds still") == 1);
		CHECK(Near(partial.GetPhysicsWorld()->GetRagdollCapsules(kinematic)[0].Start, held, 1e-4f));
		REQUIRE(partial.GetAnimationSystem()->Seek(kinematic, 0));
		Simulate(partial, 2);
		CHECK(partial.GetPhysicsWorld()->GetRagdollCapsules(kinematic)[0].Start.x == doctest::Approx(held.x + 5).scale(0).epsilon(1e-3));
		partial.OnSimulationStop();
	}

	TEST_CASE("A KinematicRoot with nothing below it is rejected and said so")
	{
		ClearAssetCache clear;
		Scene scene;
		Entity rig = Rig(scene);
		auto& ragdoll = rig.GetComponent<RagdollComponent>();
		ragdoll.KinematicRoot = true;
		ragdoll.RootJoint = "Tip";
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		const auto since = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(physics.ActivateRagdoll(rig));
		CHECK(CountLogMessages(since, "KinematicRoot leaves nothing to simulate") == 1);
		CHECK_FALSE(physics.IsRagdollActive(rig));
		scene.OnSimulationStop();
	}

	TEST_CASE("Recovering a partial rig never moves its entity, however far the root lags")
	{
		ClearAssetCache clear;
		Scene scene;
		Floor(scene);
		Entity rig = PartialBiped(scene, false);
		rig.GetTransform().Rotation = glm::radians(glm::vec3(0, 33, 0));
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		scene.GetAnimationSystem()->Update(0);
		REQUIRE(physics.ActivateRagdoll(rig));
		Simulate(scene, 10);
		// Moved since the last step, so the root body is behind the entity.
		rig.GetTransform().Translation.x += 0.3f;
		const TransformComponent before = rig.GetTransform();
		REQUIRE(physics.RecoverRagdoll(rig, "Lean", 0.2f));
		CHECK(rig.GetTransform().Translation == before.Translation);
		CHECK(rig.GetTransform().Rotation == before.Rotation);
		scene.OnSimulationStop();
	}

	TEST_CASE("A partial rig blends back to the pose IK made of its joints")
	{
		ClearAssetCache clear;
		Scene scene;
		scene.GetPhysicsSettings().Gravity = glm::vec3(0, -3, 0);
		Entity rig = PartialBiped(scene, false);
		auto target = scene.CreateEntity("Target");
		target.GetTransform().Translation = { 3, 1.4f, 0 };
		auto& ik = rig.AddComponent<InverseKinematicsComponent>();
		ik.Target = target.GetUUID();
		ik.Chains.push_back({ .Name = "Look", .Type = IKChainType::LookAt, .Joint = "Head", .FollowTarget = true });
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		scene.GetAnimationSystem()->Update(0);
		Simulate(scene, 3);
		const auto aim = [&] { return glm::normalize(glm::vec3(BoneWorldTransform(scene, rig, "Head")[2])); };
		const glm::vec3 looking = aim();
		REQUIRE(glm::length(looking - glm::vec3(0, 0, 1)) > 0.5f);
		REQUIRE(physics.ActivateRagdoll(rig));
		Simulate(scene, 30);
		REQUIRE(physics.RecoverRagdoll(rig, "", 0.2f));
		Simulate(scene, 10);
		// The last of the blend is already close to the IK's pose: the end of the blend does not pop.
		CHECK(glm::dot(aim(), looking) > 0.9f);
		Simulate(scene, 10);
		CHECK(Near(aim(), looking, 0.02f));
		scene.OnSimulationStop();
	}

	TEST_CASE("A rig that starts enabled keeps its character, and recovery keeps what it was built as")
	{
		ClearAssetCache clear;
		Scene scene;
		Floor(scene);
		Entity rig = PartialBiped(scene);
		auto& ragdoll = rig.GetComponent<RagdollComponent>();
		ragdoll.Enabled = true;
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		CHECK(physics.IsRagdollActive(rig));
		CHECK(physics.HasCharacter(rig));
		Simulate(scene, 5);
		REQUIRE(physics.RecoverRagdoll(rig, "", 0.25f));
		// An edit while it blends is for the next rig: this one goes on leaving the entity alone.
		ragdoll.KinematicRoot = false;
		Simulate(scene, 2);
		CHECK_FALSE(physics.RagdollOwnsRoot(rig));
		CHECK(physics.HasCharacter(rig));
		scene.OnSimulationStop();
	}

	TEST_CASE("Motor strength fades while the rig is inactive and powers a selected subtree of a biped")
	{
		ClearAssetCache clear;
		Scene scene;
		Floor(scene);
		Entity rig = scene.CreateEntity("Biped");
		auto& animator = rig.AddComponent<AnimatorComponent>();
		animator.Mesh = FixturePath("BipedArmature.gltf").string();
		animator.Clip = "Wave";
		animator.PlayOnStart = false;
		rig.GetTransform().Translation = { 0, 0.1f, 0 };
		auto& ragdoll = rig.AddComponent<RagdollComponent>();
		// The chest and everything above it: the arms branch off the subtree's root. The arm joints swing far enough for the
		// wave.
		ragdoll.RootJoint = "Chest";
		for (const char* joint : { "RightUpperArm", "RightForeArm" })
			ragdoll.Bones.push_back({ .Joint = joint, .SwingAngle = 120, .TwistMin = -90, .TwistMax = 90 });
		scene.GetPhysicsSettings().Gravity = glm::vec3(0);
		scene.OnSimulationStart();
		auto& physics = *scene.GetPhysicsWorld();
		// A fade started before activation has moved on by then.
		REQUIRE(physics.SetRagdollMotorStrength(rig, 1, 0.5f));
		Simulate(scene, 15);
		CHECK(physics.GetRagdollMotorStrength(rig).value() == doctest::Approx(0.5f).epsilon(0.02));
		// Activated on the rest pose; the wave then raises the right arm and bends its elbow, which the motors follow.
		REQUIRE(physics.ActivateRagdoll(rig));
		REQUIRE(scene.GetAnimationSystem()->Seek(rig, 0.5f));
		Simulate(scene, 240);
		CHECK(physics.GetRagdollMotorStrength(rig) == 1.0f);
		const auto pose = physics.GetRagdollPose(rig);
		REQUIRE(pose);
		CHECK(pose->Powered);
		CHECK(ragdoll.Enabled);
		const auto source = AssetManager::GetMesh(animator.Mesh);
		std::vector<glm::mat4> animated;
		REQUIRE(scene.GetAnimationSystem()->GetAnimatedPose(rig, *source, animated));
		// The armature's scale is in every joint's matrix: the rotations come from the decomposition the motors use.
		const auto Rotation = [](const glm::mat4& matrix) {
			glm::vec3 position, scale;
			glm::quat rotation;
			REQUIRE(DecomposeRagdollTransform(matrix, position, rotation, scale));
			return rotation;
		};
		const auto physical = [&](const char* joint) {
			const uint32_t node = Node(*source, joint);
			const auto found = std::ranges::find(pose->Joints, node, &std::pair<uint32_t, glm::mat4>::first);
			REQUIRE(found != pose->Joints.end());
			return Rotation(found->second);
		};
		const auto relative = [](const glm::quat& parent, const glm::quat& child) { return glm::normalize(glm::conjugate(parent) * child); };
		const auto rotationOf = [&](const char* joint) { return Rotation(animated[Node(*source, joint)]); };
		// The elbow's bend relative to the upper arm, and the shoulder's relative to the chest (the subtree root), match
		// the animation within a few degrees.
		for (const auto& [parent, child] : { std::pair{ "RightUpperArm", "RightForeArm" }, std::pair{ "Chest", "RightUpperArm" } })
		{
			CAPTURE(child);
			const glm::quat error = glm::conjugate(relative(rotationOf(parent), rotationOf(child))) * relative(physical(parent), physical(child));
			CHECK(glm::degrees(2.0f * std::acos(std::min(1.0f, std::abs(error.w)))) < 3.0f);
		}
		scene.OnSimulationStop();
	}
}
