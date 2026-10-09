#include <doctest/doctest.h>

#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Core/JsonUtils.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Physics/PhysicsLayers.h>
#include <Basalt/Physics/PhysicsWorld.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>

#include "TestUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace Basalt;

namespace {

	constexpr float Step = 1.0f / 60.0f;

	Entity CreateGround(Scene& scene)
	{
		Entity ground = scene.CreateEntity("Ground");
		ground.GetTransform().Translation = { 0.0f, -0.5f, 0.0f };
		ground.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		ground.AddComponent<BoxColliderComponent>().HalfExtents = { 50.0f, 0.5f, 50.0f };
		return ground;
	}

	Entity CreateBox(Scene& scene, const glm::vec3& position)
	{
		Entity box = scene.CreateEntity("Box");
		box.GetTransform().Translation = position;
		box.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		box.AddComponent<BoxColliderComponent>();
		return box;
	}

	// A dynamic box that ignores gravity, for joints tested without a load.
	Entity CreateWeightlessBox(Scene& scene, const glm::vec3& position)
	{
		Entity box = CreateBox(scene, position);
		box.GetComponent<RigidBodyComponent>().GravityFactor = 0.0f;
		return box;
	}

	// Hangs the box from the world 1 m above its centre, with Axis pointing down from the anchor to the box.
	JointComponent& HangFromWorld(Entity box, JointType type)
	{
		auto& joint = box.AddComponent<JointComponent>();
		joint.Type = type;
		joint.Anchor = { 0.0f, 1.0f, 0.0f };
		joint.Axis = { 0.0f, -1.0f, 0.0f };
		return joint;
	}

	// How far (degrees) a rotation turns a unit local axis away from where it points at rest.
	float TiltDegrees(const glm::quat& rotation, const glm::vec3& localAxis)
	{
		return glm::degrees(std::acos(std::clamp(glm::dot(rotation * localAxis, localAxis), -1.0f, 1.0f)));
	}

	void Simulate(Scene& scene, float seconds)
	{
		const int frames = static_cast<int>(std::lround(seconds / Step));
		for (int i = 0; i < frames; i++)
			scene.OnUpdate(Step);
	}

	// Log messages containing text since the history's total count was `since`.
	int CountMessages(uint64_t since, const std::string& text)
	{
		uint64_t next = 0;
		int count = 0;
		for (const LogMessage& message : Log::GetHistory().GetMessagesSince(since, next))
		{
			if (message.Text.find(text) != std::string::npos)
				count++;
		}
		return count;
	}

	// Writes a glTF with one triangle-list primitive. Returns its asset key.
	std::string WriteMeshGltf(const BasaltTest::TempProject& project, const std::string& file, const std::vector<float>& positions, const std::vector<uint16_t>& indices)
	{
		std::string buffer(reinterpret_cast<const char*>(positions.data()), positions.size() * sizeof(float));
		const size_t indexOffset = buffer.size();
		buffer.append(reinterpret_cast<const char*>(indices.data()), indices.size() * sizeof(uint16_t));
		// glTF requires the position bounds; non-finite coordinates are left out of them.
		glm::vec3 min(FLT_MAX);
		glm::vec3 max(-FLT_MAX);
		for (size_t i = 0; i + 2 < positions.size(); i += 3)
		{
			const glm::vec3 position(positions[i], positions[i + 1], positions[i + 2]);
			if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z))
			{
				min = glm::min(min, position);
				max = glm::max(max, position);
			}
		}

		const nlohmann::json gltf = {
			{ "asset", { { "version", "2.0" } } },
			{ "buffers", { { { "byteLength", buffer.size() }, { "uri", "data:application/octet-stream;base64," + BasaltTest::Base64(buffer.data(), buffer.size()) } } } },
			{ "bufferViews", { { { "buffer", 0 }, { "byteOffset", 0 }, { "byteLength", indexOffset } }, { { "buffer", 0 }, { "byteOffset", indexOffset }, { "byteLength", buffer.size() - indexOffset } } } },
			{ "accessors", { { { "bufferView", 0 }, { "componentType", 5126 }, { "count", positions.size() / 3 }, { "type", "VEC3" }, { "min", { min.x, min.y, min.z } }, { "max", { max.x, max.y, max.z } } }, { { "bufferView", 1 }, { "componentType", 5123 }, { "count", indices.size() }, { "type", "SCALAR" } } } },
			{ "meshes", { { { "primitives", { { { "attributes", { { "POSITION", 0 } } }, { "indices", 1 } } } } } } },
			{ "nodes", { { { "mesh", 0 } } } },
			{ "scenes", { { { "nodes", { 0 } } } } },
			{ "scene", 0 },
		};
		return project.WriteFile("Assets/Models/" + file, gltf.dump());
	}

	// Writes a glTF holding a unit cube spanning x = 0.5..1.5 (off-centre, so mirroring shows) plus one vertex at
	// x = 10 that no triangle uses. Returns its asset key.
	std::string WriteOffsetCubeGltf(const BasaltTest::TempProject& project)
	{
		std::vector<float> positions;
		for (int i = 0; i < 8; i++)
			positions.insert(positions.end(), { (i & 1) ? 1.5f : 0.5f, (i & 2) ? 0.5f : -0.5f, (i & 4) ? 0.5f : -0.5f });
		positions.insert(positions.end(), { 10.0f, 0.0f, 0.0f });
		const std::vector<uint16_t> indices = { 0, 6, 2, 0, 4, 6, 1, 3, 7, 1, 7, 5, 0, 1, 5, 0, 5, 4, 2, 7, 3, 2, 6, 7, 0, 3, 1, 0, 2, 3, 4, 5, 7, 4, 7, 6 };
		return WriteMeshGltf(project, "OffsetCube.gltf", positions, indices);
	}

	// Replaces the component through the registry, as SetComponent and component.set do.
	void SetJoint(Entity entity, const std::function<void(JointComponent&)>& change)
	{
		JointComponent joint = entity.GetComponent<JointComponent>();
		change(joint);
		entity.AddOrReplaceComponent<JointComponent>(joint);
	}

}

TEST_SUITE("Physics")
{
	TEST_CASE("A dynamic box falls under gravity and rests on static ground")
	{
		Scene scene;
		CreateGround(scene);
		Entity box = CreateBox(scene, { 0.0f, 5.0f, 0.0f });

		scene.OnSimulationStart();
		REQUIRE(scene.GetPhysicsWorld());
		CHECK(scene.GetPhysicsWorld()->GetBodyCount() == 2);

		Simulate(scene, 0.5f);
		const float afterHalfSecond = box.GetTransform().Translation.y;
		// Free fall: 5 - 0.5 * 9.81 * 0.25 ~= 3.77
		CHECK(afterHalfSecond == doctest::Approx(3.77f).epsilon(0.03));

		Simulate(scene, 3.0f);
		CHECK(box.GetTransform().Translation.y == doctest::Approx(0.5f).epsilon(0.02));
		CHECK(glm::length(scene.GetPhysicsWorld()->GetLinearVelocity(box)) < 0.05f);
		scene.OnSimulationStop();
		CHECK(scene.GetPhysicsWorld() == nullptr);
	}

	TEST_CASE("The collision matrix lets bodies on ignored layer pairs pass through each other")
	{
		PhysicsLayers layers;
		std::string error;
		REQUIRE(layers.Add("Ghost", error));
		REQUIRE(layers.Add("Debris", error));
		layers.SetCollides(0, 1, false);
		layers.SetCollides(2, 2, false);

		Scene scene;
		scene.SetPhysicsLayers(layers);
		CreateGround(scene);
		Entity ghost = CreateBox(scene, { 0.0f, 2.0f, 0.0f });
		ghost.GetComponent<RigidBodyComponent>().Layer = "Ghost";
		Entity solid = CreateBox(scene, { 3.0f, 2.0f, 0.0f });
		solid.GetComponent<RigidBodyComponent>().Layer = "Debris";
		// Two Debris boxes stacked: Debris ignores itself, so the top box falls through the bottom one.
		Entity upper = CreateBox(scene, { 3.0f, 4.0f, 0.0f });
		upper.GetComponent<RigidBodyComponent>().Layer = "Debris";

		scene.OnSimulationStart();
		Simulate(scene, 2.0f);
		CHECK(ghost.GetTransform().Translation.y < -5.0f);
		CHECK(solid.GetTransform().Translation.y == doctest::Approx(0.5f).epsilon(0.02));
		CHECK(upper.GetTransform().Translation.y == doctest::Approx(0.5f).epsilon(0.02));
		scene.OnSimulationStop();
	}

	TEST_CASE("Physics uses the active project's layers unless the scene overrides them")
	{
		BasaltTest::TempProject temp("PhysicsProjectLayers");
		REQUIRE(temp.IsValid());
		std::string error;
		PhysicsLayers& projectLayers = Project::GetActive()->GetConfig().Physics;
		REQUIRE(projectLayers.Add("Ghost", error));
		projectLayers.SetCollides(0, 1, false);

		auto fall = [](Scene& scene) {
			CreateGround(scene);
			Entity ghost = CreateBox(scene, { 0.0f, 2.0f, 0.0f });
			ghost.GetComponent<RigidBodyComponent>().Layer = "Ghost";
			scene.OnSimulationStart();
			Simulate(scene, 2.0f);
			scene.OnSimulationStop();
			return ghost.GetTransform().Translation.y;
		};
		Scene fromProject;
		CHECK(fall(fromProject) < -5.0f);

		// The override has no "Ghost" layer: the box warns once, falls back to Default and lands.
		Scene overridden;
		overridden.SetPhysicsLayers(PhysicsLayers());
		const uint64_t since = Log::GetHistory().GetTotalCount();
		CHECK(fall(overridden) == doctest::Approx(0.5f).epsilon(0.02));
		CHECK(CountMessages(since, "unknown physics layer 'Ghost'") == 1);
		const Ref<Scene> source = CreateRef<Scene>();
		source->SetPhysicsLayers(PhysicsLayers());
		CHECK(Scene::Copy(source)->GetPhysicsLayers().has_value());
	}

	TEST_CASE("The highest layer index filters like any other")
	{
		PhysicsLayers layers;
		std::string error;
		for (uint32_t i = 1; i < PhysicsLayers::MaxLayers; i++)
			REQUIRE(layers.Add("L" + std::to_string(i), error));
		layers.SetCollides(0, 31, false);
		Scene scene;
		scene.SetPhysicsLayers(layers);
		CreateGround(scene);
		Entity last = CreateBox(scene, { 0.0f, 2.0f, 0.0f });
		last.GetComponent<RigidBodyComponent>().Layer = "L31";
		Entity neighbour = CreateBox(scene, { 3.0f, 2.0f, 0.0f });
		neighbour.GetComponent<RigidBodyComponent>().Layer = "L30";

		scene.OnSimulationStart();
		Simulate(scene, 2.0f);
		CHECK(last.GetTransform().Translation.y < -5.0f);
		CHECK(neighbour.GetTransform().Translation.y == doctest::Approx(0.5f).epsilon(0.02));
		const PhysicsQueryFilter lastOnly = { .LayerMask = 1u << 31 };
		CHECK(scene.GetPhysicsWorld()->RaycastAll({ 0.0f, 50.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 100.0f, lastOnly).size() == 1);
		scene.OnSimulationStop();
	}

	TEST_CASE("A running world keeps the layers it started with")
	{
		BasaltTest::TempProject temp("PhysicsLayersSnapshot");
		REQUIRE(temp.IsValid());
		Scene scene;
		CreateGround(scene);
		scene.OnSimulationStart();
		std::string error;
		REQUIRE(Project::GetActive()->GetConfig().Physics.Add("Late", error));
		CHECK(scene.GetPhysicsWorld()->GetLayers().GetCount() == 1);
		scene.OnSimulationStop();
		scene.OnSimulationStart();
		CHECK(scene.GetPhysicsWorld()->GetLayers().GetCount() == 2);
		scene.OnSimulationStop();
	}

	TEST_CASE("Changing a body's layer at runtime rebuilds it on the new layer; warnings are logged once")
	{
		PhysicsLayers layers;
		std::string error;
		REQUIRE(layers.Add("Ghost", error));
		layers.SetCollides(0, 1, false);
		Scene scene;
		scene.SetPhysicsLayers(layers);
		CreateGround(scene);
		Entity box = CreateBox(scene, { 0.0f, 0.5f, 0.0f });
		box.GetComponent<RigidBodyComponent>().Layer = "Missing";

		const uint64_t since = Log::GetHistory().GetTotalCount();
		scene.OnSimulationStart();
		for (int i = 0; i < 5; i++)
		{
			box.AddOrReplaceComponent<RigidBodyComponent>(box.GetComponent<RigidBodyComponent>());
			Simulate(scene, Step);
		}
		CHECK(CountMessages(since, "unknown physics layer 'Missing'") == 1);
		Simulate(scene, 1.0f);
		CHECK(box.GetTransform().Translation.y == doctest::Approx(0.5f).epsilon(0.02));

		RigidBodyComponent rigidBody = box.GetComponent<RigidBodyComponent>();
		rigidBody.Layer = "Ghost";
		box.AddOrReplaceComponent<RigidBodyComponent>(rigidBody);
		Simulate(scene, 1.0f);
		CHECK(box.GetTransform().Translation.y < -3.0f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Continuous bodies do not tunnel through thin walls")
	{
		auto shoot = [](bool continuous) {
			Scene scene;
			Entity wall = scene.CreateEntity("Wall");
			wall.GetTransform().Translation = { 5.0f, 0.0f, 0.0f };
			wall.AddComponent<RigidBodyComponent>();
			wall.AddComponent<BoxColliderComponent>().HalfExtents = { 0.025f, 5.0f, 5.0f };
			Entity bullet = scene.CreateEntity("Bullet");
			auto& rigidBody = bullet.AddComponent<RigidBodyComponent>();
			rigidBody.Type = RigidBodyType::Dynamic;
			rigidBody.GravityFactor = 0.0f;
			rigidBody.LinearDamping = 0.0f;
			rigidBody.Continuous = continuous;
			bullet.AddComponent<SphereColliderComponent>().Radius = 0.05f;

			scene.OnSimulationStart();
			// 200 m/s moves 3.3 m per 60 Hz step, far more than the wall and bullet are thick.
			scene.GetPhysicsWorld()->SetLinearVelocity(bullet, { 200.0f, 0.0f, 0.0f });
			Simulate(scene, 0.25f);
			const float x = bullet.GetTransform().Translation.x;
			scene.OnSimulationStop();
			return x;
		};
		CHECK(shoot(false) > 10.0f);
		CHECK(shoot(true) < 5.0f);
	}

	TEST_CASE("Continuous on a non-dynamic body or a trigger is ignored with one warning")
	{
		Scene scene;
		Entity platform = CreateGround(scene);
		platform.GetComponent<RigidBodyComponent>().Continuous = true;
		Entity trigger = CreateBox(scene, { 0.0f, 3.0f, 0.0f });
		trigger.GetComponent<RigidBodyComponent>().IsTrigger = true;
		trigger.GetComponent<RigidBodyComponent>().Continuous = true;
		const uint64_t since = Log::GetHistory().GetTotalCount();
		scene.OnSimulationStart();
		Simulate(scene, 0.1f);
		CHECK(CountMessages(since, "Continuous only affects dynamic bodies") == 2);
		scene.OnSimulationStop();
	}

	TEST_CASE("Raycasts hit the closest non-trigger body")
	{
		Scene scene;
		Entity ground = CreateGround(scene);
		Entity trigger = scene.CreateEntity("Trigger");
		trigger.GetTransform().Translation = { 0.0f, 3.0f, 0.0f };
		trigger.AddComponent<RigidBodyComponent>().IsTrigger = true;
		trigger.AddComponent<BoxColliderComponent>();

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();

		const auto hit = physics.Raycast({ 0.0f, 10.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 100.0f);
		REQUIRE(hit.has_value());
		CHECK(hit->EntityID == ground.GetUUID());
		CHECK(hit->Distance == doctest::Approx(10.0f).epsilon(0.01));
		CHECK(hit->Normal.y == doctest::Approx(1.0f).epsilon(0.01));

		CHECK_FALSE(physics.Raycast({ 0.0f, 10.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 100.0f).has_value());
		CHECK_FALSE(physics.Raycast({ 0.0f, 10.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 5.0f).has_value());
		CHECK_FALSE(physics.Raycast({ 0.0f, 10.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 100.0f, { .IgnoreEntity = ground.GetUUID() }).has_value());
		scene.OnSimulationStop();
	}

	TEST_CASE("Shape queries: layer masks, all hits, casts, overlaps and filters")
	{
		PhysicsLayers layers;
		std::string error;
		REQUIRE(layers.Add("Enemy", error));
		Scene scene;
		scene.SetPhysicsLayers(layers);
		Entity ground = CreateGround(scene);
		auto addStatic = [&scene](const char* name, const glm::vec3& position) {
			Entity entity = scene.CreateEntity(name);
			entity.GetTransform().Translation = position;
			entity.AddComponent<RigidBodyComponent>();
			entity.AddComponent<BoxColliderComponent>();
			return entity;
		};
		Entity enemy = addStatic("Enemy", { 0.0f, 2.0f, 0.0f });
		enemy.GetComponent<RigidBodyComponent>().Layer = "Enemy";
		Entity trigger = addStatic("Trigger", { 0.0f, 5.0f, 0.0f });
		trigger.GetComponent<RigidBodyComponent>().IsTrigger = true;
		// Box and sphere colliders make one compound body with two sub-shapes.
		Entity compound = addStatic("Compound", { 5.0f, 2.0f, 0.0f });
		compound.AddComponent<SphereColliderComponent>().Radius = 0.6f;

		scene.OnSimulationStart();
		const PhysicsWorld& physics = *scene.GetPhysicsWorld();
		const glm::vec3 top = { 0.0f, 10.0f, 0.0f };
		const glm::vec3 down = { 0.0f, -2.0f, 0.0f };
		const uint32_t defaultOnly = *physics.GetLayers().MaskFromNames({ "Default" }, error);
		const uint32_t enemyOnly = *physics.GetLayers().MaskFromNames({ "Enemy" }, error);

		SUBCASE("Rays")
		{
			auto hit = physics.Raycast(top, down, 100.0f);
			REQUIRE(hit);
			CHECK(hit->EntityID == enemy.GetUUID());
			CHECK(hit->Distance == doctest::Approx(7.5f).epsilon(0.01));
			hit = physics.Raycast(top, down, 100.0f, { .LayerMask = defaultOnly });
			REQUIRE(hit);
			CHECK(hit->EntityID == ground.GetUUID());
			hit = physics.Raycast(top, down, 100.0f, { .IncludeTriggers = true });
			REQUIRE(hit);
			CHECK(hit->EntityID == trigger.GetUUID());

			auto hits = physics.RaycastAll(top, down, 100.0f, { .IncludeTriggers = true });
			REQUIRE(hits.size() == 3);
			CHECK(hits[0].EntityID == trigger.GetUUID());
			CHECK(hits[1].EntityID == enemy.GetUUID());
			CHECK(hits[2].EntityID == ground.GetUUID());
			CHECK(hits[2].Distance == doctest::Approx(10.0f).epsilon(0.01));
			CHECK(physics.RaycastAll(top, down, 100.0f, { .LayerMask = enemyOnly, .IgnoreEntity = enemy.GetUUID() }).empty());
			// A ray through both sub-shapes of the compound reports the entity once, at the first surface.
			hits = physics.RaycastAll({ 5.0f, 10.0f, 0.0f }, down, 100.0f, { .LayerMask = defaultOnly });
			REQUIRE(hits.size() == 2);
			CHECK(hits[0].EntityID == compound.GetUUID());
			CHECK(hits[0].Distance == doctest::Approx(7.4f).epsilon(0.01));
			CHECK(hits[1].EntityID == ground.GetUUID());
		}

		SUBCASE("Sphere and box casts")
		{
			auto hit = physics.SphereCast(top, 0.5f, down, 100.0f);
			REQUIRE(hit);
			CHECK(hit->EntityID == enemy.GetUUID());
			CHECK(hit->Distance == doctest::Approx(7.0f).epsilon(0.01));
			CHECK(hit->Normal.y == doctest::Approx(1.0f).epsilon(0.01));
			CHECK(hit->Point.y == doctest::Approx(2.5f).epsilon(0.01));

			// Starting inside a body hits it immediately, whichever way the cast moves (also out of the body,
			// which Jolt would skip as a back-face hit by default).
			for (const glm::vec3& direction : { down, -down, glm::vec3(1.0f, 0.0f, 0.0f) })
			{
				hit = physics.SphereCast({ 0.0f, 2.3f, 0.0f }, 0.1f, direction, 100.0f, { .LayerMask = enemyOnly });
				REQUIRE(hit);
				CHECK(hit->EntityID == enemy.GetUUID());
				CHECK(hit->Distance == doctest::Approx(0.0f));
				hit = physics.BoxCast({ 0.0f, 2.3f, 0.0f }, glm::vec3(0.1f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), direction, 100.0f, { .LayerMask = enemyOnly });
				REQUIRE(hit);
				CHECK(hit->EntityID == enemy.GetUUID());
			}

			const auto hits = physics.SphereCastAll(top, 0.5f, down, 100.0f);
			REQUIRE(hits.size() == 2);
			CHECK(hits[1].EntityID == ground.GetUUID());
			CHECK(hits[1].Distance == doctest::Approx(9.5f).epsilon(0.01));

			// A thin box lying flat reaches the enemy later than the same box stood upright by rotation.
			const glm::vec3 halfExtents = { 1.0f, 0.1f, 0.1f };
			hit = physics.BoxCast(top, halfExtents, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), down, 100.0f);
			REQUIRE(hit);
			CHECK(hit->Distance == doctest::Approx(7.4f).epsilon(0.01));
			hit = physics.BoxCast(top, halfExtents, glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f)), down, 100.0f);
			REQUIRE(hit);
			CHECK(hit->Distance == doctest::Approx(6.5f).epsilon(0.01));
			CHECK(physics.BoxCastAll(top, halfExtents, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), down, 100.0f, { .LayerMask = defaultOnly }).size() == 1);
		}

		SUBCASE("Overlaps")
		{
			CHECK(physics.OverlapSphere({ 5.0f, 2.0f, 0.0f }, 0.1f) == std::vector<UUID>{ compound.GetUUID() });
			std::vector<UUID> expected = { ground.GetUUID(), enemy.GetUUID() };
			std::ranges::sort(expected);
			CHECK(physics.OverlapSphere({ 0.0f, 0.0f, 0.0f }, 3.0f) == expected);
			CHECK(physics.OverlapSphere({ 0.0f, 0.0f, 0.0f }, 3.0f, { .LayerMask = enemyOnly }) == std::vector<UUID>{ enemy.GetUUID() });
			CHECK(physics.OverlapBox({ 0.0f, 5.0f, 0.0f }, glm::vec3(0.2f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)).empty());
			CHECK(physics.OverlapBox({ 0.0f, 5.0f, 0.0f }, glm::vec3(0.2f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), { .IncludeTriggers = true }) == std::vector<UUID>{ trigger.GetUUID() });
			// Rotated 45 degrees, a box beside the enemy reaches into it with its corner.
			const glm::vec3 beside = { 1.75f, 2.0f, 0.0f };
			CHECK(physics.OverlapBox(beside, glm::vec3(1.0f, 1.0f, 0.1f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), { .LayerMask = enemyOnly }).empty());
			CHECK(physics.OverlapBox(beside, glm::vec3(1.0f, 1.0f, 0.1f), glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 0.0f, 1.0f)), { .LayerMask = enemyOnly }).size() == 1);
		}

		SUBCASE("Invalid queries return nothing")
		{
			const float nan = std::numeric_limits<float>::quiet_NaN();
			CHECK_FALSE(physics.Raycast(top, { 0.0f, 0.0f, 0.0f }, 100.0f));
			CHECK_FALSE(physics.Raycast(top, down, -1.0f));
			CHECK_FALSE(physics.Raycast(top, down, std::numeric_limits<float>::infinity()));
			CHECK_FALSE(physics.Raycast({ nan, 0.0f, 0.0f }, down, 100.0f));
			CHECK_FALSE(physics.SphereCast(top, 0.0f, down, 100.0f));
			CHECK(physics.SphereCastAll(top, nan, down, 100.0f).empty());
			CHECK_FALSE(physics.BoxCast(top, { 1.0f, 0.0f, 1.0f }, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), down, 100.0f));
			CHECK_FALSE(physics.BoxCast(top, glm::vec3(1.0f), glm::quat(0.0f, 0.0f, 0.0f, 0.0f), down, 100.0f));
			CHECK(physics.OverlapSphere({ 0.0f, 0.0f, 0.0f }, -1.0f).empty());
			CHECK(physics.OverlapBox({ 0.0f, 0.0f, 0.0f }, glm::vec3(-1.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)).empty());
		}
		scene.OnSimulationStop();
	}

	TEST_CASE("Physics materials combine by the mode that ranks later")
	{
		using Mode = PhysicsCombineMode;
		// Default matches Jolt: geometric mean friction, maximum restitution.
		CHECK(CombineFriction(Mode::Default, 0.25f, Mode::Default, 1.0f) == doctest::Approx(0.5f));
		CHECK(CombineRestitution(Mode::Default, 0.25f, Mode::Default, 1.0f) == doctest::Approx(1.0f));
		// Default defers to the other body's mode.
		CHECK(CombineRestitution(Mode::Default, 0.2f, Mode::Min, 0.6f) == doctest::Approx(0.2f));
		CHECK(CombineFriction(Mode::GeometricMean, 0.25f, Mode::Default, 1.0f) == doctest::Approx(0.5f));
		CHECK(CombineFriction(Mode::Average, 0.2f, Mode::GeometricMean, 0.6f) == doctest::Approx(0.4f));
		CHECK(CombineFriction(Mode::Min, 0.2f, Mode::Average, 0.6f) == doctest::Approx(0.2f));
		CHECK(CombineFriction(Mode::Min, 0.5f, Mode::Multiply, 0.6f) == doctest::Approx(0.3f));
		CHECK(CombineRestitution(Mode::Multiply, 0.5f, Mode::Max, 0.6f) == doctest::Approx(0.6f));
		// The order of the two bodies does not matter.
		CHECK(CombineFriction(Mode::Max, 0.6f, Mode::Min, 0.2f) == doctest::Approx(0.6f));
		CHECK(CombineFriction(Mode::Default, 0.0f, Mode::Default, 1.0f) == 0.0f);
	}

	TEST_CASE("Friction and restitution combine modes change how bodies slide and bounce")
	{
		// A ball with restitution 0.8 dropped on ground with restitution 0: the default (the larger value)
		// bounces it, the ground's Min stops it dead.
		auto bounceHeight = [](PhysicsCombineMode groundMode) {
			Scene scene;
			Entity ground = CreateGround(scene);
			ground.GetComponent<RigidBodyComponent>().RestitutionCombine = groundMode;
			Entity ball = scene.CreateEntity("Ball");
			ball.GetTransform().Translation = { 0.0f, 3.0f, 0.0f };
			auto& body = ball.AddComponent<RigidBodyComponent>();
			body.Type = RigidBodyType::Dynamic;
			body.Restitution = 0.8f;
			body.LinearDamping = 0.0f;
			ball.AddComponent<SphereColliderComponent>();
			scene.OnSimulationStart();
			Simulate(scene, 1.0f); // lands after ~0.71 s
			float highest = 0.0f;
			for (int i = 0; i < 60; i++)
			{
				scene.OnUpdate(Step);
				highest = std::max(highest, ball.GetTransform().Translation.y);
			}
			scene.OnSimulationStop();
			return highest;
		};
		CHECK(bounceHeight(PhysicsCombineMode::Default) > 1.5f);
		CHECK(bounceHeight(PhysicsCombineMode::Min) < 0.6f);

		// A frictionless box sliding over ground with friction 1: the default geometric mean is 0, so it keeps
		// sliding; the ground's Max uses friction 1 and stops it.
		auto slideSpeed = [](PhysicsCombineMode groundMode) {
			Scene scene;
			Entity ground = CreateGround(scene);
			auto& groundBody = ground.GetComponent<RigidBodyComponent>();
			groundBody.Friction = 1.0f;
			groundBody.FrictionCombine = groundMode;
			Entity box = CreateBox(scene, { 0.0f, 0.5f, 0.0f });
			auto& body = box.GetComponent<RigidBodyComponent>();
			body.Friction = 0.0f;
			body.LinearDamping = 0.0f;
			scene.OnSimulationStart();
			scene.GetPhysicsWorld()->SetLinearVelocity(box, { 3.0f, 0.0f, 0.0f });
			Simulate(scene, 1.0f);
			const float speed = scene.GetPhysicsWorld()->GetLinearVelocity(box).x;
			scene.OnSimulationStop();
			return speed;
		};
		CHECK(slideSpeed(PhysicsCombineMode::Default) == doctest::Approx(3.0f).epsilon(0.02));
		CHECK(slideSpeed(PhysicsCombineMode::Max) < 0.05f);
	}

	TEST_CASE("Out-of-range friction and restitution are clamped with a warning")
	{
		// Restitution 2 combined with Max would add energy on every bounce; clamped to 1 the ball can at most
		// return to its drop height.
		Scene scene;
		Entity ground = CreateGround(scene);
		auto& groundBody = ground.GetComponent<RigidBodyComponent>();
		groundBody.RestitutionCombine = PhysicsCombineMode::Max;
		groundBody.Friction = -1.0f;
		Entity ball = scene.CreateEntity("Ball");
		ball.GetTransform().Translation = { 0.0f, 3.0f, 0.0f };
		auto& body = ball.AddComponent<RigidBodyComponent>();
		body.Type = RigidBodyType::Dynamic;
		body.Restitution = 2.0f;
		body.LinearDamping = 0.0f;
		ball.AddComponent<SphereColliderComponent>();

		const uint64_t since = Log::GetHistory().GetTotalCount();
		scene.OnSimulationStart();
		CHECK(CountMessages(since, "Restitution must be between 0 and 1") == 1);
		CHECK(CountMessages(since, "Friction must not be negative") == 1);
		float highest = 0.0f;
		for (int i = 0; i < 240; i++)
		{
			scene.OnUpdate(Step);
			if (i > 60)
				highest = std::max(highest, ball.GetTransform().Translation.y);
		}
		CHECK(highest < 3.25f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Velocities, impulses and teleporting through the transform")
	{
		Scene scene;
		Entity box = CreateWeightlessBox(scene, { 0.0f, 0.0f, 0.0f });
		box.GetComponent<RigidBodyComponent>().LinearDamping = 0.0f;

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		physics.SetLinearVelocity(box, { 2.0f, 0.0f, 0.0f });
		Simulate(scene, 1.0f);
		CHECK(box.GetTransform().Translation.x == doctest::Approx(2.0f).epsilon(0.02));

		// Mass 1: an impulse of 1 adds 1 m/s.
		physics.AddImpulse(box, { 0.0f, 0.0f, 1.0f });
		CHECK(physics.GetLinearVelocity(box).z == doctest::Approx(1.0f).epsilon(0.01));

		// Moving the entity teleports the body.
		box.GetTransform().Translation = { 100.0f, 0.0f, 0.0f };
		scene.OnUpdate(Step);
		CHECK(box.GetTransform().Translation.x == doctest::Approx(100.0f + 2.0f * Step).epsilon(0.01));
		scene.OnSimulationStop();
	}

	TEST_CASE("Kinematic bodies follow their transform and push dynamic bodies")
	{
		Scene scene;
		Entity pusher = scene.CreateEntity("Pusher");
		pusher.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		pusher.AddComponent<BoxColliderComponent>();
		Entity box = CreateWeightlessBox(scene, { 1.5f, 0.0f, 0.0f });

		scene.OnSimulationStart();
		for (int i = 0; i < 60; i++)
		{
			pusher.GetTransform().Translation.x += 2.0f * Step;
			scene.OnUpdate(Step);
		}
		CHECK(pusher.GetTransform().Translation.x == doctest::Approx(2.0f));
		CHECK(box.GetTransform().Translation.x > 2.5f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Bodies are created and removed when components change at runtime")
	{
		Scene scene;
		CreateGround(scene);
		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		CHECK(physics.GetBodyCount() == 1);

		Entity box = CreateBox(scene, { 0.0f, 3.0f, 0.0f });
		CHECK_FALSE(physics.HasBody(box));
		scene.OnUpdate(Step);
		CHECK(physics.HasBody(box));

		// Without a collider there is no body.
		Entity noCollider = scene.CreateEntity("NoCollider");
		noCollider.AddComponent<RigidBodyComponent>();
		scene.OnUpdate(Step);
		CHECK_FALSE(physics.HasBody(noCollider));

		box.RemoveComponent<BoxColliderComponent>();
		scene.OnUpdate(Step);
		CHECK_FALSE(physics.HasBody(box));

		Entity sphere = scene.CreateEntity("Sphere");
		sphere.AddComponent<RigidBodyComponent>();
		sphere.AddComponent<SphereColliderComponent>();
		sphere.AddComponent<CapsuleColliderComponent>();
		scene.OnUpdate(Step);
		CHECK(physics.HasBody(sphere));
		scene.DestroyEntity(sphere);
		CHECK(physics.GetBodyCount() == 1);
		scene.OnSimulationStop();
	}

	TEST_CASE("Simulation is deterministic")
	{
		auto run = []() {
			Scene scene;
			CreateGround(scene);
			std::vector<Entity> boxes;
			boxes.reserve(5);
			for (int i = 0; i < 5; i++)
				boxes.push_back(CreateBox(scene, { 0.3f * i, 2.0f + 1.1f * i, 0.1f * i }));
			scene.OnSimulationStart();
			Simulate(scene, 2.0f);
			std::vector<glm::vec3> positions;
			positions.reserve(boxes.size());
			for (Entity box : boxes)
				positions.push_back(box.GetTransform().Translation);
			scene.OnSimulationStop();
			return positions;
		};
		CHECK(run() == run());
	}

	TEST_CASE("Hinge joints swing around their axis and respect limits")
	{
		Scene scene;
		// A pendulum: the box's centre is 1 m from the hinge, which is pinned to the world at the origin.
		Entity pendulum = CreateBox(scene, { 1.0f, 0.0f, 0.0f });
		auto& hinge = pendulum.AddComponent<JointComponent>();
		hinge.Type = JointType::Hinge;
		hinge.Anchor = { -1.0f, 0.0f, 0.0f };
		hinge.Axis = { 0.0f, 0.0f, 1.0f };

		Entity limited = CreateBox(scene, { 11.0f, 0.0f, 0.0f });
		auto& limitedHinge = limited.AddComponent<JointComponent>(hinge);
		limitedHinge.UseLimits = true;
		limitedHinge.LimitMin = -30.0f;
		limitedHinge.LimitMax = 30.0f;

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		REQUIRE(physics.HasJoint(pendulum));
		REQUIRE(physics.HasJoint(limited));
		CHECK(physics.GetJointPosition(pendulum).value() == doctest::Approx(0.0f).epsilon(0.001));

		float lowest = 0.0f;
		float maxLimitedAngle = 0.0f;
		for (int i = 0; i < 120; i++)
		{
			scene.OnUpdate(Step);
			const glm::vec3 position = pendulum.GetTransform().Translation;
			CHECK(glm::length(position) == doctest::Approx(1.0f).epsilon(0.02));
			CHECK(position.z == doctest::Approx(0.0f).epsilon(0.001));
			lowest = std::min(lowest, position.y);
			maxLimitedAngle = std::max(maxLimitedAngle, std::abs(physics.GetJointPosition(limited).value()));
		}
		CHECK(lowest < -0.95f);
		CHECK(maxLimitedAngle == doctest::Approx(30.0f).epsilon(0.05));
		CHECK(std::abs(physics.GetJointPosition(pendulum).value()) > 45.0f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Slider joints move only along their axis")
	{
		Scene scene;
		Entity slider = CreateBox(scene, { 0.0f, 0.0f, 0.0f });
		auto& joint = slider.AddComponent<JointComponent>();
		joint.Type = JointType::Slider;
		joint.Axis = { 1.0f, 0.0f, 0.0f };
		joint.UseLimits = true;
		joint.LimitMin = -1.0f;
		joint.LimitMax = 2.0f;

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		// Gravity pulls down and the push has a sideways part; only the X part may move the body.
		physics.SetLinearVelocity(slider, { 1.0f, 0.0f, 1.0f });
		Simulate(scene, 1.0f);
		const glm::vec3 position = slider.GetTransform().Translation;
		CHECK(position.x == doctest::Approx(1.0f).epsilon(0.05));
		CHECK(std::abs(position.y) < 0.01f);
		CHECK(std::abs(position.z) < 0.01f);
		CHECK(physics.GetJointPosition(slider).value() == doctest::Approx(1.0f).epsilon(0.05));

		physics.SetLinearVelocity(slider, { 5.0f, 0.0f, 0.0f });
		Simulate(scene, 1.0f);
		CHECK(slider.GetTransform().Translation.x == doctest::Approx(2.0f).epsilon(0.02));
		scene.OnSimulationStop();
	}

	TEST_CASE("Point, distance and fixed joints hold their bodies together")
	{
		Scene scene;
		Entity ceiling = CreateGround(scene);
		ceiling.GetTransform().Translation = { 0.0f, 10.0f, 0.0f };

		// Ball-and-socket hanging from the world at (0, 5, 0), starting sideways.
		Entity ball = CreateBox(scene, { 2.0f, 5.0f, 0.0f });
		auto& point = ball.AddComponent<JointComponent>();
		point.Type = JointType::Point;
		point.Anchor = { -2.0f, 0.0f, 0.0f };

		// Rope of length 3 below the static ceiling entity; starts slack, 1 m below its anchor.
		Entity weight = CreateBox(scene, { 10.0f, 8.0f, 0.0f });
		auto& rope = weight.AddComponent<JointComponent>();
		rope.Type = JointType::Distance;
		rope.ConnectedEntity = ceiling.GetUUID();
		rope.ConnectedAnchor = { 10.0f, -1.0f, 0.0f }; // ceiling local space: world (10, 9, 0)
		rope.UseLimits = true;
		rope.LimitMin = 0.0f;
		rope.LimitMax = 3.0f;

		// Two weightless boxes welded 2 m apart; only the first one is pushed.
		Entity first = CreateBox(scene, { -10.0f, 0.0f, 0.0f });
		Entity second = CreateBox(scene, { -10.0f, 0.0f, 2.0f });
		first.GetComponent<RigidBodyComponent>().GravityFactor = 0.0f;
		second.GetComponent<RigidBodyComponent>().GravityFactor = 0.0f;
		auto& weld = second.AddComponent<JointComponent>();
		weld.Type = JointType::Fixed;
		weld.ConnectedEntity = first.GetUUID();

		scene.OnSimulationStart();
		scene.GetPhysicsWorld()->SetLinearVelocity(first, { 0.0f, 1.0f, 0.0f });
		for (int i = 0; i < 120; i++)
		{
			scene.OnUpdate(Step);
			CHECK(glm::distance(ball.GetTransform().Translation, glm::vec3(0.0f, 5.0f, 0.0f)) == doctest::Approx(2.0f).epsilon(0.02));
			// The rope may stretch briefly when it snaps taut; Jolt corrects position error over a few steps.
			CHECK(glm::distance(weight.GetTransform().Translation, glm::vec3(10.0f, 9.0f, 0.0f)) <= 3.15f);
			CHECK(glm::distance(first.GetTransform().Translation, second.GetTransform().Translation) == doctest::Approx(2.0f).epsilon(0.01));
		}
		CHECK(weight.GetTransform().Translation.y == doctest::Approx(6.0f).epsilon(0.02));
		// The push is off-centre for the welded pair, so it tumbles; its centre still rises at about 0.5 m/s.
		CHECK((first.GetTransform().Translation.y + second.GetTransform().Translation.y) * 0.5f == doctest::Approx(0.95f).epsilon(0.1));
		CHECK(scene.GetPhysicsWorld()->GetJointPosition(ball) == std::nullopt);
		scene.OnSimulationStop();
	}

	TEST_CASE("Joints follow body rebuilds, component changes and destruction")
	{
		Scene scene;
		Entity frame = CreateBox(scene, { 0.0f, 0.0f, 0.0f });
		frame.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		Entity door = CreateBox(scene, { 1.0f, 0.0f, 0.0f });
		door.AddComponent<JointComponent>().ConnectedEntity = frame.GetUUID();
		door.GetComponent<JointComponent>().Anchor = { -0.5f, 0.0f, 0.0f };

		// Joints on static-only pairs, without a body, or to missing entities are skipped with a warning.
		Entity wall = CreateGround(scene);
		wall.GetTransform().Translation.y = -50.0f;
		wall.AddComponent<JointComponent>().ConnectedEntity = frame.GetUUID();
		Entity noBody = scene.CreateEntity("NoBody");
		noBody.AddComponent<JointComponent>();
		Entity dangling = CreateBox(scene, { 5.0f, 0.0f, 0.0f });
		dangling.AddComponent<JointComponent>().ConnectedEntity = 12345;

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		CHECK(physics.HasJoint(door));
		CHECK_FALSE(physics.HasJoint(wall));
		CHECK_FALSE(physics.HasJoint(noBody));
		CHECK_FALSE(physics.HasJoint(dangling));

		// Rebuilding either body rebuilds the joint.
		door.AddOrReplaceComponent<BoxColliderComponent>(BoxColliderComponent{ { 0.4f, 0.4f, 0.4f } });
		scene.OnUpdate(Step);
		CHECK(physics.HasJoint(door));
		frame.AddOrReplaceComponent<BoxColliderComponent>(BoxColliderComponent{ { 0.4f, 0.4f, 0.4f } });
		scene.OnUpdate(Step);
		CHECK(physics.HasJoint(door));

		door.RemoveComponent<JointComponent>();
		CHECK_FALSE(physics.HasJoint(door));
		door.AddComponent<JointComponent>().ConnectedEntity = frame.GetUUID();
		scene.OnUpdate(Step);
		CHECK(physics.HasJoint(door));

		// Joints skipped for a missing body are built once that body exists.
		noBody.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		noBody.AddComponent<BoxColliderComponent>();
		scene.OnUpdate(Step);
		CHECK(physics.HasJoint(noBody));
		Entity late = scene.CreateEntityWithUUID(12345, "Late");
		late.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		late.AddComponent<BoxColliderComponent>();
		scene.OnUpdate(Step);
		CHECK(physics.HasJoint(dangling));

		// Destroying the joint's own entity removes its constraint; the connected body is unaffected.
		scene.DestroyEntity(dangling);
		Simulate(scene, 0.1f);
		CHECK(physics.HasBody(late));

		// Destroying the connected entity removes the joint before its body; the door falls.
		scene.DestroyEntity(frame);
		CHECK_FALSE(physics.HasJoint(door));
		Simulate(scene, 0.5f);
		CHECK_FALSE(physics.HasJoint(door));
		CHECK(door.GetTransform().Translation.y < -0.5f);
		scene.OnSimulationStop();
	}

	TEST_CASE("A joint to an entity that gets its body during play is built then")
	{
		// Regression: the joint was dropped for good when its connected body did not exist yet.
		Scene scene;
		Entity frame = scene.CreateEntity("Frame");
		Entity door = CreateBox(scene, { 1.0f, 0.0f, 0.0f });
		auto& hinge = door.AddComponent<JointComponent>();
		hinge.ConnectedEntity = frame.GetUUID();
		hinge.Anchor = { -0.5f, 0.0f, 0.0f };

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		CHECK_FALSE(physics.HasJoint(door));
		frame.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		frame.AddComponent<BoxColliderComponent>().HalfExtents = { 0.1f, 0.1f, 0.1f };
		scene.OnUpdate(Step);
		CHECK(physics.HasJoint(door));
		Simulate(scene, 0.5f);
		// The vertical hinge holds the door up.
		CHECK(std::abs(door.GetTransform().Translation.y) < 0.05f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Joint warnings are logged once, not on every update")
	{
		Scene scene;
		Entity ball = CreateBox(scene, { 0.0f, 0.0f, 0.0f });
		auto& point = ball.AddComponent<JointComponent>();
		point.Type = JointType::Point;
		point.MotorMode = JointMotorMode::Velocity;
		Entity orphan = CreateBox(scene, { 5.0f, 0.0f, 0.0f });
		orphan.AddComponent<JointComponent>().ConnectedEntity = 999;

		const uint64_t before = Log::GetHistory().GetTotalCount();
		scene.OnSimulationStart();
		for (int i = 0; i < 30; i++)
		{
			// Scripts set joints every frame: a changing motor target and an unchanged broken joint.
			SetJoint(ball, [i](JointComponent& joint) { joint.MotorTarget = static_cast<float>(i); });
			SetJoint(orphan, [](JointComponent&) {});
			scene.OnUpdate(Step);
		}
		CHECK(scene.GetPhysicsWorld()->HasJoint(ball));
		CHECK(CountMessages(before, "MotorMode has no effect on point joints") == 1);
		CHECK(CountMessages(before, "missing entity 999") == 1);

		// A different problem is reported again.
		SetJoint(orphan, [](JointComponent& joint) { joint.ConnectedEntity = 998; });
		scene.OnUpdate(Step);
		CHECK(CountMessages(before, "missing entity 998") == 1);
		scene.OnSimulationStop();
	}

	TEST_CASE("Invalid joint limits are clamped with a warning, at creation and during play")
	{
		Scene scene;
		// Pendulums pinned to the world at the origin, swinging down (negative angles) under gravity.
		auto pendulum = [&](float x, float min, float max) {
			Entity box = CreateBox(scene, { x + 1.0f, 0.0f, 0.0f });
			auto& hinge = box.AddComponent<JointComponent>();
			hinge.Anchor = { -1.0f, 0.0f, 0.0f };
			hinge.Axis = { 0.0f, 0.0f, 1.0f };
			hinge.UseLimits = true;
			hinge.LimitMin = min;
			hinge.LimitMax = max;
			return box;
		};
		Entity inverted = pendulum(0.0f, 30.0f, -30.0f);      // -> [0, 0]
		Entity excludesRest = pendulum(10.0f, 10.0f, 400.0f); // -> [0, 180]

		Entity slider = CreateWeightlessBox(scene, { 20.0f, 0.0f, 0.0f });
		auto& rail = slider.AddComponent<JointComponent>();
		rail.Type = JointType::Slider;
		rail.Axis = { 1.0f, 0.0f, 0.0f };
		rail.UseLimits = true;
		rail.LimitMin = 1.0f; // -> 0
		rail.LimitMax = 2.0f;

		// A rope whose LimitMax is below LimitMin becomes a rigid rod of length LimitMin.
		Entity weight = CreateWeightlessBox(scene, { 30.0f, -1.0f, 0.0f });
		auto& rope = weight.AddComponent<JointComponent>();
		rope.Type = JointType::Distance;
		rope.ConnectedAnchor = { 30.0f, 0.0f, 0.0f };
		rope.UseLimits = true;
		rope.LimitMin = 3.0f;
		rope.LimitMax = 1.0f;

		const uint64_t before = Log::GetHistory().GetTotalCount();
		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		REQUIRE(physics.HasJoint(inverted));
		REQUIRE(physics.HasJoint(excludesRest));
		REQUIRE(physics.HasJoint(slider));
		REQUIRE(physics.HasJoint(weight));
		CHECK(CountMessages(before, "must satisfy -180") == 2);
		CHECK(CountMessages(before, "must satisfy LimitMin <= 0") == 1);
		CHECK(CountMessages(before, "must satisfy 0 <= LimitMin") == 1);

		physics.SetLinearVelocity(slider, { -2.0f, 0.0f, 0.0f });
		Simulate(scene, 1.0f);
		CHECK(std::abs(physics.GetJointPosition(inverted).value()) < 2.0f);
		CHECK(physics.GetJointPosition(excludesRest).value() > -2.0f);
		CHECK(physics.GetJointPosition(slider).value() > -0.05f);
		CHECK(glm::distance(weight.GetTransform().Translation, glm::vec3(30.0f, 0.0f, 0.0f)) == doctest::Approx(3.0f).epsilon(0.03));

		// Updating limits in play clamps and warns the same way; a zero-length rope is called out.
		const uint64_t during = Log::GetHistory().GetTotalCount();
		SetJoint(inverted, [](JointComponent& joint) { joint.LimitMax = -10.0f; });
		SetJoint(weight, [](JointComponent& joint) {
			joint.LimitMin = 0.0f;
			joint.LimitMax = 0.0f;
		});
		scene.OnUpdate(Step);
		CHECK(physics.HasJoint(inverted));
		CHECK(CountMessages(during, "must satisfy -180") == 1);
		CHECK(CountMessages(during, "pull the anchors together") == 1);
		scene.OnSimulationStop();
	}

	TEST_CASE("Joint settings changed during play take effect")
	{
		Scene scene;
		// Slider limits narrow in place; the rest pose stays where it was.
		Entity slider = CreateWeightlessBox(scene, { 0.0f, 0.0f, 0.0f });
		auto& rail = slider.AddComponent<JointComponent>();
		rail.Type = JointType::Slider;
		rail.Axis = { 1.0f, 0.0f, 0.0f };
		rail.UseLimits = true;
		rail.LimitMin = -1.0f;
		rail.LimitMax = 2.0f;

		// A pendulum that gets limits (a structural change) once it has swung.
		Entity pendulum = CreateBox(scene, { 11.0f, 0.0f, 0.0f });
		auto& hinge = pendulum.AddComponent<JointComponent>();
		hinge.Anchor = { -1.0f, 0.0f, 0.0f };
		hinge.Axis = { 0.0f, 0.0f, 1.0f };

		// A box held up by a point joint until it is given a break force.
		Entity hanging = CreateBox(scene, { 20.0f, 0.0f, 0.0f });
		hanging.AddComponent<JointComponent>().Type = JointType::Point;

		// A box resting on a plate through a vertical slider that collides with it, until it does not.
		Entity plate = CreateGround(scene);
		plate.GetTransform().Translation = { 40.0f, -0.5f, 0.0f };
		plate.GetComponent<BoxColliderComponent>().HalfExtents = { 2.0f, 0.5f, 2.0f };
		Entity resting = CreateBox(scene, { 40.0f, 0.5f, 0.0f });
		auto& guide = resting.AddComponent<JointComponent>();
		guide.Type = JointType::Slider;
		guide.ConnectedEntity = plate.GetUUID();
		guide.EnableCollision = true;

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		physics.SetLinearVelocity(slider, { 1.0f, 0.0f, 0.0f });
		Simulate(scene, 0.25f);
		SetJoint(slider, [](JointComponent& joint) { joint.LimitMax = 0.5f; });
		Simulate(scene, 1.0f);
		CHECK(physics.GetJointPosition(slider).value() == doctest::Approx(0.5f).epsilon(0.05));
		CHECK(slider.GetTransform().Translation.x == doctest::Approx(0.5f).epsilon(0.05));

		const float swung = physics.GetJointPosition(pendulum).value();
		CHECK(swung < -20.0f);
		SetJoint(pendulum, [](JointComponent& joint) {
			joint.UseLimits = true;
			joint.LimitMin = -5.0f;
			joint.LimitMax = 5.0f;
		});
		scene.OnUpdate(Step);
		// Rebuilt from the current pose, which is the new rest pose.
		CHECK(std::abs(physics.GetJointPosition(pendulum).value()) < 2.0f);

		CHECK(hanging.HasComponent<JointComponent>());
		SetJoint(hanging, [](JointComponent& joint) { joint.BreakForce = 5.0f; });
		CHECK(resting.GetTransform().Translation.y == doctest::Approx(0.5f).epsilon(0.02));
		SetJoint(resting, [](JointComponent& joint) { joint.EnableCollision = false; });
		Simulate(scene, 1.0f);
		CHECK_FALSE(hanging.HasComponent<JointComponent>());
		CHECK(resting.GetTransform().Translation.y < -2.0f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Hinge and slider joints break on limit and motor effort")
	{
		Scene scene;
		// A pendulum locked level by [0, 0] limits: the limit carries about 9.8 N·m of gravity torque.
		Entity locked = CreateBox(scene, { 1.0f, 0.0f, 0.0f });
		auto& hinge = locked.AddComponent<JointComponent>();
		hinge.Anchor = { -1.0f, 0.0f, 0.0f };
		hinge.Axis = { 0.0f, 0.0f, 1.0f };
		hinge.UseLimits = true;
		hinge.BreakTorque = 5.0f;

		// A box resting on the lower end of a vertical slider: the limit carries its 9.8 N weight.
		Entity rail = CreateBox(scene, { 10.0f, 0.0f, 0.0f });
		auto& slider = rail.AddComponent<JointComponent>();
		slider.Type = JointType::Slider;
		slider.UseLimits = true;
		slider.LimitMax = 1.0f;
		slider.BreakForce = 5.0f;
		Entity strongRail = CreateBox(scene, { 12.0f, 0.0f, 0.0f });
		strongRail.AddComponent<JointComponent>(slider).BreakForce = 50.0f;

		// A piston motor pushing into a static block stalls at its force limit, above BreakForce.
		Entity piston = CreateWeightlessBox(scene, { 20.0f, 0.0f, 0.0f });
		auto& motor = piston.AddComponent<JointComponent>();
		motor.Type = JointType::Slider;
		motor.MotorMode = JointMotorMode::Velocity;
		// Reaching 0.5 m/s within one step takes 30 N, below the break force.
		motor.MotorTarget = 0.5f;
		motor.MotorMaxForce = 200.0f;
		motor.BreakForce = 50.0f;
		Entity block = CreateBox(scene, { 20.0f, 1.1f, 0.0f });
		block.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		// The same piston with nothing in its way only overcomes damping.
		Entity freePiston = CreateWeightlessBox(scene, { 30.0f, 0.0f, 0.0f });
		freePiston.AddComponent<JointComponent>(motor);

		scene.OnSimulationStart();
		Simulate(scene, 1.0f);
		CHECK_FALSE(locked.HasComponent<JointComponent>());
		CHECK_FALSE(rail.HasComponent<JointComponent>());
		CHECK(strongRail.HasComponent<JointComponent>());
		CHECK_FALSE(piston.HasComponent<JointComponent>());
		CHECK(freePiston.HasComponent<JointComponent>());
		scene.OnSimulationStop();
	}

	TEST_CASE("Bodies collide again once a non-colliding joint between them breaks")
	{
		Scene scene;
		Entity plate = CreateGround(scene);
		// Resting on the plate, joined by a point joint too weak to carry the box.
		Entity box = CreateBox(scene, { 0.0f, 0.5f, 0.0f });
		auto& joint = box.AddComponent<JointComponent>();
		joint.Type = JointType::Point;
		joint.ConnectedEntity = plate.GetUUID();
		joint.BreakForce = 5.0f;

		scene.OnSimulationStart();
		Simulate(scene, 1.0f);
		CHECK_FALSE(box.HasComponent<JointComponent>());
		CHECK(box.GetTransform().Translation.y == doctest::Approx(0.5f).epsilon(0.02));
		scene.OnSimulationStop();
	}

	TEST_CASE("Hinge and slider axes ignore non-uniform scale")
	{
		Scene scene;
		// A diagonal slider axis on a stretched box: a scaled matrix would bend it toward X.
		Entity box = CreateBox(scene, { 0.0f, 0.0f, 0.0f });
		box.GetTransform().Scale = { 4.0f, 1.0f, 1.0f };
		box.GetComponent<RigidBodyComponent>().GravityFactor = 0.0f;
		auto& joint = box.AddComponent<JointComponent>();
		joint.Type = JointType::Slider;
		joint.Axis = { 1.0f, 1.0f, 0.0f };

		scene.OnSimulationStart();
		scene.GetPhysicsWorld()->SetLinearVelocity(box, { 1.0f, 1.0f, 0.0f });
		Simulate(scene, 1.0f);
		const glm::vec3 position = box.GetTransform().Translation;
		CHECK(position.x > 0.5f);
		CHECK(position.x == doctest::Approx(position.y).epsilon(0.01));
		scene.OnSimulationStop();
	}

	TEST_CASE("Simulation with joints is deterministic")
	{
		auto run = []() {
			Scene scene;
			Entity previous;
			std::vector<Entity> links;
			for (int i = 0; i < 6; i++)
			{
				Entity link = CreateBox(scene, { 1.2f * static_cast<float>(i + 1), 0.0f, 0.0f });
				auto& joint = link.AddComponent<JointComponent>();
				joint.Type = JointType::Point;
				joint.Anchor = { -0.6f, 0.0f, 0.0f };
				joint.ConnectedEntity = previous ? previous.GetUUID() : UUID(0);
				links.push_back(link);
				previous = link;
			}
			scene.OnSimulationStart();
			Simulate(scene, 2.0f);
			std::vector<glm::vec3> positions;
			positions.reserve(links.size());
			for (Entity link : links)
				positions.push_back(link.GetTransform().Translation);
			scene.OnSimulationStop();
			return positions;
		};
		CHECK(run() == run());
	}

	TEST_CASE("Joint motors drive hinges and sliders and update without a rebuild")
	{
		Scene scene;
		Entity wheel = CreateWeightlessBox(scene, { 0.0f, 0.0f, 0.0f });
		wheel.GetComponent<RigidBodyComponent>().AngularDamping = 0.0f;
		auto& hinge = wheel.AddComponent<JointComponent>();
		hinge.Axis = { 0.0f, 0.0f, 1.0f };
		hinge.MotorMode = JointMotorMode::Velocity;
		hinge.MotorTarget = 90.0f;

		Entity arm = CreateWeightlessBox(scene, { 5.0f, 0.0f, 0.0f });
		auto& servo = arm.AddComponent<JointComponent>();
		servo.Axis = { 0.0f, 0.0f, 1.0f };
		servo.MotorMode = JointMotorMode::Position;
		servo.MotorTarget = 45.0f;

		Entity piston = CreateBox(scene, { 10.0f, 0.0f, 0.0f });
		auto& slider = piston.AddComponent<JointComponent>();
		slider.Type = JointType::Slider;
		slider.Axis = { 0.0f, 1.0f, 0.0f };
		slider.MotorMode = JointMotorMode::Position;
		slider.MotorTarget = 1.5f;

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		Simulate(scene, 0.5f);
		CHECK(physics.GetJointPosition(wheel).value() == doctest::Approx(45.0f).epsilon(0.05));
		Simulate(scene, 2.5f);
		CHECK(physics.GetJointPosition(arm).value() == doctest::Approx(45.0f).epsilon(0.02));
		// The position motor holds the piston up against gravity.
		CHECK(piston.GetTransform().Translation.y == doctest::Approx(1.5f).epsilon(0.05));

		// Changing the target through the registry (as SetComponent does) keeps the original rest pose:
		// a rebuilt joint would measure from the current 45 degrees instead.
		JointComponent retarget = arm.GetComponent<JointComponent>();
		retarget.MotorTarget = -30.0f;
		arm.AddOrReplaceComponent<JointComponent>(retarget);
		Simulate(scene, 3.0f);
		CHECK(physics.GetJointPosition(arm).value() == doctest::Approx(-30.0f).epsilon(0.02));
		CHECK(glm::degrees(arm.GetTransform().GetRotationEuler().z) == doctest::Approx(-30.0f).epsilon(0.02));

		// Turning the motor off lets gravity take the piston back down.
		JointComponent off = piston.GetComponent<JointComponent>();
		off.MotorMode = JointMotorMode::Off;
		piston.AddOrReplaceComponent<JointComponent>(off);
		Simulate(scene, 0.5f);
		CHECK(piston.GetTransform().Translation.y < 0.5f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Joints break above their break force and are removed")
	{
		Scene scene;
		// A 1 kg box hanging from the world needs about 9.8 N to hold.
		Entity weak = CreateBox(scene, { 0.0f, 0.0f, 0.0f });
		auto& weakJoint = weak.AddComponent<JointComponent>();
		weakJoint.Type = JointType::Point;
		weakJoint.BreakForce = 5.0f;

		Entity strong = CreateBox(scene, { 5.0f, 0.0f, 0.0f });
		strong.AddComponent<JointComponent>(weakJoint).BreakForce = 50.0f;

		// A welded box that is twisted hard breaks on torque.
		Entity twisted = CreateWeightlessBox(scene, { 10.0f, 0.0f, 0.0f });
		auto& weld = twisted.AddComponent<JointComponent>();
		weld.Type = JointType::Fixed;
		weld.BreakTorque = 10.0f;

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		physics.SetAngularVelocity(twisted, { 0.0f, 20.0f, 0.0f });
		Simulate(scene, 1.0f);
		CHECK_FALSE(weak.HasComponent<JointComponent>());
		CHECK_FALSE(physics.HasJoint(weak));
		CHECK(weak.GetTransform().Translation.y < -3.0f);
		CHECK(strong.HasComponent<JointComponent>());
		CHECK(strong.GetTransform().Translation.y == doctest::Approx(0.0f).epsilon(0.01));
		CHECK_FALSE(twisted.HasComponent<JointComponent>());
		scene.OnSimulationStop();
	}

	TEST_CASE("Jointed bodies collide with each other only when EnableCollision is set")
	{
		// The filter applies to the two bodies whether the joint is on the box or on a joint entity.
		auto settle = [](bool enableCollision, bool onJointEntity) {
			Scene scene;
			Entity plate = CreateGround(scene);
			// A box resting on the plate, joined to it by a vertical slider: without collision it slides through.
			Entity box = CreateBox(scene, { 0.0f, 0.5f, 0.0f });
			Entity holder = onJointEntity ? scene.CreateChildEntity(box, "Slider") : box;
			auto& joint = holder.AddComponent<JointComponent>();
			joint.Type = JointType::Slider;
			joint.BodyEntity = onJointEntity ? box.GetUUID() : UUID(0);
			joint.ConnectedEntity = plate.GetUUID();
			joint.EnableCollision = enableCollision;
			scene.OnSimulationStart();
			Simulate(scene, 1.0f);
			const float y = box.GetTransform().Translation.y;
			scene.OnSimulationStop();
			return y;
		};
		for (bool onJointEntity : { false, true })
		{
			INFO("on a joint entity: " << onJointEntity);
			CHECK(settle(true, onJointEntity) == doctest::Approx(0.5f).epsilon(0.02));
			CHECK(settle(false, onJointEntity) < -2.0f);
		}
	}

	TEST_CASE("Spring-softened limits let a slider overshoot and pull it back")
	{
		// Peak and final offset of a weightless slider thrown against its +1 m limit.
		auto run = [](float frequency) {
			Scene scene;
			Entity box = CreateWeightlessBox(scene, { 0.0f, 0.0f, 0.0f });
			auto& joint = box.AddComponent<JointComponent>();
			joint.Type = JointType::Slider;
			joint.Axis = { 1.0f, 0.0f, 0.0f };
			joint.UseLimits = true;
			joint.LimitMin = -1.0f;
			joint.LimitMax = 1.0f;
			joint.LimitSpringFrequency = frequency;
			joint.LimitSpringDamping = 0.2f;
			scene.OnSimulationStart();
			scene.GetPhysicsWorld()->SetLinearVelocity(box, { 5.0f, 0.0f, 0.0f });
			float peak = 0.0f;
			for (int i = 0; i < 30; i++)
			{
				scene.OnUpdate(Step);
				peak = std::max(peak, box.GetTransform().Translation.x);
			}
			const float after = box.GetTransform().Translation.x;
			scene.OnSimulationStop();
			return std::pair(peak, after);
		};
		const auto [rigidPeak, rigidAfter] = run(0.0f);
		CHECK(rigidPeak < 1.05f);
		const auto [softPeak, softAfter] = run(2.0f);
		CHECK(softPeak > 1.2f);
		// Half a second later the spring has thrown it back inside the limits.
		CHECK(softAfter < 1.0f);
	}

	TEST_CASE("A spring on a distance joint without limits makes a bungee")
	{
		// Length of a 2 m rope holding a 1 kg box after it settles.
		auto settle = [](float frequency) {
			Scene scene;
			Entity box = CreateBox(scene, { 0.0f, 0.0f, 0.0f });
			auto& joint = box.AddComponent<JointComponent>();
			joint.Type = JointType::Distance;
			joint.ConnectedAnchor = { 0.0f, 2.0f, 0.0f };
			joint.LimitSpringFrequency = frequency;
			scene.OnSimulationStart();
			Simulate(scene, 4.0f);
			const float length = 2.0f - box.GetTransform().Translation.y;
			scene.OnSimulationStop();
			return length;
		};
		CHECK(settle(0.0f) == doctest::Approx(2.0f).epsilon(0.01));
		// A 1 Hz spring stretches by g / (2 pi f)^2 = 0.25 m under its load.
		CHECK(settle(1.0f) == doctest::Approx(2.25f).epsilon(0.02));
	}

	TEST_CASE("Limit springs update in place and warn when they have no effect")
	{
		Scene scene;
		// A pendulum resting against its -30 degree limit.
		Entity pendulum = CreateBox(scene, { 1.0f, 0.0f, 0.0f });
		auto& hinge = pendulum.AddComponent<JointComponent>();
		hinge.Type = JointType::Hinge;
		hinge.Anchor = { -1.0f, 0.0f, 0.0f };
		hinge.Axis = { 0.0f, 0.0f, 1.0f };
		hinge.UseLimits = true;
		hinge.LimitMin = -30.0f;
		hinge.LimitMax = 30.0f;

		Entity ball = CreateBox(scene, { 5.0f, 0.0f, 0.0f });
		auto& point = ball.AddComponent<JointComponent>();
		point.Type = JointType::Point;
		point.LimitSpringFrequency = 2.0f;
		Entity door = CreateBox(scene, { 10.0f, 0.0f, 0.0f });
		auto& free = door.AddComponent<JointComponent>();
		free.LimitSpringFrequency = 2.0f;
		free.LimitSpringDamping = -1.0f;

		const uint64_t before = Log::GetHistory().GetTotalCount();
		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		Simulate(scene, 2.0f);
		CHECK(physics.GetJointPosition(pendulum).value() == doctest::Approx(-30.0f).epsilon(0.05));
		CHECK(CountMessages(before, "no effect on point joints") == 1);
		CHECK(CountMessages(before, "LimitSpringFrequency, LimitSpringDamping have no effect without UseLimits") == 1);
		CHECK(CountMessages(before, "LimitSpringDamping -1 is negative") == 1);

		// Softening the limit keeps the rest pose (a rebuild would measure from the current angle): the
		// pendulum sinks past -30 degrees and its angle is still measured from where it started.
		SetJoint(pendulum, [](JointComponent& joint) { joint.LimitSpringFrequency = 1.0f; });
		scene.OnUpdate(Step);
		CHECK(physics.GetJointPosition(pendulum).value() < -29.0f);
		Simulate(scene, 2.0f);
		CHECK(physics.GetJointPosition(pendulum).value() < -35.0f);
		SetJoint(pendulum, [](JointComponent& joint) { joint.LimitSpringFrequency = 0.0f; });
		Simulate(scene, 2.0f);
		CHECK(physics.GetJointPosition(pendulum).value() == doctest::Approx(-30.0f).epsilon(0.05));
		scene.OnSimulationStop();
	}

	TEST_CASE("Cone joints keep their axis inside the cone")
	{
		Scene scene;
		// Boxes hanging from the world 1 m above them, knocked sideways; the angle is how far each box's
		// down axis tilts from vertical.
		auto hang = [&](float x, bool useLimits, float halfAngle) {
			Entity box = CreateBox(scene, { x, 0.0f, 0.0f });
			auto& joint = HangFromWorld(box, JointType::Cone);
			joint.UseLimits = useLimits;
			joint.LimitMax = halfAngle;
			return box;
		};
		Entity cone = hang(0.0f, true, 20.0f);
		Entity free = hang(5.0f, false, 0.0f);
		Entity clamped = hang(10.0f, true, 20.0f);
		clamped.GetComponent<JointComponent>().LimitMin = -10.0f;
		auto tilt = [](Entity box) { return TiltDegrees(box.GetTransform().Rotation, { 0.0f, 1.0f, 0.0f }); };

		const uint64_t before = Log::GetHistory().GetTotalCount();
		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		for (Entity box : { cone, free, clamped })
			physics.SetLinearVelocity(box, { 0.0f, 0.0f, 4.0f });
		float coneMax = 0.0f;
		float freeMax = 0.0f;
		for (int i = 0; i < 90; i++)
		{
			scene.OnUpdate(Step);
			coneMax = std::max(coneMax, tilt(cone));
			freeMax = std::max(freeMax, tilt(free));
			// The anchor holds like a point joint.
			CHECK(glm::distance(cone.GetTransform().Translation + cone.GetTransform().Rotation * glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)) < 0.02f);
		}
		CHECK(coneMax == doctest::Approx(20.0f).epsilon(0.1));
		CHECK(freeMax > 60.0f);
		CHECK(physics.HasJoint(clamped));
		CHECK(CountMessages(before, "LimitMin has no effect on cone joints") == 1);
		CHECK(physics.GetJointPosition(cone) == std::nullopt);

		// Widening the cone in place lets the next push swing further.
		SetJoint(cone, [](JointComponent& joint) { joint.LimitMax = 60.0f; });
		scene.OnUpdate(Step);
		physics.SetLinearVelocity(cone, { 0.0f, 0.0f, 4.0f });
		coneMax = 0.0f;
		for (int i = 0; i < 60; i++)
		{
			scene.OnUpdate(Step);
			coneMax = std::max(coneMax, tilt(cone));
		}
		CHECK(coneMax > 35.0f);
		CHECK(coneMax < 63.0f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Cone and six-DOF joints break on the torque at their limits")
	{
		Scene scene;
		// A narrow cone carries torque once the box hits its edge; a free cone never does.
		auto hang = [&](float x, JointType type, bool useLimits) {
			Entity box = CreateBox(scene, { x, 0.0f, 0.0f });
			auto& joint = HangFromWorld(box, type);
			joint.UseLimits = useLimits;
			joint.LimitMax = type == JointType::Cone ? 5.0f : 0.0f;
			joint.AngularLimitMax = { 5.0f, 5.0f, 5.0f };
			joint.AngularLimitMin = { -5.0f, -5.0f, -5.0f };
			joint.BreakTorque = 20.0f;
			return box;
		};
		Entity cone = hang(0.0f, JointType::Cone, true);
		Entity freeCone = hang(5.0f, JointType::Cone, false);
		Entity sixDOF = hang(10.0f, JointType::SixDOF, true);
		Entity freeSixDOF = hang(15.0f, JointType::SixDOF, false);

		scene.OnSimulationStart();
		for (Entity box : { cone, freeCone, sixDOF, freeSixDOF })
			scene.GetPhysicsWorld()->SetLinearVelocity(box, { 0.0f, 0.0f, 4.0f });
		Simulate(scene, 0.5f);
		CHECK_FALSE(cone.HasComponent<JointComponent>());
		CHECK(freeCone.HasComponent<JointComponent>());
		CHECK_FALSE(sixDOF.HasComponent<JointComponent>());
		CHECK(freeSixDOF.HasComponent<JointComponent>());
		scene.OnSimulationStop();
	}

	TEST_CASE("Six-DOF joints limit twist and swing around their frame")
	{
		Scene scene;
		// Weightless boxes pinned at their centre. Axis (the twist axis) is local Y.
		auto pinned = [&](float x, bool useLimits) {
			Entity box = CreateWeightlessBox(scene, { x, 0.0f, 0.0f });
			auto& joint = box.AddComponent<JointComponent>();
			joint.Type = JointType::SixDOF;
			joint.Axis = { 0.0f, 1.0f, 0.0f };
			joint.SecondaryAxis = { 1.0f, 0.0f, 0.0f };
			joint.UseLimits = useLimits;
			joint.AngularLimitMin = { -10.0f, -30.0f, -30.0f };
			joint.AngularLimitMax = { 20.0f, 30.0f, 30.0f };
			return box;
		};
		Entity twist = pinned(0.0f, true);
		Entity freeTwist = pinned(5.0f, false);
		Entity swing = pinned(10.0f, true);
		// Setting only the swing maximum is enough: a minimum of 0 mirrors it without a warning.
		swing.GetComponent<JointComponent>().AngularLimitMin = { -10.0f, 0.0f, 0.0f };
		const uint64_t before = Log::GetHistory().GetTotalCount();

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		physics.SetAngularVelocity(twist, { 0.0f, 3.0f, 0.0f });
		physics.SetAngularVelocity(freeTwist, { 0.0f, 3.0f, 0.0f });
		physics.SetAngularVelocity(swing, { 3.0f, 0.0f, 0.0f });
		float twistMax = 0.0f;
		float swingMax = 0.0f;
		for (int i = 0; i < 60; i++)
		{
			scene.OnUpdate(Step);
			twistMax = std::max(twistMax, glm::degrees(glm::angle(twist.GetTransform().Rotation)));
			swingMax = std::max(swingMax, TiltDegrees(swing.GetTransform().Rotation, { 0.0f, 1.0f, 0.0f }));
			// Translation is locked without linear limits.
			CHECK(glm::length(twist.GetTransform().Translation) < 0.01f);
		}
		// Spinning positive around Y hits the +20 degree twist limit.
		CHECK(twistMax == doctest::Approx(20.0f).epsilon(0.1));
		CHECK(glm::degrees(glm::angle(freeTwist.GetTransform().Rotation)) > 90.0f);
		CHECK(swingMax == doctest::Approx(30.0f).epsilon(0.1));
		CHECK(CountMessages(before, "six-DOF swing") == 0);
		CHECK(physics.GetJointPosition(twist) == std::nullopt);
		scene.OnSimulationStop();
	}

	TEST_CASE("Six-DOF translation limits, locked axes, soft limits and warnings")
	{
		Scene scene;
		// Weightless boxes thrown diagonally: X (Axis) may move within [-0.5, 1], Y and Z are locked, and so
		// is every rotation.
		auto slider = [&](float z, float springFrequency) {
			Entity box = CreateWeightlessBox(scene, { 0.0f, 0.0f, z });
			auto& joint = box.AddComponent<JointComponent>();
			joint.Type = JointType::SixDOF;
			joint.Axis = { 1.0f, 0.0f, 0.0f };
			joint.SecondaryAxis = { 0.0f, 1.0f, 0.0f };
			joint.UseLimits = true;
			joint.LinearLimitMin = { -0.5f, 0.0f, 0.0f };
			joint.LinearLimitMax = { 1.0f, 0.0f, 0.0f };
			joint.LimitSpringFrequency = springFrequency;
			joint.LimitSpringDamping = 0.2f;
			return box;
		};
		Entity rigid = slider(0.0f, 0.0f);
		Entity soft = slider(5.0f, 2.0f);

		// Invalid settings are fixed up with a warning and the joint is still built.
		Entity odd = CreateWeightlessBox(scene, { 0.0f, 0.0f, 10.0f });
		auto& oddJoint = odd.AddComponent<JointComponent>();
		oddJoint.Type = JointType::SixDOF;
		oddJoint.Axis = { 1.0f, 0.0f, 0.0f };
		oddJoint.SecondaryAxis = { 2.0f, 0.0f, 0.0f };
		oddJoint.UseLimits = true;
		oddJoint.LimitMax = 1.0f;
		oddJoint.LinearLimitMin = { 0.5f, 0.0f, 0.0f };
		oddJoint.AngularLimitMin = { 0.0f, -10.0f, 0.0f };
		oddJoint.AngularLimitMax = { 0.0f, 30.0f, 0.0f };

		const uint64_t before = Log::GetHistory().GetTotalCount();
		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		physics.SetLinearVelocity(rigid, { 4.0f, 3.0f, 3.0f });
		physics.SetAngularVelocity(rigid, { 2.0f, 2.0f, 2.0f });
		physics.SetLinearVelocity(soft, { 4.0f, 3.0f, 3.0f });
		float rigidPeak = 0.0f;
		float softPeak = 0.0f;
		for (int i = 0; i < 30; i++)
		{
			scene.OnUpdate(Step);
			const glm::vec3 position = rigid.GetTransform().Translation;
			rigidPeak = std::max(rigidPeak, position.x);
			softPeak = std::max(softPeak, soft.GetTransform().Translation.x);
			// The spring softens only the limited X axis; the locked axes stay rigid.
			CHECK(std::abs(soft.GetTransform().Translation.y) < 0.01f);
			CHECK(std::abs(position.y) < 0.01f);
			CHECK(std::abs(position.z) < 0.01f);
			CHECK(glm::degrees(glm::angle(rigid.GetTransform().Rotation)) < 1.0f);
		}
		CHECK(rigidPeak == doctest::Approx(1.0f).epsilon(0.05));
		CHECK(softPeak > 1.2f);
		CHECK(physics.HasJoint(odd));
		CHECK(CountMessages(before, "SecondaryAxis is zero or parallel to Axis") == 1);
		CHECK(CountMessages(before, "LimitMax has no effect on six-DOF joints") == 1);
		CHECK(CountMessages(before, "six-DOF linear X limits [0.5, 0] must satisfy") == 1);
		CHECK(CountMessages(before, "six-DOF swing (angular Y) limits [-10, 30] must be a symmetric half angle") == 1);

		// Widening a limit updates the live joint: the next push carries the box further along X.
		SetJoint(rigid, [](JointComponent& joint) { joint.LinearLimitMax.x = 3.0f; });
		physics.SetLinearVelocity(rigid, { 4.0f, 0.0f, 0.0f });
		Simulate(scene, 0.5f);
		CHECK(rigid.GetTransform().Translation.x > 1.5f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Six-DOF motors drive each axis toward its velocity or position target")
	{
		Scene scene;
		// Weightless boxes pinned at their centre with the joint frame on the world axes (X = Axis, Y =
		// SecondaryAxis), so joint-frame and world rotations agree.
		auto pinned = [&](float x) {
			Entity box = CreateWeightlessBox(scene, { x, 0.0f, 0.0f });
			box.GetComponent<RigidBodyComponent>().AngularDamping = 0.0f;
			auto& joint = box.AddComponent<JointComponent>();
			joint.Type = JointType::SixDOF;
			joint.Axis = { 1.0f, 0.0f, 0.0f };
			joint.SecondaryAxis = { 0.0f, 1.0f, 0.0f };
			return box;
		};
		// A target orientation on every rotation axis (the Velocity-free way animation drives a limb).
		Entity limb = pinned(0.0f);
		auto& limbJoint = limb.GetComponent<JointComponent>();
		limbJoint.AngularMotorMode = { JointMotorMode::Position, JointMotorMode::Position, JointMotorMode::Position };
		limbJoint.AngularMotorTarget = { 30.0f, -20.0f, 15.0f };
		limbJoint.MotorSpringFrequency = 4.0f;
		// Spinning around the twist axis only; the other rotation axes have no motor.
		Entity spinner = pinned(5.0f);
		auto& spinnerJoint = spinner.GetComponent<JointComponent>();
		spinnerJoint.AngularMotorMode[0] = JointMotorMode::Velocity;
		spinnerJoint.AngularMotorTarget = { 90.0f, 0.0f, 0.0f };
		// Free translation along X driven to a position, along Y at a speed; Z stays locked.
		Entity carriage = pinned(10.0f);
		auto& carriageJoint = carriage.GetComponent<JointComponent>();
		carriageJoint.FreeLinearAxes = { true, true, false };
		carriageJoint.LinearMotorMode = { JointMotorMode::Position, JointMotorMode::Velocity, JointMotorMode::Off };
		carriageJoint.LinearMotorTarget = { 2.0f, 0.5f, 0.0f };
		const uint64_t before = Log::GetHistory().GetTotalCount();

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		physics.SetLinearVelocity(carriage, { 0.0f, 0.0f, 3.0f });
		Simulate(scene, 3.0f);
		const glm::vec3 limbRotation = physics.GetJointRotation(limb).value();
		CHECK(limbRotation.x == doctest::Approx(30.0f).epsilon(0.03));
		CHECK(limbRotation.y == doctest::Approx(-20.0f).epsilon(0.03));
		CHECK(limbRotation.z == doctest::Approx(15.0f).epsilon(0.03));
		// The joint frame is the world frame here, so the entity's own rotation matches.
		CHECK(glm::degrees(limb.GetTransform().GetRotationEuler().x) == doctest::Approx(30.0f).epsilon(0.03));
		const glm::vec3 spin = physics.GetAngularVelocity(spinner);
		CHECK(glm::degrees(spin.x) == doctest::Approx(90.0f).epsilon(0.02));
		CHECK(std::abs(spin.y) < 0.01f);
		CHECK(std::abs(spin.z) < 0.01f);
		CHECK(carriage.GetTransform().Translation.x == doctest::Approx(12.0f).epsilon(0.01));
		CHECK(physics.GetLinearVelocity(carriage).y == doctest::Approx(0.5f).epsilon(0.02));
		CHECK(std::abs(carriage.GetTransform().Translation.z) < 0.01f);
		CHECK(CountMessages(before, "six-DOF") == 0);

		// Retargeting through the registry updates the live joint: it keeps measuring from the first rest
		// pose instead of being rebuilt at the current one.
		SetJoint(limb, [](JointComponent& joint) { joint.AngularMotorTarget = { -10.0f, 0.0f, 0.0f }; });
		SetJoint(spinner, [](JointComponent& joint) { joint.AngularMotorMode[0] = JointMotorMode::Off; });
		Simulate(scene, 3.0f);
		const glm::vec3 retargeted = physics.GetJointRotation(limb).value();
		CHECK(retargeted.x == doctest::Approx(-10.0f).epsilon(0.03));
		CHECK(std::abs(retargeted.y) < 0.5f);
		CHECK(std::abs(retargeted.z) < 0.5f);
		// Without its motor the spinner coasts (no damping).
		CHECK(glm::degrees(physics.GetAngularVelocity(spinner).x) == doctest::Approx(90.0f).epsilon(0.02));
		scene.OnSimulationStop();
	}

	TEST_CASE("A stiffer six-DOF motor spring reaches its target sooner and MotorMaxForce caps it")
	{
		Scene scene;
		auto powered = [&](float x, float frequency, float maxForce) {
			Entity box = CreateWeightlessBox(scene, { x, 0.0f, 0.0f });
			auto& joint = box.AddComponent<JointComponent>();
			joint.Type = JointType::SixDOF;
			joint.Axis = { 1.0f, 0.0f, 0.0f };
			joint.SecondaryAxis = { 0.0f, 1.0f, 0.0f };
			joint.AngularMotorMode = { JointMotorMode::Off, JointMotorMode::Off, JointMotorMode::Position };
			joint.AngularMotorTarget = { 0.0f, 0.0f, 60.0f };
			joint.MotorSpringFrequency = frequency;
			joint.MotorMaxForce = maxForce;
			return box;
		};
		Entity soft = powered(0.0f, 0.5f, 1000.0f);
		Entity stiff = powered(5.0f, 5.0f, 1000.0f);
		Entity weak = powered(10.0f, 5.0f, 0.2f);
		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		Simulate(scene, 0.5f);
		const float softAngle = physics.GetJointRotation(soft).value().z;
		const float stiffAngle = physics.GetJointRotation(stiff).value().z;
		const float weakAngle = physics.GetJointRotation(weak).value().z;
		CHECK(stiffAngle == doctest::Approx(60.0f).epsilon(0.05));
		CHECK(softAngle < 45.0f);
		// 0.2 N·m barely turns a 1 kg box in half a second.
		CHECK(weakAngle < 15.0f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Six-DOF free axes ignore their limits and motors on locked axes warn")
	{
		Scene scene;
		Entity box = CreateWeightlessBox(scene, { 0.0f, 0.0f, 0.0f });
		auto& joint = box.AddComponent<JointComponent>();
		joint.Type = JointType::SixDOF;
		joint.Axis = { 1.0f, 0.0f, 0.0f };
		joint.SecondaryAxis = { 0.0f, 1.0f, 0.0f };
		joint.UseLimits = true;
		joint.FreeLinearAxes.x = true;
		joint.LinearLimitMax = { 1.0f, 0.5f, 0.0f };
		joint.AngularLimitMax = { 0.0f, 0.0f, 0.0f };
		// Z translation and every rotation are locked: these motors cannot move anything.
		joint.LinearMotorMode[2] = JointMotorMode::Velocity;
		joint.AngularMotorMode[1] = JointMotorMode::Position;

		// Without UseLimits translation is locked, so a linear motor there warns with the reason.
		Entity locked = CreateWeightlessBox(scene, { 0.0f, 0.0f, 5.0f });
		auto& lockedJoint = locked.AddComponent<JointComponent>();
		lockedJoint.Type = JointType::SixDOF;
		lockedJoint.LinearMotorMode[1] = JointMotorMode::Velocity;
		lockedJoint.LinearMotorTarget.y = 1.0f;
		lockedJoint.MotorSpringFrequency = -1.0f;

		const uint64_t before = Log::GetHistory().GetTotalCount();
		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		physics.SetLinearVelocity(box, { 4.0f, 4.0f, 0.0f });
		Simulate(scene, 1.0f);
		// X is free well past its 1 m limit; Y stops at its 0.5 m limit.
		CHECK(box.GetTransform().Translation.x > 3.0f);
		CHECK(box.GetTransform().Translation.y == doctest::Approx(0.5f).epsilon(0.05));
		CHECK(glm::length(locked.GetTransform().Translation - glm::vec3(0.0f, 0.0f, 5.0f)) < 0.01f);
		CHECK(CountMessages(before, "six-DOF linear X limits are ignored: the axis is free (FreeLinearAxes)") == 1);
		CHECK(CountMessages(before, "six-DOF linear Z motor has no effect: the axis is locked") == 1);
		CHECK(CountMessages(before, "six-DOF angular Y motor has no effect: the axis is locked") == 1);
		CHECK(CountMessages(before, "six-DOF linear Y motor has no effect: the axis is locked (translation is locked without UseLimits)") == 1);
		CHECK(CountMessages(before, "MotorSpringFrequency -1 must be greater than 0; using 2") == 1);
		CHECK(physics.HasJoint(box));
		CHECK(physics.HasJoint(locked));

		// Freeing an axis during play updates the live joint.
		SetJoint(box, [](JointComponent& changed) { changed.FreeLinearAxes.y = true; });
		physics.SetLinearVelocity(box, { 0.0f, 4.0f, 0.0f });
		Simulate(scene, 0.5f);
		CHECK(box.GetTransform().Translation.y > 1.5f);
		scene.OnSimulationStop();
	}

	TEST_CASE("A motor on a locked six-DOF axis stays off and cannot break the joint")
	{
		Scene scene;
		// Twist is free, swing is locked: a fast velocity motor on a locked swing axis must not fight the
		// lock (Jolt only skips motors when a whole group is locked).
		Entity limb = CreateWeightlessBox(scene, { 0.0f, 0.0f, 0.0f });
		auto& joint = limb.AddComponent<JointComponent>();
		joint.Type = JointType::SixDOF;
		joint.Axis = { 1.0f, 0.0f, 0.0f };
		joint.SecondaryAxis = { 0.0f, 1.0f, 0.0f };
		joint.UseLimits = true;
		joint.AngularLimitMin = { -180.0f, 0.0f, 0.0f };
		joint.AngularLimitMax = { 180.0f, 0.0f, 0.0f };
		joint.AngularMotorMode = { JointMotorMode::Off, JointMotorMode::Velocity, JointMotorMode::Off };
		joint.AngularMotorTarget = { 0.0f, 2000.0f, 0.0f };
		joint.MotorMaxForce = 10000.0f;
		joint.BreakTorque = 50.0f;
		scene.OnSimulationStart();
		Simulate(scene, 0.5f);
		CHECK(scene.GetPhysicsWorld()->HasJoint(limb));
		CHECK(glm::length(scene.GetPhysicsWorld()->GetAngularVelocity(limb)) < 0.01f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Six-DOF motor effort breaks a joint, and position targets stop at the limits")
	{
		Scene scene;
		// A carriage free along X, pushed by a velocity motor into a static block: it stalls at its force
		// limit, well above BreakForce, so the motor's effort breaks the joint.
		auto carriage = [&](float z) {
			Entity box = CreateWeightlessBox(scene, { 0.0f, 0.0f, z });
			auto& joint = box.AddComponent<JointComponent>();
			joint.Type = JointType::SixDOF;
			joint.Axis = { 1.0f, 0.0f, 0.0f };
			joint.SecondaryAxis = { 0.0f, 1.0f, 0.0f };
			joint.FreeLinearAxes.x = true;
			joint.LinearMotorMode[0] = JointMotorMode::Velocity;
			// Reaching 0.5 m/s within one step takes 30 N, below the break force.
			joint.LinearMotorTarget.x = 0.5f;
			joint.MotorMaxForce = 200.0f;
			joint.BreakForce = 50.0f;
			return box;
		};
		Entity pushing = carriage(0.0f);
		Entity block = CreateBox(scene, { 1.1f, 0.0f, 0.0f });
		block.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		// The same carriage with nothing in its way.
		Entity coasting = carriage(5.0f);

		// A position target beyond the 1 m limit is clamped to it, so the spring eases in toward 1 m. Unclamped,
		// a 4 Hz spring toward 3 m would hit the limit after about 0.035 s and keep pressing into it.
		Entity reaching = CreateWeightlessBox(scene, { 0.0f, 0.0f, 10.0f });
		auto& reach = reaching.AddComponent<JointComponent>();
		reach.Type = JointType::SixDOF;
		reach.Axis = { 1.0f, 0.0f, 0.0f };
		reach.SecondaryAxis = { 0.0f, 1.0f, 0.0f };
		reach.UseLimits = true;
		reach.LinearLimitMax = { 1.0f, 0.0f, 0.0f };
		reach.LinearMotorMode[0] = JointMotorMode::Position;
		reach.LinearMotorTarget.x = 3.0f;
		reach.MotorSpringFrequency = 4.0f;
		reach.MotorMaxForce = 1000.0f;

		scene.OnSimulationStart();
		Simulate(scene, 0.1f);
		// Critically damped toward 1 m: about 0.71 m after 0.1 s.
		CHECK(reaching.GetTransform().Translation.x == doctest::Approx(0.71f).epsilon(0.1));
		Simulate(scene, 1.9f);
		CHECK_FALSE(pushing.HasComponent<JointComponent>());
		CHECK(coasting.HasComponent<JointComponent>());
		CHECK(reaching.GetTransform().Translation.x == doctest::Approx(1.0f).epsilon(0.02));
		scene.OnSimulationStop();
	}

	TEST_CASE("Hinge and slider motor springs set how fast a position motor arrives")
	{
		Scene scene;
		auto servo = [&](float x, JointType type, float frequency) {
			Entity box = CreateWeightlessBox(scene, { x, 0.0f, 0.0f });
			auto& joint = box.AddComponent<JointComponent>();
			joint.Type = type;
			joint.Axis = { 0.0f, 0.0f, 1.0f };
			joint.MotorMode = JointMotorMode::Position;
			joint.MotorTarget = type == JointType::Hinge ? 60.0f : 1.0f;
			joint.MotorSpringFrequency = frequency;
			joint.MotorMaxForce = 1000.0f;
			return box;
		};
		Entity softHinge = servo(0.0f, JointType::Hinge, 0.5f);
		Entity stiffHinge = servo(5.0f, JointType::Hinge, 5.0f);
		Entity softSlider = servo(10.0f, JointType::Slider, 0.5f);
		Entity stiffSlider = servo(15.0f, JointType::Slider, 5.0f);
		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		Simulate(scene, 0.5f);
		CHECK(physics.GetJointPosition(stiffHinge).value() == doctest::Approx(60.0f).epsilon(0.05));
		CHECK(physics.GetJointPosition(softHinge).value() < 45.0f);
		CHECK(physics.GetJointPosition(stiffSlider).value() == doctest::Approx(1.0f).epsilon(0.05));
		CHECK(physics.GetJointPosition(softSlider).value() < 0.75f);
		scene.OnSimulationStop();
	}

	TEST_CASE("A non-positive motor spring frequency falls back to the default and still drives")
	{
		Scene scene;
		Entity arm = CreateWeightlessBox(scene, { 0.0f, 0.0f, 0.0f });
		auto& servo = arm.AddComponent<JointComponent>();
		servo.Axis = { 0.0f, 0.0f, 1.0f };
		servo.MotorMode = JointMotorMode::Position;
		servo.MotorTarget = 45.0f;
		servo.MotorSpringFrequency = 0.0f;
		const uint64_t before = Log::GetHistory().GetTotalCount();
		scene.OnSimulationStart();
		Simulate(scene, 3.0f);
		CHECK(scene.GetPhysicsWorld()->GetJointPosition(arm).value() == doctest::Approx(45.0f).epsilon(0.02));
		CHECK(CountMessages(before, "MotorSpringFrequency 0 must be greater than 0; using 2") == 1);
		scene.OnSimulationStop();
	}

	TEST_CASE("GetJointRotation measures cone and six-DOF joints from their rest pose")
	{
		Scene scene;
		// Turned bodies give each body a different local twist axis, so Jolt picks different cone frames
		// for them; the rest pose must still read as no rotation.
		Entity cone = CreateWeightlessBox(scene, { 0.0f, 0.0f, 0.0f });
		cone.GetTransform().SetRotationEuler(glm::radians(glm::vec3(20.0f, 37.0f, -15.0f)));
		auto& coneJoint = HangFromWorld(cone, JointType::Cone);
		coneJoint.Axis = { 0.3f, -1.0f, 0.2f };
		Entity anchor = CreateBox(scene, { 5.0f, 2.0f, 0.0f });
		anchor.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		anchor.GetTransform().SetRotationEuler(glm::radians(glm::vec3(0.0f, 50.0f, 0.0f)));
		Entity limb = CreateWeightlessBox(scene, { 5.0f, 0.0f, 0.0f });
		limb.GetComponent<RigidBodyComponent>().AngularDamping = 0.0f;
		auto& limbJoint = limb.AddComponent<JointComponent>();
		limbJoint.Type = JointType::SixDOF;
		limbJoint.ConnectedEntity = anchor.GetUUID();
		limbJoint.Axis = { 0.0f, 1.0f, 0.0f };
		limbJoint.SecondaryAxis = { 1.0f, 0.0f, 0.0f };
		Entity hinge = CreateWeightlessBox(scene, { 10.0f, 0.0f, 0.0f });
		hinge.AddComponent<JointComponent>();

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		scene.OnUpdate(Step);
		CHECK(glm::length(physics.GetJointRotation(cone).value()) < 0.1f);
		CHECK(glm::length(physics.GetJointRotation(limb).value()) < 0.1f);
		CHECK(physics.GetJointRotation(hinge) == std::nullopt);
		CHECK(physics.GetJointRotation(anchor) == std::nullopt);

		// A twist around the six-DOF Axis (world Y here) reads as rotation around the frame's X.
		physics.SetAngularVelocity(limb, { 0.0f, glm::radians(30.0f), 0.0f });
		Simulate(scene, 1.0f);
		const glm::vec3 twist = physics.GetJointRotation(limb).value();
		CHECK(twist.x == doctest::Approx(30.0f).epsilon(0.03));
		CHECK(std::abs(twist.y) < 0.5f);
		CHECK(std::abs(twist.z) < 0.5f);
		// The cone's measured angle matches how far its body turned.
		physics.SetAngularVelocity(cone, { 0.0f, 0.0f, glm::radians(25.0f) });
		const glm::quat start = cone.GetTransform().Rotation;
		Simulate(scene, 1.0f);
		const float turned = glm::degrees(glm::angle(cone.GetTransform().Rotation * glm::inverse(start)));
		const glm::vec3 coneRotation = physics.GetJointRotation(cone).value();
		CHECK(glm::degrees(glm::angle(Math::QuatFromEuler(glm::radians(coneRotation)))) == doctest::Approx(turned).epsilon(0.02));
		scene.OnSimulationStop();
	}

	TEST_CASE("Joint entities give one body several joints")
	{
		Scene scene;
		// A 2 m rung hanging level from two 2 m ropes, one at each end, each on its own child entity.
		Entity rung = CreateBox(scene, { 0.0f, 0.0f, 0.0f });
		rung.GetComponent<BoxColliderComponent>().HalfExtents = { 1.0f, 0.1f, 0.1f };
		auto rope = [&](const char* name, float x) {
			Entity holder = scene.CreateChildEntity(rung, name);
			auto& joint = holder.AddComponent<JointComponent>();
			joint.Type = JointType::Distance;
			joint.BodyEntity = rung.GetUUID();
			joint.Anchor = { x, 0.0f, 0.0f };
			joint.ConnectedAnchor = { x, 2.0f, 0.0f };
			return holder;
		};
		Entity left = rope("Left", -1.0f);
		Entity right = rope("Right", 1.0f);
		// A joint entity's own transform plays no part: the anchor is in the rung's space wherever it sits.
		left.GetTransform().Translation = { 0.0f, 5.0f, 3.0f };
		left.GetTransform().SetRotationEuler(glm::radians(glm::vec3(0.0f, 90.0f, 45.0f)));

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		CHECK(physics.HasJoint(left));
		CHECK(physics.HasJoint(right));
		CHECK_FALSE(physics.HasJoint(rung));
		// The holders have no body of their own; their joints move the rung.
		CHECK_FALSE(physics.HasBody(left));
		physics.SetLinearVelocity(rung, { 0.0f, 0.0f, 1.0f });
		Simulate(scene, 1.0f);
		// Moving a joint entity elsewhere in the hierarchy leaves its joint alone.
		scene.SetParent(right, Entity{});
		Simulate(scene, 1.0f);
		CHECK(physics.HasJoint(right));
		const auto& transform = rung.GetTransform();
		CHECK(std::abs(transform.Translation.x) < 0.02f);
		// Both ropes hold it level: its local X stays horizontal while it swings.
		CHECK(std::abs((transform.Rotation * glm::vec3(1.0f, 0.0f, 0.0f)).y) < 0.02f);

		// Rebuilding the rung's body rebuilds both of its joints.
		rung.AddOrReplaceComponent<BoxColliderComponent>(BoxColliderComponent{ { 1.0f, 0.12f, 0.12f } });
		scene.OnUpdate(Step);
		CHECK(physics.HasJoint(left));
		CHECK(physics.HasJoint(right));

		// With one rope gone, the free end drops.
		scene.DestroyEntity(left);
		Simulate(scene, 1.0f);
		CHECK(physics.HasJoint(right));
		CHECK((transform.Rotation * glm::vec3(1.0f, 0.0f, 0.0f)).y > 0.5f);

		// Destroying the body removes the joints that move it, including one on a joint entity outside it.
		scene.DestroyEntity(rung);
		Simulate(scene, 0.1f);
		CHECK_FALSE(physics.HasJoint(right));
		CHECK(right.HasComponent<JointComponent>());
		scene.OnSimulationStop();
	}

	TEST_CASE("A joint entity waits for its BodyEntity's body and rejects bad bodies")
	{
		Scene scene;
		Entity frame = CreateGround(scene);
		Entity body = scene.CreateEntity("Body");
		body.GetTransform().Translation = { 0.0f, 2.0f, 0.0f };
		Entity holder = scene.CreateEntity("Holder");
		auto& joint = holder.AddComponent<JointComponent>();
		joint.Type = JointType::Point;
		joint.BodyEntity = body.GetUUID();
		joint.ConnectedEntity = frame.GetUUID();
		Entity missing = scene.CreateEntity("Missing");
		missing.AddComponent<JointComponent>().BodyEntity = 4242;
		// A joint between a body and itself is rejected, even when held elsewhere.
		Entity self = CreateBox(scene, { 5.0f, 0.0f, 0.0f });
		Entity selfHolder = scene.CreateEntity("SelfHolder");
		auto& selfJoint = selfHolder.AddComponent<JointComponent>();
		selfJoint.BodyEntity = self.GetUUID();
		selfJoint.ConnectedEntity = self.GetUUID();

		const uint64_t before = Log::GetHistory().GetTotalCount();
		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		Simulate(scene, 0.1f);
		CHECK_FALSE(physics.HasJoint(holder));
		CHECK_FALSE(physics.HasJoint(missing));
		CHECK_FALSE(physics.HasJoint(selfHolder));
		CHECK(CountMessages(before, "its BodyEntity 'Body' has no RigidBody") == 1);
		CHECK(CountMessages(before, "its BodyEntity 4242 is missing") == 1);
		CHECK(CountMessages(before, "connects to its own body's entity") == 1);

		// The joint is built once the body exists, and holds it up.
		body.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		body.AddComponent<BoxColliderComponent>();
		scene.OnUpdate(Step);
		CHECK(physics.HasJoint(holder));
		Simulate(scene, 0.5f);
		CHECK(std::abs(body.GetTransform().Translation.y - 2.0f) < 0.05f);
		scene.OnSimulationStop();
	}

	TEST_CASE("A joint entity's joint goes with its body and comes back with it")
	{
		// The holders are top-level entities, so destroying a body does not destroy them along with it.
		auto run = [](bool destroy) {
			Scene scene;
			const UUID bodyID = 777;
			auto createBody = [&] {
				Entity body = scene.CreateEntityWithUUID(bodyID, "Body");
				body.GetTransform().Translation = { 0.0f, 3.0f, 0.0f };
				body.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
				body.AddComponent<BoxColliderComponent>();
				return body;
			};
			Entity body = createBody();
			Entity holder = scene.CreateEntity("Holder");
			auto& joint = holder.AddComponent<JointComponent>();
			joint.Type = JointType::Point;
			joint.BodyEntity = bodyID;

			const uint64_t before = Log::GetHistory().GetTotalCount();
			scene.OnSimulationStart();
			PhysicsWorld& physics = *scene.GetPhysicsWorld();
			REQUIRE(physics.HasJoint(holder));
			if (destroy)
				scene.DestroyEntity(body);
			else
				body.RemoveComponent<RigidBodyComponent>();
			Simulate(scene, 0.5f);
			CHECK_FALSE(physics.HasJoint(holder));
			CHECK(holder.HasComponent<JointComponent>());
			CHECK(CountMessages(before, destroy ? "its BodyEntity 777 is missing" : "its BodyEntity 'Body' has no RigidBody") == 1);

			// A body with the same UUID (or the same entity's body again) gets the joint back.
			if (destroy)
				body = createBody();
			else
				body.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
			body.GetTransform().Translation = { 0.0f, 3.0f, 0.0f };
			scene.OnUpdate(Step);
			CHECK(physics.HasJoint(holder));
			Simulate(scene, 0.5f);
			CHECK(std::abs(body.GetTransform().Translation.y - 3.0f) < 0.05f);
			scene.OnSimulationStop();
		};
		SUBCASE("destroyed")
		{
			run(true);
		}
		SUBCASE("RigidBody removed")
		{
			run(false);
		}
	}

	TEST_CASE("Changing BodyEntity during play moves the joint and its collision filter")
	{
		// Regression: Jolt replays cached contacts for resting bodies without asking the pair filter, so a
		// box already resting on the plate kept colliding after a joint started ignoring the pair.
		Scene scene;
		Entity plate = CreateGround(scene);
		// Boxes resting on the plate. The holder's vertical slider to the plate, without collision, lets its
		// body sink through the plate.
		Entity first = CreateBox(scene, { 0.0f, 0.5f, 0.0f });
		Entity second = CreateBox(scene, { 3.0f, 0.5f, 0.0f });
		Entity holder = scene.CreateEntity("Holder");
		auto& joint = holder.AddComponent<JointComponent>();
		joint.Type = JointType::Slider;
		joint.BodyEntity = first.GetUUID();
		joint.ConnectedEntity = plate.GetUUID();

		scene.OnSimulationStart();
		Simulate(scene, 0.2f);
		CHECK(first.GetTransform().Translation.y < 0.45f);
		CHECK(second.GetTransform().Translation.y == doctest::Approx(0.5f).epsilon(0.02));
		SetJoint(holder, [&](JointComponent& changed) { changed.BodyEntity = second.GetUUID(); });
		Simulate(scene, 1.0f);
		CHECK(scene.GetPhysicsWorld()->HasJoint(holder));
		CHECK(second.GetTransform().Translation.y < -2.0f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Turning EnableCollision off during play lets resting bodies pass through each other")
	{
		Scene scene;
		Entity plate = CreateGround(scene);
		Entity box = CreateBox(scene, { 0.0f, 0.5f, 0.0f });
		auto& joint = box.AddComponent<JointComponent>();
		joint.Type = JointType::Slider;
		joint.ConnectedEntity = plate.GetUUID();
		joint.EnableCollision = true;

		scene.OnSimulationStart();
		// Long enough for the box to come to rest (and fall asleep) on the plate.
		Simulate(scene, 2.0f);
		CHECK(box.GetTransform().Translation.y == doctest::Approx(0.5f).epsilon(0.02));
		SetJoint(box, [](JointComponent& changed) { changed.EnableCollision = false; });
		Simulate(scene, 1.0f);
		CHECK(box.GetTransform().Translation.y < -2.0f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Narrowing cone and six-DOF limits during play updates the live joint")
	{
		// A rebuild would take the current pose as the new rest pose, so a body pressed against its old limit
		// would stay there; an in-place update pulls it back inside the narrower one.
		Scene scene;
		Entity slider = CreateWeightlessBox(scene, { 0.0f, 0.0f, 0.0f });
		auto& linear = slider.AddComponent<JointComponent>();
		linear.Type = JointType::SixDOF;
		linear.Axis = { 1.0f, 0.0f, 0.0f };
		linear.UseLimits = true;
		linear.LinearLimitMin = { -0.5f, 0.0f, 0.0f };
		linear.LinearLimitMax = { 1.0f, 0.0f, 0.0f };
		Entity lamp = CreateBox(scene, { 5.0f, 0.0f, 0.0f });
		auto& cone = HangFromWorld(lamp, JointType::Cone);
		cone.UseLimits = true;
		cone.LimitMax = 60.0f;

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		auto push = [&](float seconds) {
			for (int i = 0; i < static_cast<int>(std::lround(seconds / Step)); i++)
			{
				physics.SetLinearVelocity(slider, { 2.0f, 0.0f, 0.0f });
				physics.SetLinearVelocity(lamp, { 0.0f, 0.0f, 3.0f });
				scene.OnUpdate(Step);
			}
		};
		push(1.0f);
		CHECK(slider.GetTransform().Translation.x == doctest::Approx(1.0f).epsilon(0.05));
		CHECK(TiltDegrees(lamp.GetTransform().Rotation, { 0.0f, 1.0f, 0.0f }) > 50.0f);

		SetJoint(slider, [](JointComponent& joint) { joint.LinearLimitMax.x = 0.5f; });
		SetJoint(lamp, [](JointComponent& joint) { joint.LimitMax = 20.0f; });
		push(1.0f);
		CHECK(slider.GetTransform().Translation.x == doctest::Approx(0.5f).epsilon(0.1));
		CHECK(TiltDegrees(lamp.GetTransform().Rotation, { 0.0f, 1.0f, 0.0f }) < 30.0f);
		scene.OnSimulationStop();
	}

	TEST_CASE("Joint warnings stay logged once while a script updates the joint")
	{
		Scene scene;
		// Warnings from building the joint (SecondaryAxis) and from its settings (a spring with no limited
		// translation axis to soften) must not be logged again by in-place updates.
		Entity box = CreateWeightlessBox(scene, { 0.0f, 0.0f, 0.0f });
		auto& joint = box.AddComponent<JointComponent>();
		joint.Type = JointType::SixDOF;
		joint.Axis = { 1.0f, 0.0f, 0.0f };
		joint.SecondaryAxis = { 1.0f, 0.0f, 0.0f };
		joint.UseLimits = true;
		joint.AngularLimitMax = { 30.0f, 30.0f, 30.0f };
		joint.LimitSpringFrequency = 2.0f;

		const uint64_t before = Log::GetHistory().GetTotalCount();
		scene.OnSimulationStart();
		for (int i = 0; i < 10; i++)
		{
			SetJoint(box, [i](JointComponent& changed) { changed.BreakForce = 1000.0f + static_cast<float>(i); });
			scene.OnUpdate(Step);
		}
		CHECK(scene.GetPhysicsWorld()->HasJoint(box));
		CHECK(CountMessages(before, "SecondaryAxis is zero or parallel to Axis") == 1);
		CHECK(CountMessages(before, "LimitSpringFrequency has no effect on a six-DOF joint without a limited translation axis") == 1);
		scene.OnSimulationStop();
	}

	TEST_CASE("Cone and six-DOF joints accept their full 180 degree range")
	{
		// Regression: glm::radians(180) can round past Jolt's pi, which its cone and swing limits reject.
		Scene scene;
		Entity wide = CreateBox(scene, { 0.0f, 0.0f, 0.0f });
		auto& cone = HangFromWorld(wide, JointType::Cone);
		cone.UseLimits = true;
		cone.LimitMax = 180.0f;
		Entity clamped = CreateBox(scene, { 5.0f, 0.0f, 0.0f });
		auto& clampedCone = HangFromWorld(clamped, JointType::Cone);
		clampedCone.UseLimits = true;
		clampedCone.LimitMax = 200.0f;
		Entity sixDOF = CreateBox(scene, { 10.0f, 0.0f, 0.0f });
		auto& full = HangFromWorld(sixDOF, JointType::SixDOF);
		full.UseLimits = true;
		full.AngularLimitMin = { -180.0f, -180.0f, -180.0f };
		full.AngularLimitMax = { 180.0f, 180.0f, 180.0f };

		const uint64_t before = Log::GetHistory().GetTotalCount();
		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		float maxTilt[3] = {};
		const Entity boxes[3] = { wide, clamped, sixDOF };
		for (Entity box : boxes)
			physics.SetLinearVelocity(box, { 0.0f, 0.0f, 4.0f });
		for (int i = 0; i < 60; i++)
		{
			scene.OnUpdate(Step);
			for (int b = 0; b < 3; b++)
				maxTilt[b] = std::max(maxTilt[b], TiltDegrees(boxes[b].GetTransform().Rotation, { 0.0f, 1.0f, 0.0f }));
		}
		for (int b = 0; b < 3; b++)
		{
			INFO("box " << b);
			CHECK(physics.HasJoint(boxes[b]));
			CHECK(maxTilt[b] > 60.0f);
		}
		CHECK(CountMessages(before, "cone half angle LimitMax 200 must be within [0, 180] degrees") == 1);
		scene.OnSimulationStop();
	}

	TEST_CASE("Fields a joint ignores are reported by name")
	{
		Scene scene;
		Entity ball = CreateBox(scene, { 0.0f, 0.0f, 0.0f });
		auto& point = ball.AddComponent<JointComponent>();
		point.Type = JointType::Point;
		point.Axis = { 1.0f, 0.0f, 0.0f };
		point.MotorMode = JointMotorMode::Velocity;
		point.BreakTorque = 5.0f;
		// Cone and six-DOF joints without limits rotate freely: they carry no torque and use no limits.
		Entity cone = CreateBox(scene, { 5.0f, 0.0f, 0.0f });
		auto& freeCone = HangFromWorld(cone, JointType::Cone);
		freeCone.BreakTorque = 5.0f;
		Entity sixDOF = CreateBox(scene, { 10.0f, 0.0f, 0.0f });
		HangFromWorld(sixDOF, JointType::SixDOF).AngularLimitMax = { 10.0f, 10.0f, 10.0f };
		Entity hinge = CreateBox(scene, { 15.0f, 0.0f, 0.0f });
		hinge.AddComponent<JointComponent>().LinearLimitMax = { 1.0f, 0.0f, 0.0f };

		const uint64_t before = Log::GetHistory().GetTotalCount();
		scene.OnSimulationStart();
		Simulate(scene, 0.1f);
		CHECK(CountMessages(before, "Axis, MotorMode, BreakTorque have no effect on point joints") == 1);
		CHECK(CountMessages(before, "BreakTorque has no effect without UseLimits") == 1);
		CHECK(CountMessages(before, "AngularLimitMax has no effect without UseLimits") == 1);
		CHECK(CountMessages(before, "LinearLimitMax has no effect on hinge joints") == 1);
		// The joints are still built.
		for (Entity entity : { ball, cone, sixDOF, hinge })
			CHECK(scene.GetPhysicsWorld()->HasJoint(entity));
		scene.OnSimulationStop();
	}

	TEST_CASE("Simulation with several joints on one body is deterministic")
	{
		auto run = []() {
			Scene scene;
			Entity hub = CreateBox(scene, { 0.0f, 0.0f, 0.0f });
			std::vector<Entity> spokes;
			for (int i = 0; i < 3; i++)
			{
				const float angle = glm::radians(120.0f * static_cast<float>(i));
				const glm::vec3 direction(std::cos(angle), 0.0f, std::sin(angle));
				Entity spoke = CreateBox(scene, direction * 1.5f);
				Entity holder = scene.CreateChildEntity(hub, "Spoke");
				auto& joint = holder.AddComponent<JointComponent>();
				joint.Type = JointType::Point;
				joint.BodyEntity = hub.GetUUID();
				joint.ConnectedEntity = spoke.GetUUID();
				joint.Anchor = direction * 0.75f;
				spokes.push_back(spoke);
			}
			// The hub hangs from the world; the spokes swing around it.
			auto& top = hub.AddComponent<JointComponent>();
			top.Type = JointType::Point;
			top.Anchor = { 0.0f, 1.0f, 0.0f };
			scene.OnSimulationStart();
			Simulate(scene, 1.0f);
			// Rebuilding the hub's body removes and rebuilds all four of its joints mid-run.
			hub.AddOrReplaceComponent<BoxColliderComponent>(BoxColliderComponent{ { 0.55f, 0.55f, 0.55f } });
			Simulate(scene, 1.0f);
			std::vector<glm::vec3> positions;
			positions.reserve(spokes.size() + 1);
			positions.push_back(hub.GetTransform().Translation);
			for (Entity spoke : spokes)
				positions.push_back(spoke.GetTransform().Translation);
			scene.OnSimulationStop();
			return positions;
		};
		CHECK(run() == run());
	}
}

TEST_SUITE("PhysicsLayers")
{
	TEST_CASE("Layers start with Default, look up by name and keep the matrix symmetric")
	{
		PhysicsLayers layers;
		CHECK(layers.GetNames() == std::vector<std::string>{ "Default" });
		std::string error;
		REQUIRE(layers.Add("Player", error));
		REQUIRE(layers.Add("Debris", error));
		CHECK_FALSE(layers.Add("Player", error));
		CHECK(error.find("duplicate") != std::string::npos);
		CHECK_FALSE(layers.Add("", error));

		CHECK(layers.Find("Debris") == 2u);
		CHECK_FALSE(layers.Find("Enemy").has_value());
		CHECK(layers.ShouldCollide(1, 2));

		layers.SetCollides(2, 1, false);
		layers.SetCollides(2, 2, false);
		CHECK_FALSE(layers.ShouldCollide(1, 2));
		CHECK_FALSE(layers.ShouldCollide(2, 1));
		CHECK_FALSE(layers.ShouldCollide(2, 2));
		CHECK(layers.ShouldCollide(0, 2));
		// Masks only have bits for defined layers; an undefined layer's mask is empty.
		CHECK(layers.GetCollisionMask(2) == 0b001u);
		CHECK(layers.GetCollisionMask(0) == 0b111u);
		CHECK(layers.GetCollisionMask(3) == 0u);

		// Equality compares names and the matrix between them.
		PhysicsLayers same;
		REQUIRE(same.Add("Player", error));
		REQUIRE(same.Add("Debris", error));
		CHECK(same != layers);
		same.SetCollides(1, 2, false);
		same.SetCollides(2, 2, false);
		CHECK(same == layers);

		CHECK(layers.MaskFromNames({ "Default", "Debris" }, error) == 0b101u);
		CHECK_FALSE(layers.MaskFromNames({ "Enemy" }, error).has_value());
		CHECK(error.find("Enemy") != std::string::npos);

		for (uint32_t i = layers.GetCount(); i < PhysicsLayers::MaxLayers; i++)
			REQUIRE(layers.Add("Layer" + std::to_string(i), error));
		CHECK_FALSE(layers.Add("OneTooMany", error));
		// The last layer uses bit 31.
		CHECK(layers.GetCollisionMask(31) == 0xFFFFFFFFu);
		layers.SetCollides(0, 31, false);
		CHECK_FALSE(layers.ShouldCollide(31, 0));
		CHECK(layers.GetCollisionMask(31) == 0xFFFFFFFEu);
		CHECK((layers.GetCollisionMask(0) & 0x80000000u) == 0u);
	}

	TEST_CASE("Layers round-trip through JSON and reject malformed data")
	{
		PhysicsLayers layers;
		std::string error;
		REQUIRE(layers.Add("Player", error));
		REQUIRE(layers.Add("Pickup", error));
		layers.SetCollides(1, 2, false);
		layers.SetCollides(2, 2, false);

		const nlohmann::json data = layers.ToJson();
		CHECK(data["IgnoredPairs"] == nlohmann::json::parse(R"([["Player","Pickup"],["Pickup","Pickup"]])"));
		const auto loaded = PhysicsLayers::FromJson(data, error);
		REQUIRE(loaded.has_value());
		CHECK(*loaded == layers);
		CHECK(PhysicsLayers::FromJson(nlohmann::json::object(), error) == PhysicsLayers());

		const char* invalid[] = {
			R"([])",
			R"({"Names": ["Default"], "Extra": 1})",
			R"({"Names": []})",
			R"({"Names": ["Player"]})",
			R"({"Names": ["Default", "A", "A"]})",
			R"({"Names": ["Default", 3]})",
			R"({"Names": ["Default", ""]})",
			R"({"IgnoredPairs": {}})",
			R"({"IgnoredPairs": [["Default"]]})",
			R"({"IgnoredPairs": [["Default", 1]]})",
			R"({"IgnoredPairs": [["Default", "Ghost"]]})",
		};
		for (const char* text : invalid)
		{
			INFO(text);
			error.clear();
			CHECK_FALSE(PhysicsLayers::FromJson(ParseJson(text), error).has_value());
			CHECK_FALSE(error.empty());
		}
		nlohmann::json tooMany = { { "Names", nlohmann::json::array({ "Default" }) } };
		for (uint32_t i = 1; i <= PhysicsLayers::MaxLayers; i++)
			tooMany["Names"].push_back("L" + std::to_string(i));
		CHECK_FALSE(PhysicsLayers::FromJson(tooMany, error).has_value());
	}

	TEST_CASE("Projects save and load their physics layers")
	{
		BasaltTest::TempProject temp("PhysicsLayersProject");
		REQUIRE(temp.IsValid());
		const Ref<Project>& project = Project::GetActive();
		std::string error;
		REQUIRE(project->GetConfig().Physics.Add("Player", error));
		project->GetConfig().Physics.SetCollides(1, 1, false);
		REQUIRE(project->Save(error));

		const Ref<Project> loaded = Project::Load(temp.GetDirectory(), error);
		REQUIRE(loaded);
		CHECK(loaded->GetConfig().Physics == project->GetConfig().Physics);

		temp.WriteFile(Project::FileName, R"({"Name": "Bad", "PhysicsLayers": {"Names": ["Default", "Default"]}})");
		CHECK_FALSE(Project::Load(temp.GetDirectory(), error));
		CHECK(error.find("duplicate") != std::string::npos);
	}
}

TEST_SUITE("Physics")
{
	TEST_CASE("A static MeshCollider collides by the triangles of the entity's mesh")
	{
		Scene scene;
		// A plane has no volume, so only its triangles can hold the box up.
		Entity floor = scene.CreateEntity("Floor");
		floor.GetTransform().Scale = { 20.0f, 1.0f, 20.0f };
		floor.AddComponent<MeshComponent>().Mesh = "builtin://Plane";
		floor.AddComponent<RigidBodyComponent>();
		floor.AddComponent<MeshColliderComponent>();
		Entity box = CreateBox(scene, { 3.0f, 2.0f, -2.0f });

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		CHECK(physics.GetBodyCount() == 2);
		Simulate(scene, 2.0f);
		CHECK(box.GetTransform().Translation.y == doctest::Approx(0.5f).epsilon(0.02));

		// Scaled by the entity: the plane reaches x = 10, not 0.5.
		CHECK(physics.Raycast({ 9.5f, 5.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 10.0f)->EntityID == floor.GetUUID());
		CHECK_FALSE(physics.Raycast({ 10.5f, 5.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 10.0f).has_value());

		// Changing the mesh the collider borrows rebuilds the body.
		floor.GetComponent<MeshComponent>().Mesh = "builtin://Cube";
		floor.AddOrReplaceComponent<MeshComponent>(floor.GetComponent<MeshComponent>());
		Simulate(scene, Step);
		const auto hit = physics.Raycast({ -9.0f, 5.0f, 9.0f }, { 0.0f, -1.0f, 0.0f }, 10.0f);
		REQUIRE(hit.has_value());
		CHECK(hit->Point.y == doctest::Approx(0.5f).epsilon(0.01));
		scene.OnSimulationStop();
	}

	TEST_CASE("A dynamic MeshCollider uses the convex hull of the mesh")
	{
		Scene scene;
		CreateGround(scene);
		// Cylinder: radius 0.5, height 1, so its hull stands on its flat end with the centre 0.5 up.
		Entity can = scene.CreateEntity("Can");
		can.GetTransform().Translation = { 0.0f, 3.0f, 0.0f };
		can.GetTransform().Scale = { 1.0f, 2.0f, 1.0f };
		can.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		can.AddComponent<MeshColliderComponent>().Mesh = "builtin://Cylinder";

		// A plane's hull is flat (no volume); it still gets a usable mass and lands.
		Entity tile = scene.CreateEntity("Tile");
		tile.GetTransform().Translation = { 3.0f, 2.0f, 0.0f };
		tile.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		tile.AddComponent<MeshColliderComponent>().Mesh = "builtin://Plane";

		scene.OnSimulationStart();
		CHECK(scene.GetPhysicsWorld()->GetBodyCount() == 3);
		Simulate(scene, 3.0f);
		CHECK(can.GetTransform().Translation.y == doctest::Approx(1.0f).epsilon(0.02));
		CHECK(TiltDegrees(can.GetTransform().Rotation, { 0.0f, 1.0f, 0.0f }) < 1.0f);
		CHECK(std::isfinite(tile.GetTransform().Translation.y));
		CHECK(tile.GetTransform().Translation.y == doctest::Approx(0.0f).epsilon(0.1));
		scene.OnSimulationStop();
	}

	TEST_CASE("Convex MeshColliders fill the hull; kinematic triangle meshes get a body")
	{
		Scene scene;
		// Convex on a static body: a solid hull instead of the sphere's hollow triangle shell.
		Entity solid = scene.CreateEntity("Solid");
		solid.AddComponent<RigidBodyComponent>();
		auto& collider = solid.AddComponent<MeshColliderComponent>();
		collider.Mesh = "builtin://Sphere";
		collider.Convex = true;
		Entity mover = scene.CreateEntity("Mover");
		mover.GetTransform().Translation = { 5.0f, 0.0f, 0.0f };
		mover.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		mover.AddComponent<MeshColliderComponent>().Mesh = "builtin://Cube";

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		CHECK(physics.GetBodyCount() == 2);
		const auto top = physics.Raycast({ 0.0f, 5.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 10.0f);
		REQUIRE(top.has_value());
		CHECK(top->Point.y == doctest::Approx(0.5f).epsilon(0.02));
		// A ray starting inside a convex hull hits it at once; inside a hollow triangle mesh it would not.
		const auto inside = physics.Raycast({ 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, 0.1f);
		CHECK((inside.has_value() && inside->EntityID == solid.GetUUID()));
		CHECK(physics.Raycast({ 5.0f, 5.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 10.0f)->EntityID == mover.GetUUID());
		scene.OnSimulationStop();
	}

	TEST_CASE("Mesh collider shapes are cooked once per mesh and shared across entities and worlds")
	{
		// Drop any shape an earlier test cooked for these meshes.
		AssetManager::Reload("builtin://Capsule");
		Scene scene;
		for (int i = 0; i < 3; i++)
		{
			Entity pillar = scene.CreateEntity("Pillar");
			pillar.GetTransform().Translation = { 2.0f * static_cast<float>(i), 1.0f, 0.0f };
			pillar.GetTransform().Scale = glm::vec3(1.0f + static_cast<float>(i));
			pillar.AddComponent<MeshComponent>().Mesh = "builtin://Capsule";
			pillar.AddComponent<RigidBodyComponent>();
			pillar.AddComponent<MeshColliderComponent>();
		}

		const uint64_t before = PhysicsWorld::GetMeshShapeCookCount();
		scene.OnSimulationStart();
		CHECK(scene.GetPhysicsWorld()->GetBodyCount() == 3);
		CHECK(PhysicsWorld::GetMeshShapeCookCount() == before + 1);
		scene.OnSimulationStop();
		scene.OnSimulationStart();
		CHECK(PhysicsWorld::GetMeshShapeCookCount() == before + 1);

		// The hull is a different shape; a reloaded mesh is cooked again.
		Entity rolling = scene.CreateEntity("Rolling");
		rolling.AddComponent<MeshComponent>().Mesh = "builtin://Capsule";
		rolling.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		rolling.AddComponent<MeshColliderComponent>();
		Simulate(scene, Step);
		CHECK(PhysicsWorld::GetMeshShapeCookCount() == before + 2);
		scene.OnSimulationStop();
		AssetManager::Reload("builtin://Capsule");
		scene.OnSimulationStart();
		CHECK(PhysicsWorld::GetMeshShapeCookCount() == before + 4);
		scene.OnSimulationStop();
	}

	TEST_CASE("Invalid MeshColliders create no body and warn once")
	{
		Scene scene;
		Entity missing = scene.CreateEntity("Missing");
		missing.AddComponent<RigidBodyComponent>();
		missing.AddComponent<MeshColliderComponent>().Mesh = "builtin://Teapot";
		Entity badIndex = scene.CreateEntity("BadIndex");
		badIndex.AddComponent<RigidBodyComponent>();
		auto& collider = badIndex.AddComponent<MeshColliderComponent>();
		collider.Mesh = "builtin://Cube";
		collider.MeshIndex = 3;
		Entity unnamed = scene.CreateEntity("Unnamed");
		unnamed.AddComponent<RigidBodyComponent>();
		unnamed.AddComponent<MeshColliderComponent>();

		const uint64_t since = Log::GetHistory().GetTotalCount();
		scene.OnSimulationStart();
		for (int i = 0; i < 5; i++)
		{
			for (Entity entity : { missing, badIndex, unnamed })
				entity.AddOrReplaceComponent<MeshColliderComponent>(entity.GetComponent<MeshColliderComponent>());
			Simulate(scene, Step);
		}
		CHECK(scene.GetPhysicsWorld()->GetBodyCount() == 0);
		CHECK(CountMessages(since, "cannot load mesh 'builtin://Teapot'") == 1);
		CHECK(CountMessages(since, "has no mesh 3 (it has 1)") == 1);
		CHECK(CountMessages(since, "no Mesh and the entity has no MeshComponent") == 1);
		scene.OnSimulationStop();
	}

	TEST_CASE("Meshes that load but cannot be cooked warn once and are not cooked again")
	{
		BasaltTest::TempProject project("PhysicsDegenerateMesh");
		REQUIRE(project.IsValid());
		// Three points on a line: no triangle survives Jolt's degenerate-triangle removal and no hull exists.
		const std::string line = WriteMeshGltf(project, "Line.gltf", { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f }, { 0, 1, 2 });
		const float nan = std::numeric_limits<float>::quiet_NaN();
		const std::string broken = WriteMeshGltf(project, "Broken.gltf", { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, nan, 1.0f }, { 0, 1, 2 });
		REQUIRE_MESSAGE(AssetManager::GetMesh(line), AssetManager::GetError(line));
		REQUIRE_MESSAGE(AssetManager::GetMesh(broken), AssetManager::GetError(broken));

		Scene scene;
		auto addCollider = [&](const char* name, const std::string& mesh, bool convex) {
			Entity entity = scene.CreateEntity(name);
			entity.AddComponent<RigidBodyComponent>();
			auto& collider = entity.AddComponent<MeshColliderComponent>();
			collider.Mesh = mesh;
			collider.Convex = convex;
			return entity;
		};
		const std::vector<Entity> entities = { addCollider("Triangles", line, false), addCollider("Hull", line, true), addCollider("Broken", broken, false) };

		const uint64_t since = Log::GetHistory().GetTotalCount();
		const uint64_t cooked = PhysicsWorld::GetMeshShapeCookCount();
		scene.OnSimulationStart();
		for (int i = 0; i < 5; i++)
		{
			for (Entity entity : entities)
				entity.AddOrReplaceComponent<MeshColliderComponent>(entity.GetComponent<MeshColliderComponent>());
			Simulate(scene, Step);
		}
		CHECK(scene.GetPhysicsWorld()->GetBodyCount() == 0);
		// The failures are cached like shapes.
		CHECK(PhysicsWorld::GetMeshShapeCookCount() == cooked + 3);
		CHECK(CountMessages(since, "'Triangles': MeshCollider ignored: cannot build a triangle mesh") == 1);
		CHECK(CountMessages(since, "'Hull': MeshCollider ignored: cannot build a convex hull") == 1);
		CHECK(CountMessages(since, "'Broken': MeshCollider ignored: the mesh has a non-finite vertex position") == 1);
		scene.OnSimulationStop();
	}

	TEST_CASE("Dynamic MeshColliders ask for Convex, triggers use the hull, and MeshIndex picks the mesh")
	{
		Scene scene;
		Entity rock = scene.CreateEntity("Rock");
		rock.GetTransform().Translation = { 0.0f, 5.0f, 0.0f };
		rock.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		rock.AddComponent<MeshColliderComponent>().Mesh = "builtin://Cube";
		Entity convexRock = scene.CreateEntity("ConvexRock");
		convexRock.GetTransform().Translation = { 5.0f, 5.0f, 0.0f };
		convexRock.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		auto& convexCollider = convexRock.AddComponent<MeshColliderComponent>();
		convexCollider.Mesh = "builtin://Cube";
		convexCollider.Convex = true;

		// A trigger zone made from a closed mesh covers its inside, not just its surface.
		Entity zone = scene.CreateEntity("Zone");
		zone.GetTransform().Translation = { 0.0f, 0.0f, 20.0f };
		zone.GetTransform().Scale = glm::vec3(4.0f);
		auto& zoneBody = zone.AddComponent<RigidBodyComponent>();
		zoneBody.IsTrigger = true;
		zone.AddComponent<MeshColliderComponent>().Mesh = "builtin://Cube";

		// A MeshIndex of its own is ignored when the collider borrows the MeshComponent's mesh.
		Entity borrowed = scene.CreateEntity("Borrowed");
		borrowed.GetTransform().Translation = { 0.0f, 0.0f, -20.0f };
		borrowed.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
		borrowed.AddComponent<RigidBodyComponent>();
		borrowed.AddComponent<MeshColliderComponent>().MeshIndex = 1;

		const uint64_t since = Log::GetHistory().GetTotalCount();
		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		CHECK(physics.GetBodyCount() == 4);
		CHECK(CountMessages(since, "'Rock': a dynamic body collides by the convex hull of its MeshCollider; set Convex") == 1);
		CHECK(CountMessages(since, "'ConvexRock'") == 0);
		CHECK(CountMessages(since, "'Borrowed': MeshCollider MeshIndex is ignored without its own Mesh") == 1);
		CHECK(physics.OverlapSphere({ 0.0f, 0.0f, 20.0f }, 0.1f, { .IncludeTriggers = true }) == std::vector<UUID>{ zone.GetUUID() });
		scene.OnSimulationStop();
	}

	TEST_CASE("MeshColliders keep a mirroring scale and hull only the drawn vertices")
	{
		BasaltTest::TempProject project("PhysicsOffsetCube");
		REQUIRE(project.IsValid());
		const std::string key = WriteOffsetCubeGltf(project);
		REQUIRE_MESSAGE(AssetManager::GetMesh(key), AssetManager::GetError(key));

		Scene scene;
		Entity mirrored = scene.CreateEntity("Mirrored");
		mirrored.GetTransform().Scale = { -1.0f, 1.0f, 1.0f };
		mirrored.AddComponent<RigidBodyComponent>();
		mirrored.AddComponent<MeshColliderComponent>().Mesh = key;
		Entity hull = scene.CreateEntity("Hull");
		hull.GetTransform().Translation = { 0.0f, 0.0f, 10.0f };
		hull.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		auto& collider = hull.AddComponent<MeshColliderComponent>();
		collider.Mesh = key;
		collider.Convex = true;

		scene.OnSimulationStart();
		PhysicsWorld& physics = *scene.GetPhysicsWorld();
		const glm::vec3 down(0.0f, -1.0f, 0.0f);
		// Mirrored in X, the cube spans x = -1.5..-0.5, like the rendered mesh.
		const auto top = physics.Raycast({ -1.0f, 5.0f, 0.0f }, down, 10.0f);
		REQUIRE(top.has_value());
		CHECK(top->EntityID == mirrored.GetUUID());
		CHECK(top->Point.y == doctest::Approx(0.5f).epsilon(0.01));
		CHECK_FALSE(physics.Raycast({ 1.0f, 5.0f, 0.0f }, down, 10.0f).has_value());
		// The unused vertex at x = 10 does not stretch the hull.
		CHECK(physics.Raycast({ 1.0f, 5.0f, 10.0f }, down, 10.0f)->EntityID == hull.GetUUID());
		CHECK_FALSE(physics.Raycast({ 5.0f, 5.0f, 10.0f }, down, 10.0f).has_value());
		scene.OnSimulationStop();
	}
}
