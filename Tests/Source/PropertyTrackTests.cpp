#include "AutomationTestUtils.h"
#include "TestUtils.h"

#include <Basalt/Animation/AnimatedModel.h>
#include <Basalt/Animation/PropertyAnimation.h>
#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Asset/ImageUtils.h>
#include <Basalt/Automation/AutomationSession.h>
#include <Basalt/Automation/CommandRegistry.h>
#include <Basalt/Core/FileSystem.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Math/Math.h>
#include <Basalt/Physics/PhysicsWorld.h>
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
		if (type.Count == 0)
			return nudge(value.get<float>());
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

	TEST_CASE("A floor that shrinks away drops what slept on it, past its old and new bounds alike")
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
		SUBCASE("Mass, keeping the shape's inertia at the new mass")
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
		SUBCASE("Restitution, and which body's mode decides it")
		{
			const auto bounce = [](const json& crate, const json& floor) {
				RestingCrate scene;
				scene.Crate.GetTransform().Translation.y = 3;
				scene.Crate.AddComponent<BoxColliderComponent>();
				scene.World.OnRuntimeStart();
				scene.Patch(scene.Crate, "RigidBody", crate);
				scene.Patch(scene.Floor, "RigidBody", floor);
				float rebound = 0;
				for (int i = 0; i < 90; i++)
				{
					scene.Frames(1);
					rebound = std::max(rebound, scene.Physics().GetLinearVelocity(scene.Crate).y);
				}
				return rebound;
			};
			// Jolt's default keeps the larger of the two, Min the smaller.
			CHECK(bounce({ { "Restitution", 0.8f } }, { { "Restitution", 0.0f } }) > 2.0f);
			CHECK(bounce({ { "Restitution", 0.8f }, { "RestitutionCombine", "Min" } }, { { "Restitution", 0.0f } }) < 0.5f);
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

	TEST_CASE("A collider's size reaches queries and its neighbours in place")
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
			scene.Crate.AddComponent<SphereColliderComponent>().Radius = 0.2f;
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
	}

	TEST_CASE("Mass and damping writes to a static floor leave what sleeps on it alone")
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
			CHECK(applied["warnings"].dump().find("references an entity outside the prefab; the prefab keeps its Tracks") != std::string::npos);
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

	TEST_CASE("A prefab member deleted under a track fails loudly, and does not turn the track on its holder")
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
		run("prefab.save", json::object());
		run("prefab.close", json::object());

		Entity instance = session.GetScene()->GetEntityByUUID(UUID(instanceId));
		REQUIRE(instance);
		const auto& track = instance.GetComponent<PropertyAnimationComponent>().Tracks[0];
		CHECK(track.Target != 0);
		const glm::vec3 scale = instance.GetTransform().Scale;
		CHECK_FALSE(PropertyAnimation::Sample(instance, instance.GetComponent<PropertyAnimationComponent>(), 0, error));
		CHECK(error.find("not in the scene") + error.find("deleted") != 2 * std::string::npos);
		CHECK(instance.GetTransform().Scale == scale);
		REQUIRE_FALSE(GetAnimationWarnings(instance).empty());
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
		CHECK(created["warnings"].dump().find("'Root' PropertyAnimation.Tracks[0].Target references an entity outside the prefab") != std::string::npos);
	}

	TEST_CASE("project.validate reports a track whose entity is not in its scene")
	{
		TempProject project("PropertyValidate");
		AutomationSession session;
		CommandRegistry commands;
		const auto run = [&](const std::string& command, const json& params) { return BasaltTest::Run(commands, session, command, params); };
		Entity timeline = session.GetScene()->CreateEntity("Timeline");
		run("scene.save", { { "path", "Assets/Tracks.bscene" } });
		const int errors = run("project.validate", json::object())["errors"];
		std::string error;
		REQUIRE(ComponentRegistry::AddOrPatch(timeline, "PropertyAnimation", { { "Tracks", { { { "Component", "PointLight" }, { "Field", "Intensity" }, { "Target", 424242 } } } } }, error));
		run("scene.save", json::object());
		const json report = run("project.validate", json::object());
		CHECK(report["errors"] == errors + 1);
		CHECK(report.dump().find("PropertyAnimation.Tracks[0].Target names entity 424242") != std::string::npos);
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
			const bool valid = ComponentRegistry::AddOrPatch(entity, "PropertyAnimation", { { "Tracks", { { { "Component", "Material" }, { "Field", field }, { "Keys", { key } } } } } }, error);
			return valid;
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
		// Keys holding another spelling of a locked orientation are not changes, so they cost no update signals.
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
