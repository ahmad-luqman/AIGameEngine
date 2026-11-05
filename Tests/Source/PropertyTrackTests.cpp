#include "AutomationTestUtils.h"
#include "TestUtils.h"

#include <Basalt/Animation/AnimatedModel.h>
#include <Basalt/Animation/PropertyAnimation.h>
#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Asset/ImageUtils.h>
#include <Basalt/Asset/Thumbnails.h>
#include <Basalt/Automation/AutomationSession.h>
#include <Basalt/Automation/CommandRegistry.h>
#include <Basalt/Automation/EditorSelection.h>
#include <Basalt/Core/FileSystem.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Math/Math.h>
#include <Basalt/Physics/PhysicsWorld.h>
#include <Basalt/Physics/RagdollRules.h>
#include <Basalt/Project/Project.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Prefab.h>
#include <Basalt/Scene/SceneEventListener.h>
#include <Basalt/Scene/SceneSerializer.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Basalt;
using namespace BasaltTest;
using json = nlohmann::json;

namespace {

	struct ContactLog final : SceneEventListener
	{
		int Begin = 0;
		int End = 0;
		void OnContact(Scene&, ContactEventType type, Entity, Entity, const std::optional<ContactInfo>&) override
		{
			(type == ContactEventType::CollisionBegin || type == ContactEventType::TriggerEnter ? Begin : End)++;
		}
		void OnAnimationEvent(Scene&, Entity, const std::string&, const std::string&, const std::string&) override {}
		void OnScriptError(Scene&, const std::string&) override {}
	};

	// A dynamic crate that has come to rest on a static floor, tilted by `slope` degrees.
	struct RestingCrate
	{
		Scene World;
		Entity Floor;
		Entity Crate;
		ContactLog Log;

		explicit RestingCrate(float slope = 0.0f)
		{
			World.SetEventListener(&Log);
			Floor = World.CreateEntity("Floor");
			Floor.GetTransform().Rotation = Math::QuatFromEuler(glm::radians(glm::vec3(0, 0, slope)));
			Floor.AddComponent<RigidBodyComponent>().Friction = 1.0f;
			Floor.AddComponent<BoxColliderComponent>().HalfExtents = { 20, 0.5f, 20 };
			Crate = World.CreateEntity("Crate");
			Crate.GetTransform().Translation = glm::vec3(Floor.GetTransform().Rotation * glm::vec3(0, 1.01f, 0));
			Crate.GetTransform().Rotation = Floor.GetTransform().Rotation;
			auto& body = Crate.AddComponent<RigidBodyComponent>();
			body.Type = RigidBodyType::Dynamic;
			body.Friction = 1.0f;
		}
		~RestingCrate()
		{
			if (World.GetPhysicsWorld())
				World.OnRuntimeStop();
		}
		PhysicsWorld& Physics() const { return *World.GetPhysicsWorld(); }
		void Frames(int count)
		{
			for (int i = 0; i < count; i++)
				World.OnUpdate(Timestep(Step));
		}
		// Runs until the crate sleeps.
		void Start()
		{
			if (!Crate.HasComponent<BoxColliderComponent>() && !Crate.HasComponent<SphereColliderComponent>() && !Crate.HasComponent<CapsuleColliderComponent>())
				Crate.AddComponent<BoxColliderComponent>();
			World.OnRuntimeStart();
			for (int i = 0; i < 600 && !Physics().IsBodySleeping(Crate); i++)
				Frames(1);
			REQUIRE(Physics().IsBodySleeping(Crate));
			REQUIRE(Log.Begin >= 1);
			REQUIRE(Log.End == 0);
		}
		void Patch(Entity entity, const char* component, const json& data)
		{
			std::string error;
			REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(entity, component, data, error), error);
		}
	};

	Entity FindNamed(Scene& scene, Entity root, const std::string& name)
	{
		for (Entity entity : scene.GetAllEntitiesOrdered())
			if (entity.GetName() == name && scene.IsDescendantOf(entity, root))
				return entity;
		return {};
	}

	// A prefab whose root animates its child Lamp and its own light, an instance of it, and the Lamp then deleted from the prefab.
	struct DeletedLamp
	{
		TempProject Directory;
		AutomationSession Session;
		CommandRegistry Commands;
		uint64_t InstanceId = 0;

		explicit DeletedLamp(const char* name)
			: Directory(name)
		{
			auto scene = Session.GetScene();
			Entity root = scene->CreateEntity("Root");
			root.AddComponent<PointLightComponent>();
			Entity lamp = scene->CreateEntity("Lamp");
			scene->SetParent(lamp, root, false);
			lamp.GetTransform().Scale = glm::vec3(2);
			std::string error;
			REQUIRE(PropertyAnimation::Key(root, "Transform", "Scale", 0, error, lamp.GetUUID()));
			REQUIRE(PropertyAnimation::Key(root, "PointLight", "Intensity", 0, error));
			Run("prefab.create", { { "entity", IdOf(root) }, { "path", "Assets/Lamp.bprefab" } });
			InstanceId = Run("prefab.instantiate", { { "path", "Assets/Lamp.bprefab" } })["id"];
			Run("prefab.open", { { "path", "Assets/Lamp.bprefab" } });
			Run("entity.destroy", { { "entity", "Lamp" } });
			Run("prefab.save", json::object());
			Run("prefab.close", json::object());
		}
		json Run(const std::string& command, const json& params) { return BasaltTest::Run(Commands, Session, command, params); }
		Entity Instance() { return Session.GetScene()->GetEntityByUUID(UUID(InstanceId)); }
		json Tracks() { return ComponentRegistry::Find("PropertyAnimation")->Serialize(Instance())["Tracks"]; }
	};

	// A value of the field a little from `value`, which a resting crate stays in contact through.
	json Nudged(const json& value, const ComponentFieldType& type, const std::vector<std::string>* options)
	{
		if (value.is_boolean())
			return !value.get<bool>();
		if (value.is_string())
		{
			REQUIRE(options);
			const auto current = std::ranges::find(*options, value.get<std::string>());
			return *(current + 1 == options->end() ? options->begin() : current + 1);
		}
		const auto nudge = [](float number) { return number == 0 ? -0.01f : number * 1.02f; };
		// Restitution clamps at 0, so a scalar at 0 goes up. A vector at 0 is an Offset, which goes into the floor.
		if (type.Count == 0)
			return value.get<float>() == 0 ? 0.01f : nudge(value.get<float>());
		json result = json::array();
		for (const json& lane : value)
			result.push_back(nudge(lane.get<float>()));
		return result;
	}

}

TEST_SUITE("Property animation")
{
	TEST_CASE("Tracks on every live field of bodies and colliders leave a resting body's contacts alone")
	{
		for (const char* component : { "RigidBody", "BoxCollider", "SphereCollider", "CapsuleCollider" })
		{
			const ComponentInfo& info = *ComponentRegistry::Find(component);
			for (const std::string& field : *info.AnimatableFields)
			{
				CAPTURE(component);
				CAPTURE(field);
				RestingCrate scene;
				std::string error;
				info.Add(scene.Crate);
				if (std::string_view(component) == "RigidBody")
					scene.Crate.AddComponent<BoxColliderComponent>();
				// A collider's material only counts once it overrides the body's.
				if (std::string_view(component) != "RigidBody" && field != "OverrideMaterial")
					scene.Patch(scene.Crate, component, { { "OverrideMaterial", true } });
				const json original = info.Serialize(scene.Crate);
				const auto options = info.EnumOptions.find(field);
				const json changed = Nudged(original[field], info.FieldTypes.at(field), options == info.EnumOptions.end() ? nullptr : &options->second);
				REQUIRE_MESSAGE(PropertyAnimation::Key(scene.Crate, component, field, 0, error), error);
				scene.Patch(scene.Crate, component, { { field, changed } });
				REQUIRE_MESSAGE(PropertyAnimation::Key(scene.Crate, component, field, 1, error), error);
				scene.Patch(scene.Crate, component, { { field, original[field] } });
				scene.Patch(scene.Crate, "PropertyAnimation", { { "Duration", 0.25f }, { "Looping", false }, { "PlayOnStart", false } });
				scene.Start();
				const int began = scene.Log.Begin;
				REQUIRE(scene.World.GetPropertyAnimationSystem()->Control(scene.Crate, "Play", 0, error));
				scene.Frames(30);
				CHECK(scene.Log.End == 0);
				CHECK(scene.Log.Begin == began);
				CHECK(scene.World.GetPropertyAnimationSystem()->Status(scene.Crate)["failed"] == false);
				if (changed.is_number())
					CHECK(info.Serialize(scene.Crate)[field].get<float>() == doctest::Approx(changed.get<float>()));
				else
					CHECK(info.Serialize(scene.Crate)[field] == changed);
			}
		}
	}

	TEST_CASE("A floor's friction reaches what sleeps on it")
	{
		RestingCrate scene(20.0f);
		scene.Start();
		const glm::vec3 start = scene.Crate.GetTransform().Translation;
		scene.Patch(scene.Floor, "RigidBody", { { "Friction", 0.0f } });
		scene.Frames(60);
		CHECK_FALSE(scene.Physics().IsBodySleeping(scene.Crate));
		CHECK(glm::length(scene.Crate.GetTransform().Translation - start) > 0.5f);
		CHECK(scene.Log.End == 0);
	}

	TEST_CASE("A floor that reaches a sleeping body wakes it, past its old bounds")
	{
		RestingCrate scene;
		Entity shelf = scene.World.CreateEntity("Shelf");
		shelf.GetTransform().Translation = { 10, 0, 0 };
		shelf.AddComponent<RigidBodyComponent>();
		shelf.AddComponent<BoxColliderComponent>().HalfExtents = { 1, 0.5f, 1 };
		// The crate sleeps on the floor, nowhere near the shelf until the shelf's box grows under it.
		scene.Start();
		scene.Patch(shelf, "BoxCollider", { { "HalfExtents", { 10.5f, 0.75f, 1 } } });
		scene.Frames(30);
		CHECK(scene.Crate.GetTransform().Translation.y > 1.1f);
		CHECK_FALSE(scene.Physics().IsBodySleeping(scene.Crate));
	}

	TEST_CASE("A floor that shrinks away drops what slept on it")
	{
		RestingCrate scene;
		scene.Start();
		scene.Patch(scene.Floor, "BoxCollider", { { "Offset", { 100, 0, 0 } }, { "HalfExtents", { 1, 0.5f, 1 } } });
		scene.Frames(30);
		CHECK_FALSE(scene.Physics().IsBodySleeping(scene.Crate));
		CHECK(scene.Crate.GetTransform().Translation.y < 0.5f);
	}

	TEST_CASE("A sleeping body takes a live setting at once")
	{
		RestingCrate scene;
		scene.Start();
		scene.Patch(scene.Crate, "RigidBody", { { "GravityFactor", -1.0f } });
		scene.Frames(20);
		CHECK(scene.Crate.GetTransform().Translation.y > 1.1f);
		CHECK(scene.Log.End > 0);
	}

	TEST_CASE("Live body settings do what a rebuild would")
	{
		SUBCASE("Mass")
		{
			RestingCrate scene;
			scene.Crate.GetTransform().Translation.y = 100;
			scene.Crate.GetComponent<RigidBodyComponent>().GravityFactor = 0;
			scene.Crate.GetComponent<RigidBodyComponent>().LinearDamping = 0;
			scene.Crate.AddComponent<BoxColliderComponent>();
			scene.World.OnRuntimeStart();
			scene.Patch(scene.Crate, "RigidBody", { { "Mass", 4.0f } });
			scene.Frames(1);
			scene.Physics().AddImpulse(scene.Crate, { 8, 0, 0 });
			scene.Frames(1);
			CHECK(scene.Physics().GetLinearVelocity(scene.Crate).x == doctest::Approx(2.0f).epsilon(0.01));
		}
		SUBCASE("Damping")
		{
			RestingCrate scene;
			scene.Crate.GetTransform().Translation.y = 100;
			scene.Crate.GetComponent<RigidBodyComponent>().GravityFactor = 0;
			scene.Crate.AddComponent<BoxColliderComponent>();
			scene.World.OnRuntimeStart();
			scene.Patch(scene.Crate, "RigidBody", { { "LinearDamping", 10.0f }, { "AngularDamping", 10.0f } });
			scene.Frames(1);
			scene.Physics().SetLinearVelocity(scene.Crate, { 10, 0, 0 });
			scene.Physics().SetAngularVelocity(scene.Crate, { 0, 10, 0 });
			scene.Frames(30);
			CHECK(scene.Physics().GetLinearVelocity(scene.Crate).x < 1.0f);
			CHECK(scene.Physics().GetAngularVelocity(scene.Crate).y < 1.0f);
		}
		SUBCASE("Combine modes")
		{
			RestingCrate scene(20.0f);
			scene.Start();
			// With no friction of its own and the default geometric mean the crate slides, while Max keeps the floor's grip.
			scene.Patch(scene.Crate, "RigidBody", { { "Friction", 0.0f } });
			scene.Frames(30);
			CHECK(glm::length(scene.Physics().GetLinearVelocity(scene.Crate)) > 0.5f);
			scene.Patch(scene.Crate, "RigidBody", { { "FrictionCombine", "Max" } });
			scene.Frames(90);
			CHECK(glm::length(scene.Physics().GetLinearVelocity(scene.Crate)) < 0.05f);
		}
		SUBCASE("Sleeping")
		{
			RestingCrate scene;
			scene.Start();
			scene.Patch(scene.Crate, "RigidBody", { { "AllowSleeping", false } });
			scene.Frames(120);
			CHECK_FALSE(scene.Physics().IsBodySleeping(scene.Crate));
			scene.Patch(scene.Crate, "RigidBody", { { "AllowSleeping", true } });
			scene.Frames(120);
			CHECK(scene.Physics().IsBodySleeping(scene.Crate));
		}
		SUBCASE("Continuous collision")
		{
			RestingCrate scene;
			Entity wall = scene.World.CreateEntity("Wall");
			wall.GetTransform().Translation = { 20, 5, 0 };
			wall.AddComponent<RigidBodyComponent>();
			wall.AddComponent<BoxColliderComponent>().HalfExtents = { 0.05f, 5, 5 };
			scene.Crate.GetTransform().Translation = { 0, 5, 0 };
			auto& body = scene.Crate.GetComponent<RigidBodyComponent>();
			body.GravityFactor = 0;
			body.LinearDamping = 0;
			scene.Crate.AddComponent<SphereColliderComponent>().Radius = 0.05f;
			scene.World.OnRuntimeStart();
			scene.Patch(scene.Crate, "RigidBody", { { "Continuous", true } });
			scene.Frames(1);
			scene.Physics().SetLinearVelocity(scene.Crate, { 300, 0, 0 });
			scene.Frames(20);
			CHECK(scene.Crate.GetTransform().Translation.x < 20);
		}
		SUBCASE("A collider's own material")
		{
			RestingCrate scene(20.0f);
			auto& collider = scene.Crate.AddComponent<BoxColliderComponent>();
			collider.Friction = 0;
			scene.Start();
			scene.Patch(scene.Crate, "BoxCollider", { { "OverrideMaterial", true } });
			scene.Frames(30);
			CHECK(glm::length(scene.Physics().GetLinearVelocity(scene.Crate)) > 0.5f);
			CHECK(scene.Log.End == 0);
		}
	}

	TEST_CASE("A collider's size reaches queries in place")
	{
		RestingCrate scene;
		scene.Crate.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		scene.Crate.AddComponent<BoxColliderComponent>();
		scene.World.OnRuntimeStart();
		scene.Frames(2);
		scene.Patch(scene.Crate, "BoxCollider", { { "HalfExtents", { 0.5f, 3, 0.5f } } });
		scene.Frames(1);
		const auto hit = scene.Physics().Raycast({ 0, 10, 0 }, { 0, -1, 0 }, 20);
		REQUIRE(hit);
		CHECK(hit->Distance == doctest::Approx(10 - (1.01f + 3)).epsilon(0.01));
	}

	TEST_CASE("Writing the same values neither wakes a body nor touches its contacts")
	{
		RestingCrate scene;
		scene.Start();
		const json body = ComponentRegistry::Find("RigidBody")->Serialize(scene.Crate);
		const json collider = ComponentRegistry::Find("BoxCollider")->Serialize(scene.Crate);
		for (int i = 0; i < 30; i++)
		{
			scene.Patch(scene.Crate, "RigidBody", body);
			scene.Patch(scene.Crate, "BoxCollider", collider);
			scene.Frames(1);
			REQUIRE(scene.Physics().IsBodySleeping(scene.Crate));
		}
		CHECK(scene.Log.End == 0);
	}

	TEST_CASE("Changes only a rebuild can apply still rebuild")
	{
		SUBCASE("Another collider")
		{
			RestingCrate scene;
			scene.Start();
			scene.Crate.AddComponent<SphereColliderComponent>();
			scene.Frames(2);
			CHECK(scene.Log.End > 0);
		}
		SUBCASE("One collider of several")
		{
			RestingCrate scene;
			scene.Crate.AddComponent<BoxColliderComponent>();
			// The sphere stays on the floor, so only a rebuild, not a fall, can end the contact.
			scene.Crate.AddComponent<SphereColliderComponent>().Radius = 0.5f;
			scene.Start();
			scene.Crate.RemoveComponent<BoxColliderComponent>();
			scene.Frames(2);
			CHECK(scene.Log.End > 0);
		}
		for (const json& change : { json{ { "Type", "Kinematic" } }, json{ { "IsTrigger", true } }, json{ { "FixedRotation", true } }, json{ { "Layer", "Elsewhere" } } })
		{
			CAPTURE(change.dump());
			RestingCrate scene;
			scene.Start();
			scene.Patch(scene.Crate, "RigidBody", change);
			scene.Frames(2);
			CHECK(scene.Log.End > 0);
		}
		SUBCASE("Removing the body")
		{
			RestingCrate scene;
			scene.Start();
			const uint32_t bodies = scene.Physics().GetBodyCount();
			scene.Crate.RemoveComponent<RigidBodyComponent>();
			scene.Frames(1);
			CHECK(scene.Physics().GetBodyCount() == bodies - 1);
		}
		SUBCASE("A controller replacing the body")
		{
			RestingCrate scene;
			scene.Start();
			scene.Crate.AddComponent<CharacterControllerComponent>();
			scene.Frames(2);
			CHECK(scene.Log.End > 0);
			CHECK_FALSE(scene.Physics().IsBodySleeping(scene.Crate));
		}
	}

	TEST_CASE("A collider that moves its center of mass leaves the body's joints holding")
	{
		RestingCrate scene;
		scene.Crate.GetTransform().Translation = { 0, 5, 0 };
		auto& body = scene.Crate.GetComponent<RigidBodyComponent>();
		body.GravityFactor = 0;
		scene.Crate.AddComponent<BoxColliderComponent>();
		auto& joint = scene.Crate.AddComponent<JointComponent>();
		joint.Type = JointType::Hinge;
		joint.Axis = { 0, 1, 0 };
		joint.UseLimits = true;
		joint.LimitMin = -30;
		joint.LimitMax = 30;
		scene.World.OnRuntimeStart();
		scene.Frames(5);
		REQUIRE(scene.Physics().HasJoint(scene.Crate));
		// Turn the door to its limit, then move the collider while it holds there. A rebuilt joint would take that pose as
		// its new rest pose; a joint not told of the move would drag the body after its stale anchor.
		scene.Physics().SetAngularVelocity(scene.Crate, { 0, 5, 0 });
		scene.Frames(30);
		const float angle = *scene.Physics().GetJointPosition(scene.Crate);
		CHECK(std::abs(angle) > 20);
		for (int i = 1; i <= 30; i++)
		{
			scene.Patch(scene.Crate, "BoxCollider", { { "Offset", { 0.05f * static_cast<float>(i), 0, 0 } } });
			scene.Frames(1);
		}
		CHECK(glm::length(scene.Crate.GetTransform().Translation - glm::vec3(0, 5, 0)) < 0.02f);
		CHECK(*scene.Physics().GetJointPosition(scene.Crate) == doctest::Approx(angle).epsilon(0.1));
	}

	TEST_CASE("A character's collider resizes in place, and its layer still rebuilds it")
	{
		RestingCrate scene;
		scene.Crate.RemoveComponent<RigidBodyComponent>();
		scene.Crate.GetTransform().Translation = { 0, 1.5f, 0 };
		auto& capsule = scene.Crate.AddComponent<CapsuleColliderComponent>();
		capsule.Radius = 0.4f;
		capsule.HalfHeight = 0.6f;
		scene.Crate.AddComponent<CharacterControllerComponent>();
		Entity zone = scene.World.CreateEntity("Zone");
		zone.GetTransform().Translation = { 0, 1.5f, 0 };
		zone.AddComponent<RigidBodyComponent>().IsTrigger = true;
		zone.AddComponent<BoxColliderComponent>().HalfExtents = { 3, 3, 3 };
		scene.World.OnRuntimeStart();
		scene.Frames(30);
		REQUIRE(scene.Log.Begin >= 1);
		const int began = scene.Log.Begin;
		for (int i = 1; i <= 20; i++)
		{
			scene.Patch(scene.Crate, "CapsuleCollider", { { "HalfHeight", 0.6f + 0.01f * static_cast<float>(i) } });
			scene.Frames(1);
		}
		CHECK(scene.Log.Begin == began);
		CHECK(scene.Log.End == 0);
		// The new height stands: the capsule's bottom stays on the floor at 0.5.
		scene.Frames(30);
		CHECK(scene.Crate.GetTransform().Translation.y == doctest::Approx(0.5f + 0.8f + 0.4f).epsilon(0.03));
		// A live setting keeps it all too; a new layer ends what the inner body touched.
		scene.Patch(scene.Crate, "CharacterController", { { "StepHeight", 0.1f } });
		scene.Frames(1);
		CHECK(scene.Log.End == 0);
		scene.Patch(scene.Crate, "CharacterController", { { "Layer", "Elsewhere" } });
		scene.Frames(1);
		CHECK(scene.Log.End > 0);
	}

	TEST_CASE("In-place character edits log what a rebuild would")
	{
		RestingCrate scene;
		scene.Crate.RemoveComponent<RigidBodyComponent>();
		scene.Crate.GetTransform().Translation = { 0, 1.5f, 0 };
		auto& capsule = scene.Crate.AddComponent<CapsuleColliderComponent>();
		capsule.OverrideMaterial = true;
		scene.Crate.AddComponent<CharacterControllerComponent>();
		scene.World.OnRuntimeStart();
		scene.Frames(2);
		// Fixed: it must not come back with the next settings change, as the warnings of the old shape would.
		scene.Patch(scene.Crate, "CapsuleCollider", { { "OverrideMaterial", false } });
		scene.Frames(1);
		const uint64_t since = Log::GetHistory().GetTotalCount();
		scene.Patch(scene.Crate, "CharacterController", { { "StepHeight", 0.1f } });
		scene.Frames(1);
		CHECK(CountLogMessages(since, "OverrideMaterial") == 0);
		// A RigidBody a character ignores says so, which takes a rebuild.
		scene.Crate.AddComponent<RigidBodyComponent>();
		scene.Frames(1);
		CHECK(CountLogMessages(since, "its RigidBody is ignored") == 1);
	}

	TEST_CASE("Motor targets play on a joint without resetting its rest pose")
	{
		RestingCrate scene;
		scene.Crate.GetTransform().Translation = { 0, 5, 0 };
		auto& body = scene.Crate.GetComponent<RigidBodyComponent>();
		body.GravityFactor = 0;
		scene.Crate.AddComponent<BoxColliderComponent>();
		auto& joint = scene.Crate.AddComponent<JointComponent>();
		joint.Type = JointType::Hinge;
		joint.Axis = { 0, 1, 0 };
		joint.MotorMode = JointMotorMode::Position;
		joint.MotorMaxTorque = 1000;
		joint.MotorSpringFrequency = 5;
		std::string error;
		REQUIRE(PropertyAnimation::Key(scene.Crate, "Joint", "MotorTarget", 0, error));
		scene.Patch(scene.Crate, "Joint", { { "MotorTarget", 45.0f } });
		REQUIRE(PropertyAnimation::Key(scene.Crate, "Joint", "MotorTarget", 1, error));
		scene.Patch(scene.Crate, "Joint", { { "MotorTarget", 0.0f } });
		scene.Patch(scene.Crate, "PropertyAnimation", { { "Duration", 0.5f }, { "Looping", false } });
		scene.World.OnRuntimeStart();
		scene.Frames(120);
		CHECK(*scene.Physics().GetJointPosition(scene.Crate) == doctest::Approx(45).epsilon(0.05));
	}

	TEST_CASE("Unusable body settings warn once, as they do on a rebuild")
	{
		RestingCrate scene;
		scene.Start();
		const uint64_t since = Log::GetHistory().GetTotalCount();
		scene.Patch(scene.Crate, "RigidBody", { { "Friction", -1.0f } });
		scene.Frames(1);
		scene.Patch(scene.Crate, "RigidBody", { { "Friction", -2.0f } });
		scene.Frames(1);
		CHECK(CountLogMessages(since, "Friction must not be negative") == 1);
		scene.Patch(scene.Crate, "RigidBody", { { "Friction", 1.0f }, { "Mass", 0.0f }, { "LinearDamping", -1.0f } });
		scene.Frames(1);
		CHECK(CountLogMessages(since, "Mass must be positive") == 1);
		CHECK(CountLogMessages(since, "damping must not be negative") == 1);
		scene.Patch(scene.Crate, "BoxCollider", { { "HalfExtents", { 0, 0.5f, 0.5f } } });
		scene.Frames(1);
		CHECK(CountLogMessages(since, "BoxCollider HalfExtents must be positive") == 1);
		scene.Crate.RemoveComponent<BoxColliderComponent>();
		scene.Crate.AddComponent<SphereColliderComponent>();
		scene.Frames(1);
		scene.Patch(scene.Crate, "SphereCollider", { { "Radius", 0.0f } });
		scene.Frames(1);
		CHECK(CountLogMessages(since, "SphereCollider Radius must be positive") == 1);
		scene.Crate.RemoveComponent<SphereColliderComponent>();
		scene.Crate.AddComponent<CapsuleColliderComponent>();
		scene.Frames(1);
		scene.Patch(scene.Crate, "CapsuleCollider", { { "HalfHeight", -1.0f } });
		scene.Frames(1);
		CHECK(CountLogMessages(since, "CapsuleCollider Radius and HalfHeight must be positive") == 1);
	}

	TEST_CASE("Mass only warns where it counts")
	{
		RestingCrate scene;
		scene.Start();
		const uint64_t since = Log::GetHistory().GetTotalCount();
		scene.Patch(scene.Floor, "RigidBody", { { "Mass", 0.0f } });
		scene.Frames(1);
		CHECK(CountLogMessages(since, "Mass must be positive") == 0);
	}

	TEST_CASE("Writes that do not touch a static floor's surface leave what sleeps on it alone")
	{
		RestingCrate scene;
		scene.Start();
		scene.Patch(scene.Floor, "RigidBody", { { "Mass", 3.0f }, { "LinearDamping", 0.5f }, { "GravityFactor", 0.5f } });
		scene.Frames(2);
		CHECK(scene.Physics().IsBodySleeping(scene.Crate));
	}

	TEST_CASE("A track animates another entity")
	{
		Scene scene;
		Entity timeline = scene.CreateEntity("Timeline");
		Entity lamp = scene.CreateEntity("Lamp");
		lamp.AddComponent<PointLightComponent>().Intensity = 1;
		timeline.AddComponent<PointLightComponent>().Intensity = 7;
		std::string error;
		REQUIRE(PropertyAnimation::Key(timeline, "PointLight", "Intensity", 0, error, lamp.GetUUID()));
		lamp.GetComponent<PointLightComponent>().Intensity = 3;
		REQUIRE(PropertyAnimation::Key(timeline, "PointLight", "Intensity", 1, error, lamp.GetUUID()));
		auto& animation = timeline.GetComponent<PropertyAnimationComponent>();
		REQUIRE(animation.Tracks.size() == 1);
		CHECK(animation.Tracks[0].Target == lamp.GetUUID());

		REQUIRE(PropertyAnimation::Sample(timeline, animation, 0.5f, error));
		CHECK(lamp.GetComponent<PointLightComponent>().Intensity == doctest::Approx(2.0f));
		CHECK(timeline.GetComponent<PointLightComponent>().Intensity == 7);

		// The same field of the timeline's own entity is another track, whichever way its own ID is given.
		REQUIRE(PropertyAnimation::Key(timeline, "PointLight", "Intensity", 0, error));
		REQUIRE(PropertyAnimation::Key(timeline, "PointLight", "Intensity", 1, error, timeline.GetUUID()));
		REQUIRE(animation.Tracks.size() == 2);
		CHECK(animation.Tracks[1].Target == 0);
		CHECK(animation.Tracks[1].Keys.size() == 2);
		REQUIRE(PropertyAnimation::Key(timeline, "PointLight", "Intensity", 0.5f, error, lamp.GetUUID()));
		CHECK(animation.Tracks[0].Keys.size() == 3);

		// Two tracks on one field of one entity follow the repeated-target rule, however its ID is spelled.
		REQUIRE(ComponentRegistry::AddOrPatch(timeline, "PropertyAnimation", { { "Tracks", { { { "Component", "PointLight" }, { "Field", "Intensity" }, { "Keys", { { { "Time", 0 }, { "Value", { 1, 0, 0, 0 } } } } } }, { { "Component", "PointLight" }, { "Field", "Intensity" }, { "Target", static_cast<uint64_t>(timeline.GetUUID()) }, { "Keys", { { { "Time", 0 }, { "Value", { 2, 0, 0, 0 } } } } } } } } }, error));
		REQUIRE(PropertyAnimation::Sample(timeline, timeline.GetComponent<PropertyAnimationComponent>(), 0, error));
		CHECK(timeline.GetComponent<PointLightComponent>().Intensity == 2);
		const auto warnings = GetAnimationWarnings(timeline);
		CHECK(std::ranges::any_of(warnings, [](const std::string& warning) { return warning.find("reaches the same field") != std::string::npos; }));
	}

	TEST_CASE("A value another entity's component rejects names that entity")
	{
		Scene scene;
		Entity timeline = scene.CreateEntity("Timeline");
		Entity panel = scene.CreateEntity("Panel");
		panel.AddComponent<MaterialComponent>();
		PropertyAnimationComponent animation;
		animation.Tracks.push_back({ "Material", "Material", true, { { 0, glm::vec4(0.0f), "", "Assets/NotAMaterial.png" } }, panel.GetUUID() });
		std::string error;
		CHECK_FALSE(PropertyAnimation::Sample(timeline, animation, 0, error));
		CHECK(error.find("entity 'Panel'") != std::string::npos);
		CHECK(error.find("must be a .bmaterial") != std::string::npos);
	}

	TEST_CASE("A track's missing target fails, warns and suspends the timeline until repaired")
	{
		Scene scene;
		Entity timeline = scene.CreateEntity("Timeline");
		Entity lamp = scene.CreateEntity("Lamp");
		lamp.AddComponent<PointLightComponent>();
		timeline.GetTransform().Translation.x = 1;
		std::string error;
		REQUIRE(PropertyAnimation::Key(timeline, "PointLight", "Intensity", 0, error, lamp.GetUUID()));
		REQUIRE(PropertyAnimation::Key(timeline, "Transform", "Translation", 0, error));
		const auto& animation = timeline.GetComponent<PropertyAnimationComponent>();

		CHECK_FALSE(PropertyAnimation::Key(timeline, "PointLight", "Intensity", 0, error, UUID(424242)));
		CHECK(error.find("424242") != std::string::npos);

		SUBCASE("Without its component")
		{
			lamp.RemoveComponent<PointLightComponent>();
			CHECK_FALSE(PropertyAnimation::Sample(timeline, animation, 0, error));
			CHECK(error.find("has no PointLight") != std::string::npos);
			CHECK_FALSE(PropertyAnimation::Key(timeline, "PointLight", "Intensity", 0, error, lamp.GetUUID()));
			REQUIRE(GetAnimationWarnings(timeline).size() == 1);
			CHECK(GetAnimationWarnings(timeline)[0].find("has no PointLight") != std::string::npos);
		}
		SUBCASE("Gone during play")
		{
			scene.OnRuntimeStart();
			scene.OnUpdate(Timestep(Step));
			auto* system = scene.GetPropertyAnimationSystem();
			scene.DestroyEntity(lamp);
			scene.OnUpdate(Timestep(Step));
			scene.OnUpdate(Timestep(Step));
			const auto status = system->Status(timeline);
			CHECK(status["failed"] == true);
			CHECK(status["error"].get<std::string>().find("not in the scene") != std::string::npos);
			REQUIRE(GetAnimationWarnings(timeline).size() == 1);
			CHECK(GetAnimationWarnings(timeline)[0].find("is not in the scene") != std::string::npos);
			scene.OnRuntimeStop();
		}
	}

	TEST_CASE("Track targets resolve by name, follow copies and survive a save")
	{
		TempProject project("PropertyTargets");
		AutomationSession session;
		CommandRegistry commands;
		const auto run = [&](const std::string& command, const json& params) { return BasaltTest::Run(commands, session, command, params); };
		auto scene = session.GetScene();
		Entity root = scene->CreateEntity("Root");
		Entity lamp = scene->CreateEntity("Lamp");
		scene->SetParent(lamp, root, false);
		lamp.AddComponent<PointLightComponent>();
		Entity outside = scene->CreateEntity("Outside");
		outside.AddComponent<PointLightComponent>();

		run("animation.key", { { "entity", "Root" }, { "component", "PointLight" }, { "field", "Intensity" }, { "time", 0 }, { "target", "Lamp" } });
		CHECK(root.GetComponent<PropertyAnimationComponent>().Tracks[0].Target == lamp.GetUUID());
		CHECK(RunError(commands, session, "animation.key", { { "entity", "Root" }, { "component", "PointLight" }, { "field", "Intensity" }, { "time", 0 }, { "target", "Nobody" } }).find("Nobody") != std::string::npos);

		// Names work inside list elements too, and a stale ID in one element must not block edits of the others.
		json tracks = ComponentRegistry::Find("PropertyAnimation")->Serialize(root)["Tracks"];
		tracks.push_back(tracks[0]);
		tracks[1]["Target"] = "Outside";
		run("component.set", { { "entity", "Root" }, { "component", "PropertyAnimation" }, { "data", { { "Tracks", tracks } } } });
		CHECK(root.GetComponent<PropertyAnimationComponent>().Tracks[1].Target == outside.GetUUID());
		// An ID that is new must name an entity, while a track the entity already holds may have outlived its own.
		tracks[1]["Target"] = 987654321;
		CHECK(RunError(commands, session, "component.set", { { "entity", "Root" }, { "component", "PropertyAnimation" }, { "data", { { "Tracks", tracks } } } }).find("987654321") != std::string::npos);
		root.GetComponent<PropertyAnimationComponent>().Tracks[1].Target = UUID(987654321);
		run("component.set", { { "entity", "Root" }, { "component", "PropertyAnimation" }, { "data", { { "Duration", 2 }, { "Tracks", tracks } } } });
		CHECK(root.GetComponent<PropertyAnimationComponent>().Duration == 2);
		CHECK(root.GetComponent<PropertyAnimationComponent>().Tracks[1].Target == UUID(987654321));
		tracks[1]["Target"] = "Nobody";
		CHECK(RunError(commands, session, "component.set", { { "entity", "Root" }, { "component", "PropertyAnimation" }, { "data", { { "Tracks", tracks } } } }).find("Nobody") != std::string::npos);
		tracks[1]["Target"] = static_cast<uint64_t>(outside.GetUUID());
		run("component.set", { { "entity", "Root" }, { "component", "PropertyAnimation" }, { "data", { { "Tracks", tracks } } } });

		// A copy of the tree animates its own lamp; what lies outside it stays the same entity.
		const UUID copyId = scene->DuplicateEntity(root).GetUUID();
		Entity copy = scene->GetEntityByUUID(copyId);
		REQUIRE(copy);
		const auto& copied = copy.GetComponent<PropertyAnimationComponent>().Tracks;
		CHECK(copied[0].Target != lamp.GetUUID());
		CHECK(scene->GetEntityByUUID(copied[0].Target).GetName() == "Lamp");
		CHECK(scene->IsDescendantOf(scene->GetEntityByUUID(copied[0].Target), copy));
		CHECK(copied[1].Target == outside.GetUUID());

		// Files keep the reference.
		const UUID rootId = root.GetUUID(), lampId = lamp.GetUUID();
		run("scene.save", { { "path", "Assets/Targets.bscene" } });
		run("scene.open", { { "path", "Assets/Targets.bscene" } });
		CHECK(session.GetScene()->GetEntityByUUID(rootId).GetComponent<PropertyAnimationComponent>().Tracks[0].Target == lampId);
	}

	TEST_CASE("Prefab instances animate their own members")
	{
		TempProject project("PropertyPrefabs");
		AutomationSession session;
		CommandRegistry commands;
		const auto run = [&](const std::string& command, const json& params) { return BasaltTest::Run(commands, session, command, params); };
		auto scene = session.GetScene();
		Entity root = scene->CreateEntity("Root");
		Entity lamp = scene->CreateEntity("Lamp");
		scene->SetParent(lamp, root, false);
		lamp.AddComponent<PointLightComponent>();
		std::string error;
		REQUIRE(PropertyAnimation::Key(root, "PointLight", "Intensity", 0, error, lamp.GetUUID()));
		run("prefab.create", { { "entity", IdOf(root) }, { "path", "Assets/Lamp.bprefab" } });
		const auto instantiate = [&] {
			Entity instance = session.GetScene()->GetEntityByUUID(UUID(run("prefab.instantiate", { { "path", "Assets/Lamp.bprefab" } })["id"].get<uint64_t>()));
			REQUIRE(instance);
			Entity member = session.GetScene()->GetEntityByUUID(instance.GetComponent<PropertyAnimationComponent>().Tracks[0].Target);
			REQUIRE(member);
			CHECK(session.GetScene()->IsDescendantOf(member, instance));
			return instance;
		};
		Entity first = instantiate();
		Entity second = instantiate();
		CHECK(first.GetComponent<PropertyAnimationComponent>().Tracks[0].Target != second.GetComponent<PropertyAnimationComponent>().Tracks[0].Target);
		CHECK(run("prefab.overrides", { { "entity", IdOf(first) } })["overrides"].empty());

		// A reference out of the prefab means nothing in other scenes: applying it keeps the prefab's own tracks.
		Entity outside = session.GetScene()->CreateEntity("Outside");
		outside.AddComponent<PointLightComponent>();
		const json authored = ComponentRegistry::Find("PropertyAnimation")->Serialize(first)["Tracks"];
		const auto applyTracks = [&](json tracks) {
			run("component.set", { { "entity", IdOf(first) }, { "component", "PropertyAnimation" }, { "data", { { "Tracks", tracks } } } });
			const json applied = run("prefab.apply", { { "entity", IdOf(first) } });
			REQUIRE(applied.contains("warnings"));
			CHECK(applied["warnings"].dump().find("so Tracks stays as the prefab has it, without this instance's other changes to it") != std::string::npos);
			Entity fresh = instantiate();
			const auto& kept = fresh.GetComponent<PropertyAnimationComponent>().Tracks;
			REQUIRE(kept.size() == 1);
			CHECK(kept[0].Component == "PointLight");
		};
		const auto aimedOutside = [&](const char* component) {
			json track = authored[0];
			track["Component"] = component;
			track["Target"] = static_cast<uint64_t>(outside.GetUUID());
			return track;
		};
		SUBCASE("A track retargeted")
		{
			applyTracks(json::array({ aimedOutside("PointLight") }));
		}
		// Neither may become a track of the holder (0), nor take the target of whatever had its place in the list.
		SUBCASE("A new track at the end")
		{
			applyTracks(json::array({ authored[0], aimedOutside("SpotLight") }));
		}
		SUBCASE("A new track in front")
		{
			applyTracks(json::array({ aimedOutside("SpotLight"), authored[0] }));
		}
	}

	TEST_CASE("A prefab member deleted under a track fails loudly, and a repair of the prefab reaches its instances")
	{
		TempProject project("PropertyPrefabDeleted");
		AutomationSession session;
		CommandRegistry commands;
		const auto run = [&](const std::string& command, const json& params) { return BasaltTest::Run(commands, session, command, params); };
		auto scene = session.GetScene();
		Entity root = scene->CreateEntity("Root");
		Entity lamp = scene->CreateEntity("Lamp");
		scene->SetParent(lamp, root, false);
		lamp.GetTransform().Scale = glm::vec3(2);
		std::string error;
		REQUIRE(PropertyAnimation::Key(root, "Transform", "Scale", 0, error, lamp.GetUUID()));
		run("prefab.create", { { "entity", IdOf(root) }, { "path", "Assets/Lamp.bprefab" } });
		const uint64_t instanceId = run("prefab.instantiate", { { "path", "Assets/Lamp.bprefab" } })["id"];

		// Delete the lamp from the prefab itself: the instance merges a source without its target.
		run("prefab.open", { { "path", "Assets/Lamp.bprefab" } });
		run("entity.destroy", { { "entity", "Lamp" } });
		const json saved = run("prefab.save", json::object());
		CHECK(saved.dump().find("references an entity that is not in the prefab") != std::string::npos);
		run("prefab.close", json::object());

		Entity instance = session.GetScene()->GetEntityByUUID(UUID(instanceId));
		REQUIRE(instance);
		const auto track = [&] { return instance.GetComponent<PropertyAnimationComponent>().Tracks[0]; };
		// Not 0, which would turn the track on its holder, and just what a new instance of the file holds.
		CHECK(track().Target != 0);
		const uint64_t fresh = run("prefab.instantiate", { { "path", "Assets/Lamp.bprefab" } })["id"];
		CHECK(session.GetScene()->GetEntityByUUID(UUID(fresh)).GetComponent<PropertyAnimationComponent>().Tracks[0].Target == track().Target);
		const glm::vec3 scale = instance.GetTransform().Scale;
		CHECK_FALSE(PropertyAnimation::Sample(instance, instance.GetComponent<PropertyAnimationComponent>(), 0, error));
		CHECK(error.find("not in the scene") != std::string::npos);
		CHECK(instance.GetTransform().Scale == scale);
		REQUIRE_FALSE(GetAnimationWarnings(instance).empty());
		// Nobody edited this instance, so nothing stands between it and a repair of the prefab.
		CHECK(run("prefab.overrides", { { "entity", instanceId } })["overrides"].empty());

		run("prefab.open", { { "path", "Assets/Lamp.bprefab" } });
		Entity opened = session.GetScene()->FindEntityByName("Root");
		Entity bulb = session.GetScene()->CreateEntity("Bulb");
		session.GetScene()->SetParent(bulb, opened, false);
		json tracks = ComponentRegistry::Find("PropertyAnimation")->Serialize(opened)["Tracks"];
		tracks[0]["Target"] = static_cast<uint64_t>(bulb.GetUUID());
		run("component.set", { { "entity", "Root" }, { "component", "PropertyAnimation" }, { "data", { { "Tracks", tracks } } } });
		run("prefab.save", json::object());
		run("prefab.close", json::object());

		instance = session.GetScene()->GetEntityByUUID(UUID(instanceId));
		REQUIRE(instance);
		Entity repaired = session.GetScene()->GetEntityByUUID(track().Target);
		REQUIRE(repaired);
		CHECK(session.GetScene()->IsDescendantOf(repaired, instance));
		CHECK(PropertyAnimation::Sample(instance, instance.GetComponent<PropertyAnimationComponent>(), 0, error));
	}

	TEST_CASE("prefab.apply says when what it writes leaves a track without its entity")
	{
		TempProject project("PropertyPrefabApplyDeleted");
		AutomationSession session;
		CommandRegistry commands;
		const auto run = [&](const std::string& command, const json& params) { return BasaltTest::Run(commands, session, command, params); };
		auto scene = session.GetScene();
		Entity root = scene->CreateEntity("Root");
		Entity lamp = scene->CreateEntity("Lamp");
		scene->SetParent(lamp, root, false);
		std::string error;
		REQUIRE(PropertyAnimation::Key(root, "Transform", "Scale", 0, error, lamp.GetUUID()));
		run("prefab.create", { { "entity", IdOf(root) }, { "path", "Assets/Lamp.bprefab" } });
		const uint64_t instanceId = run("prefab.instantiate", { { "path", "Assets/Lamp.bprefab" } })["id"];
		Entity instance = session.GetScene()->GetEntityByUUID(UUID(instanceId));
		session.GetScene()->DestroyEntity(session.GetScene()->GetEntityByUUID(instance.GetComponent<PropertyAnimationComponent>().Tracks[0].Target));
		const json applied = run("prefab.apply", { { "entity", instanceId } });
		CHECK(applied.dump().find("PropertyAnimation.Tracks[0].Target references an entity that is not in the prefab") != std::string::npos);
	}

	TEST_CASE("prefab.apply leaves out what a component new to the prefab cannot take")
	{
		TempProject project("PropertyPrefabApplyNew");
		AutomationSession session;
		CommandRegistry commands;
		const auto run = [&](const std::string& command, const json& params) { return BasaltTest::Run(commands, session, command, params); };
		auto scene = session.GetScene();
		Entity root = scene->CreateEntity("Root");
		Entity lamp = scene->CreateEntity("Lamp");
		scene->SetParent(lamp, root, false);
		lamp.AddComponent<PointLightComponent>();
		run("prefab.create", { { "entity", IdOf(root) }, { "path", "Assets/Lamp.bprefab" } });
		const uint64_t instanceId = run("prefab.instantiate", { { "path", "Assets/Lamp.bprefab" } })["id"];
		Entity instance = session.GetScene()->GetEntityByUUID(UUID(instanceId));
		Entity outside = session.GetScene()->CreateEntity("Outside");
		outside.AddComponent<PointLightComponent>();
		Entity member;
		for (Entity candidate : session.GetScene()->GetAllEntitiesOrdered())
			if (candidate.GetName() == "Lamp" && session.GetScene()->IsDescendantOf(candidate, instance))
				member = candidate;
		REQUIRE(member);
		std::string error;
		REQUIRE(PropertyAnimation::Key(instance, "PointLight", "Intensity", 0, error, member.GetUUID()));
		REQUIRE(PropertyAnimation::Key(instance, "PointLight", "Intensity", 0, error, outside.GetUUID()));
		const json applied = run("prefab.apply", { { "entity", instanceId } });
		CHECK(applied["warnings"].dump().find("so 1 of 2 Tracks stayed out of it") != std::string::npos);
		const uint64_t fresh = run("prefab.instantiate", { { "path", "Assets/Lamp.bprefab" } })["id"];
		CHECK(session.GetScene()->GetEntityByUUID(UUID(fresh)).GetComponent<PropertyAnimationComponent>().Tracks.size() == 1);
	}

	TEST_CASE("prefab.create says what lies outside the prefab")
	{
		TempProject project("PropertyPrefabOutside");
		AutomationSession session;
		CommandRegistry commands;
		auto scene = session.GetScene();
		Entity root = scene->CreateEntity("Root");
		Entity outside = scene->CreateEntity("Outside");
		outside.AddComponent<PointLightComponent>();
		std::string error;
		REQUIRE(PropertyAnimation::Key(root, "PointLight", "Intensity", 0, error, outside.GetUUID()));
		const json created = BasaltTest::Run(commands, session, "prefab.create", { { "entity", IdOf(root) }, { "path", "Assets/Root.bprefab" } });
		REQUIRE(created.contains("warnings"));
		CHECK(created["warnings"].dump().find("'Root' PropertyAnimation.Tracks[0].Target references an entity that is not in the prefab") != std::string::npos);

		// Applying what the file already says is no news.
		const json applied = BasaltTest::Run(commands, session, "prefab.apply", { { "entity", IdOf(root) } });
		CHECK(applied.dump().find("stays as the prefab has it") == std::string::npos);

		// Saving from an open prefab says the same.
		BasaltTest::Run(commands, session, "prefab.open", { { "path", "Assets/Root.bprefab" } });
		const json saved = BasaltTest::Run(commands, session, "prefab.save", json::object());
		CHECK(saved.dump().find("references an entity that is not in the prefab") != std::string::npos);
		BasaltTest::Run(commands, session, "prefab.close", json::object());
	}

	TEST_CASE("project.validate reports a keyed track whose entity is not there")
	{
		TempProject project("PropertyValidate");
		AutomationSession session;
		CommandRegistry commands;
		const auto run = [&](const std::string& command, const json& params) { return BasaltTest::Run(commands, session, command, params); };
		Entity timeline = session.GetScene()->CreateEntity("Timeline");
		run("scene.save", { { "path", "Assets/Tracks.bscene" } });
		const json before = run("project.validate", json::object());
		const auto setTrack = [&](const json& keys) {
			std::string error;
			REQUIRE(ComponentRegistry::AddOrPatch(timeline, "PropertyAnimation", { { "Tracks", { { { "Component", "PointLight" }, { "Field", "Intensity" }, { "Target", 424242 }, { "Keys", keys } } } } }, error));
			run("scene.save", json::object());
		};
		// An empty track does nothing, so it cannot fail: nothing to report, as in scene.info.
		setTrack(json::array());
		CHECK(run("project.validate", json::object())["errors"] == before["errors"]);
		setTrack({ { { "Time", 0 } } });
		const json report = run("project.validate", json::object());
		CHECK(report["errors"].get<int>() == before["errors"].get<int>() + 1);
		CHECK(report.dump().find("PropertyAnimation.Tracks[0].Target names entity 424242") != std::string::npos);

		// In a prefab file it is a warning: prefab.create said so, and only a scene shows what an instance really finds.
		std::string error;
		run("scene.new", json::object());
		Entity root = session.GetScene()->CreateEntity("Root");
		Entity outside = session.GetScene()->CreateEntity("Outside");
		outside.AddComponent<PointLightComponent>();
		REQUIRE(PropertyAnimation::Key(root, "PointLight", "Intensity", 0, error, outside.GetUUID()));
		run("prefab.create", { { "entity", IdOf(root) }, { "path", "Assets/Root.bprefab" } });
		const json withPrefab = run("project.validate", json::object());
		CHECK(withPrefab["errors"].get<int>() == before["errors"].get<int>() + 1);
		CHECK(withPrefab["warnings"].get<int>() > report["warnings"].get<int>());
	}

	TEST_CASE("assert.component accepts what component.get returned, stale entities included")
	{
		TempProject project("PropertyAssert");
		AutomationSession session;
		CommandRegistry commands;
		Entity timeline = session.GetScene()->CreateEntity("Timeline");
		std::string error;
		REQUIRE(ComponentRegistry::AddOrPatch(timeline, "PropertyAnimation", { { "Tracks", { { { "Component", "PointLight" }, { "Field", "Intensity" }, { "Target", 424242 } } } } }, error));
		const json tracks = BasaltTest::Run(commands, session, "component.get", { { "entity", "Timeline" }, { "component", "PropertyAnimation" } })["Tracks"];
		BasaltTest::Run(commands, session, "assert.component", { { "entity", "Timeline" }, { "component", "PropertyAnimation" }, { "fields", { { "Tracks", tracks } } } });
	}

	TEST_CASE("Asset tracks step through project assets and persist like any asset field")
	{
		TempProject project("PropertyFlipbook");
		AutomationSession session;
		CommandRegistry commands;
		const auto run = [&](const std::string& command, const json& params) { return BasaltTest::Run(commands, session, command, params); };
		std::string writeError;
		for (const char* name : { "Assets/FrameA.png", "Assets/FrameB.png" })
			REQUIRE_MESSAGE(WritePng(Project::ResolvePath(name), { 255, 0, 0, 255 }, 1, 1, writeError), writeError);
		run("asset.refresh", json::object());
		Entity entity = session.GetScene()->CreateEntity("Flipbook");
		auto& material = entity.AddComponent<MaterialComponent>();
		material.AlbedoMap = "Assets/FrameA.png";
		run("animation.key", { { "entity", "Flipbook" }, { "component", "Material" }, { "field", "AlbedoMap" }, { "time", 0 } });
		entity.GetComponent<MaterialComponent>().AlbedoMap = "Assets/FrameB.png";
		run("animation.key", { { "entity", "Flipbook" }, { "component", "Material" }, { "field", "AlbedoMap" }, { "time", 0.5 } });
		entity.GetComponent<MaterialComponent>().AlbedoMap = "";
		run("animation.key", { { "entity", "Flipbook" }, { "component", "Material" }, { "field", "AlbedoMap" }, { "time", 1 } });
		const auto& animation = entity.GetComponent<PropertyAnimationComponent>();
		REQUIRE(animation.Tracks.size() == 1);
		CHECK(animation.Tracks[0].Stepped);
		CHECK(animation.Tracks[0].Keys[1].Asset == "Assets/FrameB.png");
		CHECK(animation.Tracks[0].Keys[1].Text.empty());

		const auto albedoAt = [&](float time) {
			run("animation.sample", { { "entity", "Flipbook" }, { "time", time } });
			return entity.GetComponent<MaterialComponent>().AlbedoMap;
		};
		CHECK(albedoAt(0.25f) == "Assets/FrameA.png");
		CHECK(albedoAt(0.5f) == "Assets/FrameB.png");
		CHECK(albedoAt(0.75f) == "Assets/FrameB.png");
		CHECK(albedoAt(1.0f).empty());

		// Disk references carry UUIDs, two lists deep.
		run("scene.save", { { "path", "Assets/Flipbook.bscene" } });
		const auto saved = FileSystem::ReadTextFile(Project::ResolvePath("Assets/Flipbook.bscene"));
		REQUIRE(saved);
		const json document = json::parse(*saved);
		const json key = document["Entities"][0]["Components"]["PropertyAnimation"]["Tracks"][0]["Keys"][1]["Asset"];
		REQUIRE(key.is_object());
		CHECK(key["Path"] == "Assets/FrameB.png");
		CHECK(key.contains("UUID"));

		// A move follows them, in the open scene and through the saved one.
		const UUID id = entity.GetUUID();
		run("asset.move", { { "from", "Assets/FrameB.png" }, { "to", "Assets/Frames/B.png" } });
		CHECK(session.GetScene()->GetEntityByUUID(id).GetComponent<PropertyAnimationComponent>().Tracks[0].Keys[1].Asset == "Assets/Frames/B.png");
		run("scene.save", json::object());
		run("scene.open", { { "path", "Assets/Flipbook.bscene" } });
		CHECK(session.GetScene()->GetEntityByUUID(id).GetComponent<PropertyAnimationComponent>().Tracks[0].Keys[1].Asset == "Assets/Frames/B.png");

		// The project check sees keys naming files that are gone.
		REQUIRE(std::filesystem::remove(Project::ResolvePath("Assets/FrameA.png")));
		run("asset.refresh", json::object());
		run("scene.save", json::object());
		CHECK(run("project.validate", json::object()).dump().find("Assets/FrameA.png") != std::string::npos);
	}

	TEST_CASE("Asset keys are checked against their field")
	{
		Scene scene;
		Entity entity = scene.CreateEntity();
		entity.AddComponent<MaterialComponent>();
		std::string error;
		const auto keyed = [&](const char* field, const json& key) {
			return ComponentRegistry::AddOrPatch(entity, "PropertyAnimation", { { "Tracks", { { { "Component", "Material" }, { "Field", field }, { "Keys", { key } } } } } }, error);
		};
		REQUIRE(PropertyAnimation::ResolveField("Material", "AlbedoMap")->IsAsset);
		CHECK_FALSE(PropertyAnimation::ResolveField("Material", "AlbedoColor")->IsAsset);
		CHECK(PropertyAnimation::ResolveField("Material", "AlbedoMap")->IsDiscrete());
		CHECK(keyed("AlbedoMap", { { "Time", 0 }, { "Asset", "Assets/Frame.png" } }));
		CHECK_FALSE(keyed("AlbedoMap", { { "Time", 0 }, { "Text", "Assets/Frame.png" } }));
		CHECK(error.find("Text must be empty") != std::string::npos);
		CHECK(error.find("keys use Asset") != std::string::npos);
		CHECK_FALSE(keyed("Metallic", { { "Time", 0 }, { "Asset", "Assets/Frame.png" } }));
		CHECK(error.find("Asset must be empty") != std::string::npos);
		CHECK_FALSE(keyed("AlbedoMap", { { "Time", 0 }, { "Asset", std::string(PropertyAnimationKey::MaxTextLength + 1, 'a') } }));
		CHECK(error.find("Asset holds at most") != std::string::npos);
		// A key without a path clears the field, as an empty AlbedoMap does.
		CHECK(keyed("AlbedoMap", { { "Time", 0 } }));
		entity.GetComponent<MaterialComponent>().AlbedoMap = "Assets/Frame.png";
		REQUIRE(PropertyAnimation::Sample(entity, entity.GetComponent<PropertyAnimationComponent>(), 0, error));
		CHECK(entity.GetComponent<MaterialComponent>().AlbedoMap.empty());
	}

	TEST_CASE("Starting a timeline loads the textures its asset tracks name")
	{
		TempProject project("PropertyPreload");
		std::string writeError;
		const std::string key = "Assets/Preloaded.png";
		REQUIRE_MESSAGE(WritePng(Project::ResolvePath(key), { 0, 255, 0, 255 }, 1, 1, writeError), writeError);
		Scene scene;
		Entity entity = scene.CreateEntity();
		entity.AddComponent<MaterialComponent>().AlbedoMap = key;
		std::string error;
		REQUIRE(PropertyAnimation::Key(entity, "Material", "AlbedoMap", 0, error));
		entity.GetComponent<PropertyAnimationComponent>().PlayOnStart = false;
		entity.GetComponent<MaterialComponent>().AlbedoMap.clear();
		CHECK_FALSE(AssetManager::IsLoading(key));
		scene.OnRuntimeStart();
		scene.OnUpdate(Timestep(Step));
		// Requested without any component reading it yet: joining finds the load the timeline started.
		CHECK(AssetManager::IsLoading(key));
		AssetManager::WaitForLoads();
		CHECK(AssetManager::RequestTexture(key));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Starting a timeline requests models, materials and embedded images too")
	{
		TempProject project("PropertyPreloadKinds");
		Scene scene;
		Entity entity = scene.CreateEntity();
		entity.AddComponent<MeshComponent>();
		entity.AddComponent<MaterialComponent>();
		PropertyAnimationComponent animation;
		animation.PlayOnStart = false;
		// RequestMaterial loads what holds an imported material, and nothing here exists: only the requests matter.
		animation.Tracks.push_back({ "Mesh", "Mesh", true, { { 0, glm::vec4(0.0f), "", "Assets/Model.glb" } } });
		animation.Tracks.push_back({ "Material", "AlbedoMap", true, { { 0, glm::vec4(0.0f), "", "Assets/Other.glb#image/0" } } });
		animation.Tracks.push_back({ "Material", "Material", true, { { 0, glm::vec4(0.0f), "", "Assets/Third.glb#material/0" } } });
		entity.AddComponent<PropertyAnimationComponent>(animation);
		scene.OnRuntimeStart();
		scene.OnUpdate(Timestep(Step));
		CHECK(AssetManager::IsLoading("Assets/Model.glb"));
		CHECK(AssetManager::IsLoading("Assets/Other.glb#image/0"));
		CHECK(AssetManager::IsLoading("Assets/Third.glb"));
		AssetManager::WaitForLoads();
		scene.OnRuntimeStop();
	}

	TEST_CASE("Rotation keys through gimbal lock arrive at the right orientation")
	{
		Scene scene;
		Entity entity = scene.CreateEntity();
		std::string error;
		entity.GetTransform().Rotation = Math::QuatFromEuler(glm::radians(glm::vec3(0, 60, 0)));
		REQUIRE(PropertyAnimation::Key(entity, "Transform", "Rotation", 0, error));
		entity.GetTransform().Rotation = Math::QuatFromEuler(glm::radians(glm::vec3(0, 120, 0)));
		REQUIRE(PropertyAnimation::Key(entity, "Transform", "Rotation", 1, error));
		const auto& animation = entity.GetComponent<PropertyAnimationComponent>();
		// The slerp passes through yaw 90 at the middle.
		for (const float time : { 0.5f, 0.4999f, 0.5001f, 0.49999f, 0.50001f, 0.47f })
		{
			CAPTURE(time);
			REQUIRE(PropertyAnimation::Sample(entity, animation, time, error));
			const glm::quat expected = Math::QuatFromEuler(glm::radians(glm::vec3(0, 60 + 60 * time, 0)));
			const glm::quat actual = entity.GetTransform().Rotation;
			CHECK(glm::length(glm::vec4(actual.w, actual.x, actual.y, actual.z) * (glm::dot(actual, expected) < 0 ? -1.0f : 1.0f) - glm::vec4(expected.w, expected.x, expected.y, expected.z)) < 2e-5f);
		}
		// The canonical form of a locked orientation stays what it is through patches.
		entity.GetTransform().Rotation = Math::QuatFromEuler(glm::radians(glm::vec3(30, 90, 10)));
		const json canonical = ComponentRegistry::Find("Transform")->Serialize(entity)["Rotation"];
		CHECK(canonical[1] == 90.0f);
		CHECK(canonical[2] == 0.0f);
		CHECK(canonical[0].get<float>() == doctest::Approx(20.0f).epsilon(1e-4));
		for (int i = 0; i < 3; i++)
		{
			REQUIRE(ComponentRegistry::AddOrPatch(entity, "Transform", { { "Rotation", ComponentRegistry::Find("Transform")->Serialize(entity)["Rotation"] } }, error));
			CHECK(ComponentRegistry::Find("Transform")->Serialize(entity)["Rotation"] == canonical);
		}
	}

	TEST_CASE("List element entity and asset fields are declared at any depth")
	{
		const ComponentInfo& info = *ComponentRegistry::Find("PropertyAnimation");
		CHECK(info.EntityFields == std::vector<std::string>{ "Tracks[].Target" });
		CHECK(info.AssetFields == std::vector<std::string>{ "Tracks[].Keys[].Asset" });
		json data = { { "Tracks", { { { "Keys", { { { "Asset", "a" } }, 3, { { "Asset", "b" } } } } }, 4, { { "Keys", 5 } }, { { "Keys", { { { "Other", 1 } } } } } } } };
		std::vector<std::string> labels;
		CHECK(ForEachFieldValue(data, "Tracks[].Keys[].Asset", [&](json& value, const std::string& label) { labels.push_back(label + "=" + value.get<std::string>()); }));
		CHECK(labels == std::vector<std::string>{ "Tracks[0].Keys[0].Asset=a", "Tracks[0].Keys[2].Asset=b" });
		CHECK_FALSE(ForEachFieldValue(data, "Other[].Keys[].Asset", [](json&, const std::string&) { FAIL("no such list"); }));
		json flat = { { "Field", 1 } };
		CHECK(ForEachFieldValue(flat, "Field", [](json& value, const std::string& label) { CHECK(label == "Field"); value = 2; }));
		CHECK(flat["Field"] == 2);
	}
}

TEST_SUITE("Property animation")
{
	TEST_CASE("kinematic floor's friction wakes what sleeps on it")
	{
		RestingCrate scene(20.0f);
		scene.Floor.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		scene.Start();
		const glm::vec3 start = scene.Crate.GetTransform().Translation;
		scene.Patch(scene.Floor, "RigidBody", { { "Friction", 0.0f } });
		scene.Frames(60);
		CHECK_FALSE(scene.Physics().IsBodySleeping(scene.Crate));
		CHECK(glm::length(scene.Crate.GetTransform().Translation - start) > 0.5f);
	}

	TEST_CASE("a kinematic floor that shrinks away drops what slept on it")
	{
		RestingCrate scene;
		scene.Floor.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		scene.Start();
		scene.Patch(scene.Floor, "BoxCollider", { { "Offset", { 100, 0, 0 } }, { "HalfExtents", { 1, 0.5f, 1 } } });
		scene.Frames(30);
		CHECK_FALSE(scene.Physics().IsBodySleeping(scene.Crate));
		CHECK(scene.Crate.GetTransform().Translation.y < 0.5f);
	}

	TEST_CASE("a track key naming a missing file is a validation error")
	{
		TempProject project("ProbeValidate");
		AutomationSession session;
		CommandRegistry commands;
		const auto run = [&](const std::string& command, const json& params) { return BasaltTest::Run(commands, session, command, params); };
		std::string writeError;
		REQUIRE_MESSAGE(WritePng(Project::ResolvePath("Assets/Frame.png"), { 255, 0, 0, 255 }, 1, 1, writeError), writeError);
		run("asset.refresh", json::object());
		Entity entity = session.GetScene()->CreateEntity("Flipbook");
		entity.AddComponent<MaterialComponent>().AlbedoMap = "Assets/Frame.png";
		run("animation.key", { { "entity", "Flipbook" }, { "component", "Material" }, { "field", "AlbedoMap" }, { "time", 0 } });
		entity.GetComponent<MaterialComponent>().AlbedoMap.clear();
		run("scene.save", { { "path", "Assets/Flipbook.bscene" } });
		REQUIRE(std::filesystem::remove(Project::ResolvePath("Assets/Frame.png")));
		run("asset.refresh", json::object());
		run("scene.save", json::object());
		const json report = run("project.validate", json::object());
		bool isError = false;
		for (const json& issue : report["issues"])
			if (issue.dump().find("Assets/Frame.png") != std::string::npos && issue.value("severity", "") == "error")
				isError = true;
		CAPTURE(report.dump());
		CHECK(isError);
	}

	TEST_CASE("trigger grown in place reaches a sleeper outside its old bounds")
	{
		RestingCrate scene;
		Entity zone = scene.World.CreateEntity("Zone");
		zone.GetTransform().Translation = { 6, 1, 0 };
		zone.AddComponent<RigidBodyComponent>().IsTrigger = true;
		zone.AddComponent<BoxColliderComponent>().HalfExtents = { 0.5f, 0.5f, 0.5f };
		scene.Start();
		const int began = scene.Log.Begin;
		scene.Patch(zone, "BoxCollider", { { "HalfExtents", { 6.5f, 0.5f, 0.5f } } });
		scene.Frames(10);
		CHECK(scene.Log.Begin == began + 1);
		CHECK(scene.Log.End == 0);
	}

	TEST_CASE("trigger moved off a sleeper in place reports its exit")
	{
		RestingCrate scene;
		Entity zone = scene.World.CreateEntity("Zone");
		zone.GetTransform().Translation = { 0, 1, 0 };
		zone.AddComponent<RigidBodyComponent>().IsTrigger = true;
		zone.AddComponent<BoxColliderComponent>().HalfExtents = { 2, 2, 2 };
		scene.Start();
		scene.Patch(zone, "BoxCollider", { { "Offset", { 50, 0, 0 } } });
		scene.Frames(10);
		CHECK(scene.Log.End == 1);
	}

	TEST_CASE("trigger resized in place keeps its overlaps")
	{
		RestingCrate scene;
		Entity zone = scene.World.CreateEntity("Zone");
		zone.GetTransform().Translation = { 0, 1, 0 };
		zone.AddComponent<RigidBodyComponent>().IsTrigger = true;
		zone.AddComponent<BoxColliderComponent>().HalfExtents = { 2, 2, 2 };
		scene.Start();
		const int began = scene.Log.Begin;
		for (int i = 1; i <= 20; i++)
		{
			scene.Patch(zone, "BoxCollider", { { "HalfExtents", { 2 + 0.05f * static_cast<float>(i), 2, 2 } } });
			scene.Frames(1);
		}
		CHECK(scene.Log.Begin == began);
		CHECK(scene.Log.End == 0);
	}

	TEST_CASE("a joint connected on the second body hears of its center of mass")
	{
		RestingCrate scene;
		scene.Crate.GetTransform().Translation = { 0, 5, 0 };
		scene.Crate.GetComponent<RigidBodyComponent>().GravityFactor = 0;
		scene.Crate.AddComponent<BoxColliderComponent>();
		Entity holder = scene.World.CreateEntity("Holder");
		holder.GetTransform().Translation = { 0, 5, 0 };
		auto& frame = holder.AddComponent<RigidBodyComponent>();
		frame.Type = RigidBodyType::Dynamic;
		frame.GravityFactor = 0;
		frame.Mass = 1000;
		holder.AddComponent<SphereColliderComponent>().Radius = 0.1f;
		auto& joint = holder.AddComponent<JointComponent>();
		joint.Type = JointType::Fixed;
		joint.ConnectedEntity = scene.Crate.GetUUID();
		joint.EnableCollision = false;
		scene.World.OnRuntimeStart();
		scene.Frames(5);
		REQUIRE(scene.Physics().HasJoint(holder));
		for (int i = 1; i <= 30; i++)
		{
			scene.Patch(scene.Crate, "BoxCollider", { { "Offset", { 0.05f * static_cast<float>(i), 0, 0 } } });
			scene.Frames(1);
		}
		CHECK(glm::length(scene.Crate.GetTransform().Translation - glm::vec3(0, 5, 0)) < 0.02f);
	}

	TEST_CASE("a mesh collider's material changes in place")
	{
		RestingCrate scene(20.0f);
		scene.Floor.RemoveComponent<BoxColliderComponent>();
		scene.Floor.GetTransform().Scale = { 40, 1, 40 };
		scene.Floor.AddComponent<MeshComponent>().Mesh = "builtin://Plane";
		scene.Floor.AddComponent<MeshColliderComponent>();
		scene.Crate.GetTransform().Translation = glm::vec3(scene.Floor.GetTransform().Rotation * glm::vec3(0, 0.52f, 0));
		scene.Start();
		const glm::vec3 start = scene.Crate.GetTransform().Translation;
		scene.Patch(scene.Floor, "MeshCollider", { { "OverrideMaterial", true }, { "Friction", 0.0f } });
		scene.Frames(60);
		CHECK(glm::length(scene.Crate.GetTransform().Translation - start) > 0.5f);
		CHECK(scene.Log.End == 0);
	}

	TEST_CASE("another mesh under a mesh collider rebuilds the body")
	{
		const auto ended = [](const std::function<void(RestingCrate&)>& change) {
			RestingCrate scene;
			scene.Floor.RemoveComponent<BoxColliderComponent>();
			scene.Floor.GetTransform().Scale = { 40, 1, 40 };
			scene.Floor.AddComponent<MeshComponent>().Mesh = "builtin://Plane";
			scene.Floor.AddComponent<MeshColliderComponent>();
			scene.Crate.GetTransform().Translation = { 0, 0.52f, 0 };
			scene.Start();
			change(scene);
			scene.Frames(2);
			return scene.Log.End;
		};
		SUBCASE("The mesh it borrows")
		{
			CHECK(ended([](RestingCrate& scene) { scene.Floor.GetComponent<MeshComponent>().Mesh = "builtin://Cube"; scene.Floor.AddOrReplaceComponent<MeshComponent>(scene.Floor.GetComponent<MeshComponent>()); }) > 0);
		}
		SUBCASE("Its own mesh")
		{
			CHECK(ended([](RestingCrate& scene) { scene.Patch(scene.Floor, "MeshCollider", { { "Mesh", "builtin://Cube" } }); }) > 0);
		}
		SUBCASE("Convex")
		{
			CHECK(ended([](RestingCrate& scene) { scene.Patch(scene.Floor, "MeshCollider", { { "Convex", true } }); }) > 0);
		}
	}

	TEST_CASE("every live MeshCollider field leaves contacts alone")
	{
		const ComponentInfo& info = *ComponentRegistry::Find("MeshCollider");
		for (const std::string& field : info.AnimatableFields.value())
		{
			CAPTURE(field);
			RestingCrate scene;
			scene.Floor.RemoveComponent<BoxColliderComponent>();
			scene.Floor.GetTransform().Scale = { 40, 1, 40 };
			scene.Floor.AddComponent<MeshComponent>().Mesh = "builtin://Plane";
			scene.Floor.AddComponent<MeshColliderComponent>();
			scene.Crate.GetTransform().Translation = { 0, 0.52f, 0 };
			scene.Start();
			const json original = info.Serialize(scene.Floor);
			scene.Patch(scene.Floor, "MeshCollider", { { field, Nudged(original[field], info.FieldTypes.at(field), nullptr) } });
			scene.Frames(30);
			CHECK(scene.Log.End == 0);
		}
	}

	TEST_CASE("tracks on every live CharacterController field keep its overlaps")
	{
		const ComponentInfo& info = *ComponentRegistry::Find("CharacterController");
		for (const std::string& field : info.AnimatableFields.value())
		{
			CAPTURE(field);
			RestingCrate scene;
			scene.Crate.RemoveComponent<RigidBodyComponent>();
			scene.Crate.GetTransform().Translation = { 0, 1.5f, 0 };
			auto& capsule = scene.Crate.AddComponent<CapsuleColliderComponent>();
			capsule.Radius = 0.4f;
			capsule.HalfHeight = 0.6f;
			scene.Crate.AddComponent<CharacterControllerComponent>();
			Entity zone = scene.World.CreateEntity("Zone");
			zone.GetTransform().Translation = { 0, 1.5f, 0 };
			zone.AddComponent<RigidBodyComponent>().IsTrigger = true;
			zone.AddComponent<BoxColliderComponent>().HalfExtents = { 3, 3, 3 };
			std::string error;
			const json original = info.Serialize(scene.Crate);
			const auto options = info.EnumOptions.find(field);
			const json changed = Nudged(original[field], info.FieldTypes.at(field), options == info.EnumOptions.end() ? nullptr : &options->second);
			REQUIRE_MESSAGE(PropertyAnimation::Key(scene.Crate, "CharacterController", field, 0, error), error);
			scene.Patch(scene.Crate, "CharacterController", { { field, changed } });
			REQUIRE_MESSAGE(PropertyAnimation::Key(scene.Crate, "CharacterController", field, 1, error), error);
			scene.Patch(scene.Crate, "CharacterController", { { field, original[field] } });
			scene.Patch(scene.Crate, "PropertyAnimation", { { "Duration", 0.25f }, { "Looping", false }, { "PlayOnStart", false } });
			scene.World.OnRuntimeStart();
			scene.Frames(30);
			REQUIRE(scene.Log.Begin >= 1);
			const int began = scene.Log.Begin;
			REQUIRE(scene.World.GetPropertyAnimationSystem()->Control(scene.Crate, "Play", 0, error));
			scene.Frames(30);
			CHECK(scene.Log.End == 0);
			CHECK(scene.Log.Begin == began);
		}
	}

	TEST_CASE("every ragdoll field is live or rebuilds, as AnimatableFields says")
	{
		const ComponentInfo& info = *ComponentRegistry::Find("Ragdoll");
		Scene scene;
		Entity entity = scene.CreateEntity();
		entity.AddComponent<RagdollComponent>();
		const RagdollComponent built = entity.GetComponent<RagdollComponent>();
		const json original = info.Serialize(entity);
		for (const std::string& field : info.Fields)
		{
			CAPTURE(field);
			if (field == "Enabled" || !(original[field].is_number() || original[field].is_boolean()))
				continue;
			const json changed = original[field].is_boolean() ? json(!original[field].get<bool>()) : json(original[field].get<float>() == 0 ? 0.01f : original[field].get<float>() * 0.98f);
			std::string error;
			REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(entity, "Ragdoll", { { field, changed } }, error), error);
			const bool live = std::ranges::find(info.AnimatableFields.value(), field) != info.AnimatableFields.value().end();
			CHECK(RagdollNeedsRebuild(built, entity.GetComponent<RagdollComponent>()) == !live);
			REQUIRE(ComponentRegistry::AddOrPatch(entity, "Ragdoll", { { field, original[field] } }, error));
		}
	}

	TEST_CASE("Lua writes a Target of 2^63 and above back")
	{
		TempProject project("ProbeLua");
		AutomationSession session;
		CommandRegistry commands;
		const auto run = [&](const std::string& command, const json& params) { return BasaltTest::Run(commands, session, command, params); };
		auto scene = session.GetScene();
		Entity root = scene->CreateEntity("Root");
		Entity lamp = scene->CreateEntityWithUUID(UUID(0x8000000000000123ull), "Lamp");
		lamp.AddComponent<PointLightComponent>().Intensity = 1;
		std::string error;
		REQUIRE(PropertyAnimation::Key(root, "PointLight", "Intensity", 0, error, lamp.GetUUID()));
		lamp.GetComponent<PointLightComponent>().Intensity = 3;
		REQUIRE(PropertyAnimation::Key(root, "PointLight", "Intensity", 1, error, lamp.GetUUID()));
		scene->GetEntityByUUID(root.GetUUID()).GetComponent<PropertyAnimationComponent>().PlayOnStart = false;
		run("play.start", json::object());
		run("lua.exec", { { "code", "local r = Scene.FindEntityByName('Root') local l = Scene.FindEntityByName('Lamp') local a = r:GetComponent('PropertyAnimation') assert(l.ID < 0 and a.Tracks[1].Target == l.ID, 'read') a.Tracks[1].Target = l.ID r:SetComponent('PropertyAnimation', { Tracks = a.Tracks }) assert(r:ControlPropertyAnimation('Seek', 0.5), 'seek') assert(math.abs(l:GetComponent('PointLight').Intensity - 2) < 1e-3, 'value')" } });
		run("play.stop", json::object());
	}

	TEST_CASE("nested prefabs keep tracks on their own members and take overrides")
	{
		TempProject project("ProbeNested");
		AutomationSession session;
		CommandRegistry commands;
		const auto run = [&](const std::string& command, const json& params) { return BasaltTest::Run(commands, session, command, params); };
		auto scene = session.GetScene();
		Entity inner = scene->CreateEntity("Inner");
		Entity lamp = scene->CreateEntity("Lamp");
		scene->SetParent(lamp, inner, false);
		lamp.AddComponent<PointLightComponent>();
		std::string error;
		REQUIRE(PropertyAnimation::Key(inner, "PointLight", "Intensity", 0, error, lamp.GetUUID()));
		run("prefab.create", { { "entity", IdOf(inner) }, { "path", "Assets/Inner.bprefab" } });
		Entity outer = scene->CreateEntity("Outer");
		Entity spare = scene->CreateEntity("Spare");
		spare.AddComponent<PointLightComponent>();
		scene->SetParent(spare, outer, false);
		scene->SetParent(inner, outer, false);
		run("prefab.create", { { "entity", IdOf(outer) }, { "path", "Assets/Outer.bprefab" } });
		const auto instantiate = [&] { return session.GetScene()->GetEntityByUUID(UUID(run("prefab.instantiate", { { "path", "Assets/Outer.bprefab" } })["id"].get<uint64_t>())); };
		const auto target = [&](Entity root) { return FindNamed(*session.GetScene(), root, "Inner").GetComponent<PropertyAnimationComponent>().Tracks[0].Target; };
		Entity first = instantiate(), second = instantiate();
		REQUIRE(first);
		REQUIRE(second);
		CHECK(target(first) == FindNamed(*session.GetScene(), first, "Lamp").GetUUID());
		CHECK(target(second) == FindNamed(*session.GetScene(), second, "Lamp").GetUUID());
		CHECK(target(first) != target(second));
		CHECK(run("prefab.overrides", { { "entity", IdOf(first) } })["overrides"].empty());

		Entity held = FindNamed(*session.GetScene(), first, "Inner");
		json tracks = ComponentRegistry::Find("PropertyAnimation")->Serialize(held)["Tracks"];
		tracks[0]["Target"] = static_cast<uint64_t>(FindNamed(*session.GetScene(), first, "Spare").GetUUID());
		run("component.set", { { "entity", IdOf(held) }, { "component", "PropertyAnimation" }, { "data", { { "Tracks", tracks } } } });
		run("prefab.apply", { { "entity", IdOf(first) } });
		Entity third = instantiate();
		CHECK(target(third) == FindNamed(*session.GetScene(), third, "Spare").GetUUID());
		CHECK(target(second) == FindNamed(*session.GetScene(), second, "Spare").GetUUID());
	}

	TEST_CASE("asset tracks in prefab files: encoding, thumbnails, overrides and moves")
	{
		TempProject project("ProbeAssetPrefab");
		AutomationSession session;
		CommandRegistry commands;
		const auto run = [&](const std::string& command, const json& params) { return BasaltTest::Run(commands, session, command, params); };
		std::string writeError;
		for (const char* name : { "Assets/FrameA.png", "Assets/FrameB.png" })
			REQUIRE_MESSAGE(WritePng(Project::ResolvePath(name), { 255, 0, 0, 255 }, 1, 1, writeError), writeError);
		run("asset.refresh", json::object());
		Entity entity = session.GetScene()->CreateEntity("Flipbook");
		entity.AddComponent<MaterialComponent>().AlbedoMap = "Assets/FrameA.png";
		run("animation.key", { { "entity", "Flipbook" }, { "component", "Material" }, { "field", "AlbedoMap" }, { "time", 0 } });
		entity.GetComponent<MaterialComponent>().AlbedoMap = "Assets/FrameB.png";
		run("animation.key", { { "entity", "Flipbook" }, { "component", "Material" }, { "field", "AlbedoMap" }, { "time", 0.5 } });
		const UUID id = entity.GetUUID();
		// Only the track names FrameB now.
		entity.GetComponent<MaterialComponent>().AlbedoMap = "Assets/FrameA.png";
		run("prefab.create", { { "entity", IdOf(entity) }, { "path", "Assets/Flipbook.bprefab" } });
		const auto keyOnDisk = [&] {
			const auto saved = FileSystem::ReadTextFile(Project::ResolvePath("Assets/Flipbook.bprefab"));
			REQUIRE(saved);
			return json::parse(*saved)["Entities"][0]["Components"]["PropertyAnimation"]["Tracks"][0]["Keys"][1]["Asset"];
		};
		REQUIRE(keyOnDisk().is_object());
		CHECK(keyOnDisk()["Path"] == "Assets/FrameB.png");
		ThumbnailCache thumbnails(Project::GetActive()->GetDirectory());
		const auto dependencies = thumbnails.GetDependencies("Assets/Flipbook.bprefab");
		CHECK(std::ranges::find(dependencies, "Assets/FrameB.png.meta") != dependencies.end());

		run("scene.save", { { "path", "Assets/Flipbook.bscene" } });
		run("scene.open", { { "path", "Assets/Flipbook.bscene" } });
		CHECK(run("prefab.overrides", { { "entity", static_cast<uint64_t>(id) } })["overrides"].empty());

		run("asset.move", { { "from", "Assets/FrameB.png" }, { "to", "Assets/Frames/B.png" } });
		CHECK(run("prefab.overrides", { { "entity", static_cast<uint64_t>(id) } })["overrides"].empty());
		const json made = run("prefab.instantiate", { { "path", "Assets/Flipbook.bprefab" } });
		Entity instance = session.GetScene()->GetEntityByUUID(UUID(made["id"].get<uint64_t>()));
		REQUIRE(instance);
		CHECK(instance.GetComponent<PropertyAnimationComponent>().Tracks[0].Keys[1].Asset == "Assets/Frames/B.png");
		run("scene.save", json::object());
		run("scene.open", { { "path", "Assets/Flipbook.bscene" } });
		CHECK(run("prefab.overrides", { { "entity", static_cast<uint64_t>(id) } })["overrides"].empty());
		// A key naming a missing file is an error, not the warning any path-like string gets.
		REQUIRE(std::filesystem::remove(Project::ResolvePath("Assets/FrameA.png")));
		run("asset.refresh", json::object());
		run("scene.save", json::object());
		CHECK(run("project.validate", json::object())["errors"].get<int>() >= 1);
	}

	TEST_CASE("editor.component_set resolves names inside list elements")
	{
		CommandRegistry commands;
		RegisterEditorSelectionCommands(commands);
		AutomationSession session;
		auto scene = session.GetScene();
		Entity a = scene->CreateEntity("A"), b = scene->CreateEntity("B"), lamp = scene->CreateEntity("Lamp");
		lamp.AddComponent<PointLightComponent>();
		std::string error;
		REQUIRE(PropertyAnimation::Key(a, "PointLight", "Intensity", 0, error, lamp.GetUUID()));
		REQUIRE(PropertyAnimation::Key(b, "PointLight", "Intensity", 0, error, lamp.GetUUID()));
		session.SetSelections({ a.GetUUID(), b.GetUUID() });
		json tracks = ComponentRegistry::Find("PropertyAnimation")->Serialize(a)["Tracks"];
		tracks[0]["Target"] = "Lamp";
		commands.Execute(session, "editor.component_set", { { "component", "PropertyAnimation" }, { "data", { { "Tracks", tracks } } } });
		CHECK(a.GetComponent<PropertyAnimationComponent>().Tracks[0].Target == lamp.GetUUID());
		CHECK(b.GetComponent<PropertyAnimationComponent>().Tracks[0].Target == lamp.GetUUID());
	}

	TEST_CASE("one field on two entities is not a repeated target")
	{
		Scene scene;
		Entity timeline = scene.CreateEntity("Timeline"), a = scene.CreateEntity("A"), b = scene.CreateEntity("B");
		a.AddComponent<PointLightComponent>();
		b.AddComponent<PointLightComponent>();
		std::string error;
		REQUIRE(PropertyAnimation::Key(timeline, "PointLight", "Intensity", 0, error, a.GetUUID()));
		REQUIRE(PropertyAnimation::Key(timeline, "PointLight", "Intensity", 0, error, b.GetUUID()));
		const auto warnings = GetAnimationWarnings(timeline);
		CHECK_FALSE(std::ranges::any_of(warnings, [](const std::string& warning) { return warning.find("reaches the same field") != std::string::npos; }));
	}

	TEST_CASE("restitution and its combine mode apply live")
	{
		const auto bounce = [](const json& patch) {
			RestingCrate scene;
			scene.Crate.GetTransform().Translation = { 0, 3, 0 };
			auto& crate = scene.Crate.GetComponent<RigidBodyComponent>();
			crate.Restitution = 1;
			scene.Crate.AddComponent<BoxColliderComponent>();
			scene.World.OnRuntimeStart();
			scene.Frames(1);
			if (!patch.empty())
				scene.Patch(scene.Crate, "RigidBody", patch);
			float peak = 0;
			for (int i = 0; i < 90; i++)
			{
				scene.Frames(1);
				peak = std::max(peak, scene.Physics().GetLinearVelocity(scene.Crate).y);
			}
			return peak;
		};
		CHECK(bounce(json::object()) > 2.0f);
		CHECK(bounce({ { "Restitution", 0.0f } }) < 0.5f);
		CHECK(bounce({ { "RestitutionCombine", "Min" } }) < 0.5f);
	}

	TEST_CASE("a collider sized back to what it was built with applies")
	{
		RestingCrate scene;
		scene.Crate.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		scene.Crate.AddComponent<BoxColliderComponent>();
		scene.World.OnRuntimeStart();
		scene.Frames(2);
		scene.Patch(scene.Crate, "BoxCollider", { { "HalfExtents", { 0.5f, 3, 0.5f } } });
		scene.Frames(1);
		scene.Patch(scene.Crate, "BoxCollider", { { "HalfExtents", { 0.5f, 0.5f, 0.5f } } });
		scene.Frames(1);
		const auto hit = scene.Physics().Raycast({ 0, 10, 0 }, { 0, -1, 0 }, 20);
		REQUIRE(hit);
		CHECK(hit->Distance == doctest::Approx(10 - (1.01f + 0.5f)).epsilon(0.01));
	}

	TEST_CASE("a character sized back to what it was built with applies")
	{
		RestingCrate scene;
		scene.Crate.RemoveComponent<RigidBodyComponent>();
		scene.Crate.GetTransform().Translation = { 0, 1.5f, 0 };
		auto& capsule = scene.Crate.AddComponent<CapsuleColliderComponent>();
		capsule.Radius = 0.4f;
		capsule.HalfHeight = 0.6f;
		scene.Crate.AddComponent<CharacterControllerComponent>();
		scene.World.OnRuntimeStart();
		scene.Frames(30);
		scene.Patch(scene.Crate, "CapsuleCollider", { { "HalfHeight", 0.9f } });
		scene.Frames(30);
		scene.Patch(scene.Crate, "CapsuleCollider", { { "HalfHeight", 0.6f } });
		scene.Frames(60);
		CHECK(scene.Crate.GetTransform().Translation.y == doctest::Approx(0.5f + 0.6f + 0.4f).epsilon(0.03));
	}

	TEST_CASE("a mass set in play gives the inertia a build at that mass gives")
	{
		const auto spin = [](bool live) {
			RestingCrate scene;
			scene.Crate.GetTransform().Translation.y = 100;
			scene.Crate.GetComponent<RigidBodyComponent>().GravityFactor = 0;
			scene.Crate.GetComponent<RigidBodyComponent>().AngularDamping = 0;
			if (!live)
				scene.Crate.GetComponent<RigidBodyComponent>().Mass = 4;
			scene.Crate.AddComponent<BoxColliderComponent>();
			scene.World.OnRuntimeStart();
			if (live)
				scene.Patch(scene.Crate, "RigidBody", { { "Mass", 4.0f } });
			scene.Frames(1);
			scene.Physics().AddTorque(scene.Crate, { 0, 8, 0 });
			scene.Frames(1);
			return scene.Physics().GetAngularVelocity(scene.Crate).y;
		};
		const float built = spin(false);
		CHECK(built > 0);
		CHECK(spin(true) == doctest::Approx(built).epsilon(0.01));
	}

	TEST_CASE("one timeline drives one component on two entities in the same sample")
	{
		Scene scene;
		Entity timeline = scene.CreateEntity("Timeline"), a = scene.CreateEntity("A"), b = scene.CreateEntity("B");
		a.AddComponent<PointLightComponent>().Intensity = 1;
		b.AddComponent<PointLightComponent>().Intensity = 10;
		std::string error;
		REQUIRE(PropertyAnimation::Key(timeline, "PointLight", "Intensity", 0, error, a.GetUUID()));
		REQUIRE(PropertyAnimation::Key(timeline, "PointLight", "Intensity", 0, error, b.GetUUID()));
		a.GetComponent<PointLightComponent>().Intensity = 3;
		b.GetComponent<PointLightComponent>().Intensity = 30;
		REQUIRE(PropertyAnimation::Key(timeline, "PointLight", "Intensity", 1, error, a.GetUUID()));
		REQUIRE(PropertyAnimation::Key(timeline, "PointLight", "Intensity", 1, error, b.GetUUID()));
		REQUIRE_MESSAGE(PropertyAnimation::Sample(timeline, timeline.GetComponent<PropertyAnimationComponent>(), 0.5f, error), error);
		CHECK(a.GetComponent<PointLightComponent>().Intensity == doctest::Approx(2.0f));
		CHECK(b.GetComponent<PointLightComponent>().Intensity == doctest::Approx(20.0f));
	}

	TEST_CASE("IsTrigger changes in play rebuild the body")
	{
		RestingCrate scene;
		scene.Crate.GetComponent<RigidBodyComponent>().AllowSleeping = false;
		scene.Crate.AddComponent<BoxColliderComponent>();
		scene.World.OnRuntimeStart();
		scene.Frames(60);
		scene.Patch(scene.Floor, "RigidBody", { { "IsTrigger", true } });
		scene.Frames(30);
		CHECK(scene.Crate.GetTransform().Translation.y < 0.5f);
	}

	TEST_CASE("axis rotations of either sign have no negative zeros")
	{
		for (const float degrees : { 30.0f, -30.0f, 120.0f, -120.0f })
			for (const glm::vec3& axis : { glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1) })
				for (const float sign : { 1.0f, -1.0f })
				{
					glm::quat rotation = Math::QuatFromEuler(glm::radians(axis * degrees));
					rotation = glm::quat(rotation.w * sign, rotation.x * sign, rotation.y * sign, rotation.z * sign);
					const glm::vec3 euler = Math::EulerFromQuat(rotation);
					CAPTURE(degrees);
					for (int i = 0; i < 3; i++)
						if (euler[i] == 0.0f)
							CHECK_FALSE(std::signbit(euler[i]));
				}
	}

	TEST_CASE("The merge that lost the target leaves no override")
	{
		DeletedLamp f("PropertyLostA2");
		const json overrides = f.Run("prefab.overrides", { { "entity", f.InstanceId } })["overrides"];
		CAPTURE(overrides.dump());
		CHECK(overrides.empty());
	}

	TEST_CASE("Repairing the track in the prefab reaches the instance")
	{
		DeletedLamp f("PropertyLostA3");
		f.Run("prefab.open", { { "path", "Assets/Lamp.bprefab" } });
		Entity opened = f.Session.GetScene()->FindEntityByName("Root");
		REQUIRE(opened);
		json tracks = ComponentRegistry::Find("PropertyAnimation")->Serialize(opened)["Tracks"];
		CAPTURE(tracks.dump());
		tracks[0]["Target"] = 0;
		f.Run("component.set", { { "entity", "Root" }, { "component", "PropertyAnimation" }, { "data", { { "Tracks", tracks } } } });
		f.Run("prefab.save", json::object());
		f.Run("prefab.close", json::object());
		const auto& repaired = f.Instance().GetComponent<PropertyAnimationComponent>().Tracks[0];
		CHECK(static_cast<uint64_t>(repaired.Target) == 0);
	}

	TEST_CASE("Editing another track of an instance with a lost target, then applying")
	{
		DeletedLamp f("PropertyLostA4");
		json tracks = f.Tracks();
		tracks[1]["Stepped"] = !tracks[1]["Stepped"].get<bool>();
		// The ID the track already holds does not block the edit.
		f.Run("component.set", { { "entity", f.InstanceId }, { "component", "PropertyAnimation" }, { "data", { { "Tracks", tracks } } } });
		CHECK(f.Tracks()[1]["Stepped"] == tracks[1]["Stepped"]);
		const json applied = f.Run("prefab.apply", { { "entity", f.InstanceId } });
		// The prefab keeps the list, and the warning says the edit stayed behind with it.
		CHECK(applied["warnings"].dump().find("so Tracks stays as the prefab has it, without this instance's other changes to it") != std::string::npos);
		const json made = f.Run("prefab.instantiate", { { "path", "Assets/Lamp.bprefab" } });
		Entity fresh = f.Session.GetScene()->GetEntityByUUID(UUID(made["id"].get<uint64_t>()));
		REQUIRE(fresh);
		CHECK(fresh.GetComponent<PropertyAnimationComponent>().Tracks[1].Stepped != tracks[1]["Stepped"].get<bool>());
		CHECK(f.Tracks()[1]["Stepped"] == tracks[1]["Stepped"]);
	}

	TEST_CASE("A lost target survives a save and project.validate names it")
	{
		DeletedLamp f("PropertyLostA5");
		const UUID lost = f.Instance().GetComponent<PropertyAnimationComponent>().Tracks[0].Target;
		REQUIRE(lost != 0);
		f.Run("scene.save", { { "path", "Assets/Lost.bscene" } });
		f.Run("scene.open", { { "path", "Assets/Lost.bscene" } });
		CHECK(f.Instance().GetComponent<PropertyAnimationComponent>().Tracks[0].Target == lost);
		const json report = f.Run("project.validate", json::object());
		CAPTURE(report.dump());
		CHECK(report["errors"].get<int>() >= 1);
		CHECK(report.dump().find("PropertyAnimation.Tracks[0].Target") != std::string::npos);
		Entity duplicate = f.Session.GetScene()->DuplicateEntity(f.Instance());
		CHECK(duplicate.GetComponent<PropertyAnimationComponent>().Tracks[0].Target == lost);
	}

	TEST_CASE("prefab.apply of a component the prefab lacks keeps an empty list")
	{
		TempProject project("PropertyCaseB1");
		AutomationSession session;
		CommandRegistry commands;
		const auto run = [&](const std::string& command, const json& params) { return BasaltTest::Run(commands, session, command, params); };
		auto scene = session.GetScene();
		Entity root = scene->CreateEntity("Root");
		run("prefab.create", { { "entity", IdOf(root) }, { "path", "Assets/Plain.bprefab" } });
		const uint64_t instanceId = run("prefab.instantiate", { { "path", "Assets/Plain.bprefab" } })["id"];
		Entity instance = session.GetScene()->GetEntityByUUID(UUID(instanceId));
		Entity outside = session.GetScene()->CreateEntity("Outside");
		outside.AddComponent<PointLightComponent>();
		std::string error;
		REQUIRE_MESSAGE(PropertyAnimation::Key(instance, "PointLight", "Intensity", 0, error, outside.GetUUID()), error);
		const json applied = run("prefab.apply", { { "entity", instanceId } });
		REQUIRE(applied.contains("warnings"));
		CHECK(applied["warnings"].dump().find("so 1 of 1 Tracks stayed out of it") != std::string::npos);
		const auto saved = FileSystem::ReadTextFile(Project::ResolvePath("Assets/Plain.bprefab"));
		REQUIRE(saved);
		CHECK(json::parse(*saved)["Entities"][0]["Components"]["PropertyAnimation"]["Tracks"] == json::array());
		CHECK(instance.GetComponent<PropertyAnimationComponent>().Tracks[0].Target == outside.GetUUID());
	}

	TEST_CASE("A kept list keeps its asset keys, encoded")
	{
		TempProject project("PropertyCaseB2");
		AutomationSession session;
		CommandRegistry commands;
		const auto run = [&](const std::string& command, const json& params) { return BasaltTest::Run(commands, session, command, params); };
		std::string error;
		REQUIRE_MESSAGE(WritePng(Project::ResolvePath("Assets/Frame.png"), { 255, 0, 0, 255 }, 1, 1, error), error);
		run("asset.refresh", json::object());
		Entity root = session.GetScene()->CreateEntity("Root");
		root.AddComponent<MaterialComponent>().AlbedoMap = "Assets/Frame.png";
		run("animation.key", { { "entity", "Root" }, { "component", "Material" }, { "field", "AlbedoMap" }, { "time", 0 } });
		root.GetComponent<MaterialComponent>().AlbedoMap.clear();
		run("prefab.create", { { "entity", IdOf(root) }, { "path", "Assets/Flip.bprefab" } });
		const uint64_t instanceId = run("prefab.instantiate", { { "path", "Assets/Flip.bprefab" } })["id"];
		Entity instance = session.GetScene()->GetEntityByUUID(UUID(instanceId));
		Entity outside = session.GetScene()->CreateEntity("Outside");
		json tracks = ComponentRegistry::Find("PropertyAnimation")->Serialize(instance)["Tracks"];
		tracks[0]["Target"] = static_cast<uint64_t>(outside.GetUUID());
		outside.AddComponent<MaterialComponent>();
		run("component.set", { { "entity", instanceId }, { "component", "PropertyAnimation" }, { "data", { { "Tracks", tracks } } } });
		const json applied = run("prefab.apply", { { "entity", instanceId } });
		REQUIRE(applied.contains("warnings"));
		const auto saved = FileSystem::ReadTextFile(Project::ResolvePath("Assets/Flip.bprefab"));
		REQUIRE(saved);
		const json key = json::parse(*saved)["Entities"][0]["Components"]["PropertyAnimation"]["Tracks"][0]["Keys"][0]["Asset"];
		CAPTURE(key.dump());
		REQUIRE(key.is_object());
		CHECK(key["Path"] == "Assets/Frame.png");
		const json made = run("prefab.instantiate", { { "path", "Assets/Flip.bprefab" } });
		Entity fresh = session.GetScene()->GetEntityByUUID(UUID(made["id"].get<uint64_t>()));
		REQUIRE(fresh);
		CHECK(fresh.GetComponent<PropertyAnimationComponent>().Tracks[0].Keys[0].Asset == "Assets/Frame.png");
	}

	TEST_CASE("The instance that applied keeps its own track as an override")
	{
		TempProject project("PropertyCaseB3");
		AutomationSession session;
		CommandRegistry commands;
		const auto run = [&](const std::string& command, const json& params) { return BasaltTest::Run(commands, session, command, params); };
		auto scene = session.GetScene();
		Entity root = scene->CreateEntity("Root");
		Entity lamp = scene->CreateEntity("Lamp");
		scene->SetParent(lamp, root, false);
		lamp.AddComponent<PointLightComponent>();
		std::string error;
		REQUIRE(PropertyAnimation::Key(root, "PointLight", "Intensity", 0, error, lamp.GetUUID()));
		run("prefab.create", { { "entity", IdOf(root) }, { "path", "Assets/Lamp.bprefab" } });
		const uint64_t instanceId = run("prefab.instantiate", { { "path", "Assets/Lamp.bprefab" } })["id"];
		Entity instance = session.GetScene()->GetEntityByUUID(UUID(instanceId));
		Entity outside = session.GetScene()->CreateEntity("Outside");
		outside.AddComponent<PointLightComponent>();
		json tracks = ComponentRegistry::Find("PropertyAnimation")->Serialize(instance)["Tracks"];
		tracks[0]["Target"] = static_cast<uint64_t>(outside.GetUUID());
		run("component.set", { { "entity", instanceId }, { "component", "PropertyAnimation" }, { "data", { { "Tracks", tracks } } } });
		run("prefab.apply", { { "entity", instanceId } });
		instance = session.GetScene()->GetEntityByUUID(UUID(instanceId));
		CHECK(instance.GetComponent<PropertyAnimationComponent>().Tracks[0].Target == outside.GetUUID());
		CHECK_FALSE(run("prefab.overrides", { { "entity", instanceId } })["overrides"].empty());
	}

	TEST_CASE("A track on a nested member is not outside the prefab")
	{
		TempProject project("PropertyCaseC1");
		AutomationSession session;
		CommandRegistry commands;
		const auto run = [&](const std::string& command, const json& params) { return BasaltTest::Run(commands, session, command, params); };
		auto scene = session.GetScene();
		Entity inner = scene->CreateEntity("Inner");
		Entity lamp = scene->CreateEntity("Lamp");
		lamp.AddComponent<PointLightComponent>();
		scene->SetParent(lamp, inner, false);
		run("prefab.create", { { "entity", IdOf(inner) }, { "path", "Assets/Inner.bprefab" } });
		Entity outer = scene->CreateEntity("Outer");
		scene->SetParent(inner, outer, false);
		std::string error;
		REQUIRE(PropertyAnimation::Key(outer, "PointLight", "Intensity", 0, error, lamp.GetUUID()));
		const json created = run("prefab.create", { { "entity", IdOf(outer) }, { "path", "Assets/Outer.bprefab" } });
		CHECK(created.dump().find("outside the prefab") == std::string::npos);
		const json applied = run("prefab.apply", { { "entity", IdOf(outer) } });
		CHECK(applied.dump().find("outside the prefab") == std::string::npos);
		run("prefab.open", { { "path", "Assets/Outer.bprefab" } });
		const json saved = run("prefab.save", json::object());
		CHECK(saved.dump().find("outside the prefab") == std::string::npos);
		run("prefab.close", json::object());
		const json made = run("prefab.instantiate", { { "path", "Assets/Outer.bprefab" } });
		Entity fresh = session.GetScene()->GetEntityByUUID(UUID(made["id"].get<uint64_t>()));
		REQUIRE(fresh);
		CHECK(session.GetScene()->GetEntityByUUID(fresh.GetComponent<PropertyAnimationComponent>().Tracks[0].Target).GetName() == "Lamp");
	}

	TEST_CASE("Outside references are reported for fields and not for what means no reference")
	{
		TempProject project("PropertyCaseC2");
		AutomationSession session;
		CommandRegistry commands;
		const auto run = [&](const std::string& command, const json& params) { return BasaltTest::Run(commands, session, command, params); };
		auto scene = session.GetScene();
		Entity root = scene->CreateEntity("Root");
		Entity child = scene->CreateEntity("Child");
		scene->SetParent(child, root, false);
		Entity outside = scene->CreateEntity("Outside");
		std::string error;
		REQUIRE(PropertyAnimation::Key(root, "Transform", "Scale", 0, error));
		child.AddComponent<JointComponent>();
		const json clean = run("prefab.create", { { "entity", IdOf(root) }, { "path", "Assets/Clean.bprefab" } });
		CHECK(clean.dump().find("outside the prefab") == std::string::npos);
		child.GetComponent<JointComponent>().ConnectedEntity = outside.GetUUID();
		const json created = run("prefab.create", { { "entity", IdOf(child) }, { "path", "Assets/Field.bprefab" } });
		CHECK(created.dump().find("'Child' Joint.ConnectedEntity references an entity that is not in the prefab") != std::string::npos);
	}

	TEST_CASE("A top-level ID the component holds must still name an entity")
	{
		CommandRegistry commands;
		AutomationSession session;
		Entity entity = session.GetScene()->CreateEntity("Holder");
		entity.AddComponent<JointComponent>().ConnectedEntity = UUID(987654321);
		const std::string message = RunError(commands, session, "component.set", { { "entity", "Holder" }, { "component", "Joint" }, { "data", { { "ConnectedEntity", 987654321 } } } });
		CHECK(message.find("987654321") != std::string::npos);
	}

	TEST_CASE("editor.component_set is atomic when only some of the selection holds a stale ID")
	{
		CommandRegistry commands;
		RegisterEditorSelectionCommands(commands);
		AutomationSession session;
		auto scene = session.GetScene();
		Entity a = scene->CreateEntity("A"), b = scene->CreateEntity("B");
		for (Entity entity : { a, b })
		{
			PropertyAnimationComponent animation;
			animation.Tracks.push_back({ "Transform", "Scale", false, { { 0, glm::vec4(1.0f) } } });
			entity.AddComponent<PropertyAnimationComponent>(animation);
		}
		a.GetComponent<PropertyAnimationComponent>().Tracks[0].Target = UUID(987654321);
		a.GetComponent<PropertyAnimationComponent>().Duration = 3;
		session.SetSelections({ a.GetUUID(), b.GetUUID() });
		json tracks = ComponentRegistry::Find("PropertyAnimation")->Serialize(a)["Tracks"];
		tracks[0]["Stepped"] = true;
		CHECK_THROWS_AS(commands.Execute(session, "editor.component_set", { { "component", "PropertyAnimation" }, { "data", { { "Tracks", tracks } } } }), CommandError);
		CHECK_FALSE(a.GetComponent<PropertyAnimationComponent>().Tracks[0].Stepped);
		// The entity that holds the ID may keep it.
		session.SetSelections({ a.GetUUID() });
		commands.Execute(session, "editor.component_set", { { "component", "PropertyAnimation" }, { "data", { { "Tracks", tracks } } } });
		CHECK(a.GetComponent<PropertyAnimationComponent>().Tracks[0].Stepped);
	}

	TEST_CASE("assert.component reads a stale ID the entity holds")
	{
		CommandRegistry commands;
		AutomationSession session;
		Entity entity = session.GetScene()->CreateEntity("Holder");
		PropertyAnimationComponent animation;
		animation.Tracks.push_back({ "Transform", "Scale", false, { { 0, glm::vec4(1.0f) } }, UUID(987654321) });
		entity.AddComponent<PropertyAnimationComponent>(animation);
		json tracks = ComponentRegistry::Find("PropertyAnimation")->Serialize(entity)["Tracks"];
		CHECK_NOTHROW(BasaltTest::Run(commands, session, "assert.component", { { "entity", "Holder" }, { "component", "PropertyAnimation" }, { "fields", { { "Tracks", tracks } } } }));
	}

	TEST_CASE("entity.create checks list IDs and resolves list names")
	{
		CommandRegistry commands;
		AutomationSession session;
		Entity lamp = session.GetScene()->CreateEntity("Lamp");
		const json track = { { "Component", "PointLight" }, { "Field", "Intensity" }, { "Target", "Lamp" } };
		const json made = BasaltTest::Run(commands, session, "entity.create", { { "name", "Named" }, { "components", { { "PropertyAnimation", { { "Tracks", json::array({ track }) } } } } } });
		Entity named = session.GetScene()->GetEntityByUUID(UUID(made["id"].get<uint64_t>()));
		REQUIRE(named);
		CHECK(named.GetComponent<PropertyAnimationComponent>().Tracks[0].Target == lamp.GetUUID());
		json stale = track;
		stale["Target"] = 987654321;
		CHECK(RunError(commands, session, "entity.create", { { "name", "Stale" }, { "components", { { "PropertyAnimation", { { "Tracks", json::array({ stale }) } } } } } }).find("987654321") != std::string::npos);
		CHECK_FALSE(session.GetScene()->FindEntityByName("Stale"));
	}

	TEST_CASE("project.validate accepts tracks on the holder and on live entities")
	{
		TempProject project("PropertyCaseE1");
		AutomationSession session;
		CommandRegistry commands;
		const auto run = [&](const std::string& command, const json& params) { return BasaltTest::Run(commands, session, command, params); };
		Entity timeline = session.GetScene()->CreateEntity("Timeline");
		Entity lamp = session.GetScene()->CreateEntity("Lamp");
		lamp.AddComponent<PointLightComponent>();
		timeline.AddComponent<PointLightComponent>();
		run("scene.save", { { "path", "Assets/Tracks.bscene" } });
		const int errors = run("project.validate", json::object())["errors"];
		std::string error;
		REQUIRE(PropertyAnimation::Key(timeline, "PointLight", "Intensity", 0, error));
		REQUIRE(PropertyAnimation::Key(timeline, "PointLight", "Intensity", 0, error, lamp.GetUUID()));
		run("scene.save", json::object());
		const json report = run("project.validate", json::object());
		CAPTURE(report.dump());
		CHECK(report["errors"] == errors);
		CHECK(report.dump().find("names entity") == std::string::npos);
	}

	TEST_CASE("project.validate leaves a stale top-level reference to its own rules")
	{
		TempProject project("PropertyCaseE2");
		AutomationSession session;
		CommandRegistry commands;
		const auto run = [&](const std::string& command, const json& params) { return BasaltTest::Run(commands, session, command, params); };
		Entity holder = session.GetScene()->CreateEntity("Holder");
		holder.AddComponent<JointComponent>().ConnectedEntity = UUID(424242);
		run("scene.save", { { "path", "Assets/Joint.bscene" } });
		const json report = run("project.validate", json::object());
		CAPTURE(report.dump());
		CHECK(report.dump().find("names entity") == std::string::npos);
	}

	TEST_CASE("project.validate on prefab files: its own member passes, outside warns")
	{
		TempProject project("PropertyCaseE3");
		AutomationSession session;
		CommandRegistry commands;
		const auto run = [&](const std::string& command, const json& params) { return BasaltTest::Run(commands, session, command, params); };
		auto scene = session.GetScene();
		Entity root = scene->CreateEntity("Root");
		Entity lamp = scene->CreateEntity("Lamp");
		scene->SetParent(lamp, root, false);
		lamp.AddComponent<PointLightComponent>();
		std::string error;
		REQUIRE(PropertyAnimation::Key(root, "PointLight", "Intensity", 0, error, lamp.GetUUID()));
		run("prefab.create", { { "entity", IdOf(root) }, { "path", "Assets/Own.bprefab" } });
		const json own = run("project.validate", json::object());
		CAPTURE(own.dump());
		CHECK(own.dump().find("names entity") == std::string::npos);
		Entity other = scene->CreateEntity("Other");
		Entity outside = scene->CreateEntity("Outside");
		outside.AddComponent<PointLightComponent>();
		REQUIRE(PropertyAnimation::Key(other, "PointLight", "Intensity", 0, error, outside.GetUUID()));
		run("prefab.create", { { "entity", IdOf(other) }, { "path", "Assets/Out.bprefab" } });
		const json out = run("project.validate", json::object());
		CHECK(out.dump().find("names entity") != std::string::npos);
		// A warning, not an error: it must not refuse an export that prefab.create only warned about.
		CHECK(out["errors"] == own["errors"]);
	}

	TEST_CASE("In-place character edits keep the rebuild's RigidBody and layer warnings")
	{
		RestingCrate scene;
		scene.Crate.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		scene.Crate.GetTransform().Translation = { 0, 1.5f, 0 };
		scene.Crate.AddComponent<CapsuleColliderComponent>();
		scene.Crate.AddComponent<CharacterControllerComponent>().Layer = "Nowhere";
		scene.World.OnRuntimeStart();
		scene.Frames(2);
		const uint64_t since = Log::GetHistory().GetTotalCount();
		// Same warnings, in the same order: nothing to log.
		scene.Patch(scene.Crate, "CapsuleCollider", { { "HalfHeight", 0.6f } });
		scene.Frames(1);
		CHECK(CountLogMessages(since, "its RigidBody is ignored") == 0);
		CHECK(CountLogMessages(since, "unknown physics layer") == 0);
		// A new problem logs the whole set, which still holds the two.
		scene.Patch(scene.Crate, "CapsuleCollider", { { "OverrideMaterial", true } });
		scene.Frames(1);
		CHECK(CountLogMessages(since, "its RigidBody is ignored") == 1);
		CHECK(CountLogMessages(since, "unknown physics layer") == 1);
	}

	TEST_CASE("A character's in-place shape uses its world scale")
	{
		RestingCrate scene;
		scene.Crate.RemoveComponent<RigidBodyComponent>();
		scene.Crate.GetTransform().Translation = { 0, 3, 0 };
		scene.Crate.GetTransform().Scale = glm::vec3(2);
		auto& capsule = scene.Crate.AddComponent<CapsuleColliderComponent>();
		capsule.Radius = 0.2f;
		capsule.HalfHeight = 0.3f;
		scene.Crate.AddComponent<CharacterControllerComponent>();
		scene.World.OnRuntimeStart();
		scene.Frames(60);
		scene.Patch(scene.Crate, "CapsuleCollider", { { "HalfHeight", 0.4f } });
		scene.Frames(60);
		CHECK(scene.Crate.GetTransform().Translation.y == doctest::Approx(0.5f + 2 * (0.4f + 0.2f)).epsilon(0.03));
	}

	TEST_CASE("A character's shape warnings log once, in place and on a rescale")
	{
		RestingCrate scene;
		scene.Crate.RemoveComponent<RigidBodyComponent>();
		scene.Crate.GetTransform().Translation = { 0, 1.5f, 0 };
		scene.Crate.AddComponent<CapsuleColliderComponent>();
		scene.Crate.AddComponent<CharacterControllerComponent>();
		scene.World.OnRuntimeStart();
		scene.Frames(2);
		const uint64_t since = Log::GetHistory().GetTotalCount();
		scene.Patch(scene.Crate, "CapsuleCollider", { { "HalfHeight", -1.0f } });
		scene.Frames(1);
		CHECK(CountLogMessages(since, "CapsuleCollider Radius and HalfHeight must be positive") == 1);
		CHECK(CountLogMessages(since, "using the smallest size; CapsuleCollider") == 0);
		const uint64_t rescaled = Log::GetHistory().GetTotalCount();
		scene.Crate.GetTransform().Scale = glm::vec3(1.5f);
		scene.Frames(1);
		CHECK(CountLogMessages(rescaled, "CapsuleCollider Radius and HalfHeight must be positive") == 1);
	}

	TEST_CASE("Each setting the contact reads wakes what sleeps on a static floor")
	{
		for (const json& change : { json{ { "Friction", 0.5f } }, json{ { "Restitution", 0.5f } }, json{ { "FrictionCombine", "Min" } }, json{ { "RestitutionCombine", "Min" } } })
		{
			CAPTURE(change.dump());
			RestingCrate scene;
			scene.Start();
			scene.Patch(scene.Floor, "RigidBody", change);
			scene.Frames(1);
			CHECK_FALSE(scene.Physics().IsBodySleeping(scene.Crate));
		}
	}

	TEST_CASE("A floor's combine mode that lets go slides what slept on it")
	{
		RestingCrate scene(15.0f);
		scene.Crate.GetComponent<RigidBodyComponent>().Friction = 0.1f;
		scene.Start();
		const glm::vec3 start = scene.Crate.GetTransform().Translation;
		scene.Patch(scene.Floor, "RigidBody", { { "FrictionCombine", "Min" } });
		scene.Frames(60);
		CHECK(glm::length(scene.Crate.GetTransform().Translation - start) > 0.3f);
	}

	TEST_CASE("Mass and damping writes to a kinematic floor leave what sleeps on it alone")
	{
		RestingCrate scene;
		scene.Floor.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		scene.Start();
		scene.Patch(scene.Floor, "RigidBody", { { "Mass", 3.0f }, { "LinearDamping", 0.5f }, { "GravityFactor", 0.5f } });
		scene.Frames(2);
		CHECK(scene.Physics().IsBodySleeping(scene.Crate));
	}

	TEST_CASE("Negative angular damping warns on its own")
	{
		RestingCrate scene;
		scene.Start();
		const uint64_t since = Log::GetHistory().GetTotalCount();
		scene.Patch(scene.Crate, "RigidBody", { { "AngularDamping", -1.0f } });
		scene.Frames(1);
		CHECK(CountLogMessages(since, "damping must not be negative") == 1);
	}

	TEST_CASE("Negative damping stops at 0, built or in place")
	{
		const auto speeds = [](bool live) {
			RestingCrate scene;
			scene.Crate.GetTransform().Translation.y = 100;
			auto& body = scene.Crate.GetComponent<RigidBodyComponent>();
			body.GravityFactor = 0;
			if (!live)
			{
				body.LinearDamping = -1;
				body.AngularDamping = -1;
			}
			scene.Crate.AddComponent<BoxColliderComponent>();
			scene.World.OnRuntimeStart();
			if (live)
				scene.Patch(scene.Crate, "RigidBody", { { "LinearDamping", -1.0f }, { "AngularDamping", -1.0f } });
			scene.Frames(1);
			scene.Physics().SetLinearVelocity(scene.Crate, { 1, 0, 0 });
			scene.Physics().SetAngularVelocity(scene.Crate, { 0, 1, 0 });
			scene.Frames(60);
			return glm::vec2(glm::length(scene.Physics().GetLinearVelocity(scene.Crate)), glm::length(scene.Physics().GetAngularVelocity(scene.Crate)));
		};
		for (const bool live : { false, true })
		{
			CAPTURE(live);
			const glm::vec2 speed = speeds(live);
			CHECK(speed.x == doctest::Approx(1.0f).epsilon(0.01));
			CHECK(speed.y == doctest::Approx(1.0f).epsilon(0.01));
		}
	}

	TEST_CASE("A rebuild after an in-place update repeats nothing")
	{
		RestingCrate scene;
		scene.Crate.AddComponent<BoxColliderComponent>().HalfExtents = { 0, 0.5f, 0.5f };
		scene.World.OnRuntimeStart();
		scene.Frames(2);
		const uint64_t since = Log::GetHistory().GetTotalCount();
		// The shape's warning stays in the logged set through a settings update and the rebuild after it.
		scene.Patch(scene.Crate, "RigidBody", { { "Friction", 0.5f } });
		scene.Frames(1);
		scene.Physics().RecreateBody(scene.Crate);
		scene.Frames(1);
		CHECK(CountLogMessages(since, "BoxCollider HalfExtents must be positive") == 0);
	}

	TEST_CASE("A setting fixed in place does not come back with the next problem")
	{
		RestingCrate scene;
		scene.Crate.GetComponent<RigidBodyComponent>().Friction = -1;
		scene.Crate.AddComponent<BoxColliderComponent>();
		scene.World.OnRuntimeStart();
		scene.Frames(2);
		const uint64_t since = Log::GetHistory().GetTotalCount();
		scene.Patch(scene.Crate, "RigidBody", { { "Friction", 1.0f } });
		scene.Frames(1);
		scene.Patch(scene.Crate, "RigidBody", { { "Mass", 0.0f } });
		scene.Frames(1);
		CHECK(CountLogMessages(since, "Mass must be positive") == 1);
		CHECK(CountLogMessages(since, "Friction must not be negative") == 0);
	}

	TEST_CASE("A capsule's zero radius warns")
	{
		RestingCrate scene;
		scene.Crate.AddComponent<CapsuleColliderComponent>();
		scene.Start();
		const uint64_t since = Log::GetHistory().GetTotalCount();
		scene.Patch(scene.Crate, "CapsuleCollider", { { "Radius", 0.0f } });
		scene.Frames(1);
		CHECK(CountLogMessages(since, "CapsuleCollider Radius and HalfHeight must be positive") == 1);
	}

	TEST_CASE("A new track on the holder itself goes into the prefab")
	{
		TempProject project("PropertyCaseB4");
		AutomationSession session;
		CommandRegistry commands;
		const auto run = [&](const std::string& command, const json& params) { return BasaltTest::Run(commands, session, command, params); };
		auto scene = session.GetScene();
		Entity root = scene->CreateEntity("Root");
		root.AddComponent<PointLightComponent>();
		Entity lamp = scene->CreateEntity("Lamp");
		scene->SetParent(lamp, root, false);
		lamp.AddComponent<PointLightComponent>();
		std::string error;
		REQUIRE(PropertyAnimation::Key(root, "PointLight", "Intensity", 0, error, lamp.GetUUID()));
		run("prefab.create", { { "entity", IdOf(root) }, { "path", "Assets/Lamp.bprefab" } });
		const uint64_t instanceId = run("prefab.instantiate", { { "path", "Assets/Lamp.bprefab" } })["id"];
		Entity instance = session.GetScene()->GetEntityByUUID(UUID(instanceId));
		REQUIRE_MESSAGE(PropertyAnimation::Key(instance, "PointLight", "Intensity", 0, error), error);
		const json applied = run("prefab.apply", { { "entity", instanceId } });
		CHECK(applied.dump().find("outside the prefab") == std::string::npos);
		const json made = run("prefab.instantiate", { { "path", "Assets/Lamp.bprefab" } });
		Entity fresh = session.GetScene()->GetEntityByUUID(UUID(made["id"].get<uint64_t>()));
		REQUIRE(fresh);
		const auto& tracks = fresh.GetComponent<PropertyAnimationComponent>().Tracks;
		REQUIRE(tracks.size() == 2);
		CHECK(tracks[1].Target == 0);
	}
}
