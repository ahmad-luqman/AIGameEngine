#include "Basalt/Animation/AnimationPose.h"
#include "Basalt/Animation/AnimationSystem.h"
#include "Basalt/Asset/MeshImporter.h"
#include "Basalt/Renderer/DrawBatch.h"
#include "Basalt/Scene/ComponentRegistry.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <filesystem>
#include <limits>

using namespace Basalt;

TEST_CASE("Draw batching requires identical state and contiguous instance records")
{
	int mesh = 0, material = 0, other = 0;
	DrawBatchKey key{ &mesh, &material, 3, 7, 12, false, false };
	CHECK(CanInstanceDraw(key, 10, 4, key, 14));
	CHECK_FALSE(CanInstanceDraw(key, 10, 4, key, 15));
	CHECK_FALSE(CanInstanceDraw(key, UINT32_MAX, 1, key, 0));
	for (int field = 0; field < 7; ++field)
	{
		auto changed = key;
		switch (field)
		{
			case 0:
				changed.Mesh = &other;
				break;
			case 1:
				changed.MaterialSet = &other;
				break;
			case 2:
				++changed.BaseIndex;
				break;
			case 3:
				++changed.BaseVertex;
				break;
			case 4:
				++changed.IndexCount;
				break;
			case 5:
				changed.DoubleSided = true;
				break;
			case 6:
				changed.AlphaTested = true;
				break;
			default:
				FAIL("Unexpected batch key field");
				break;
		}
		CHECK_FALSE(CanInstanceDraw(key, 10, 4, changed, 14));
		CHECK((key < changed) != (changed < key));
	}
}

TEST_CASE("Cached joint influence bounds contain exact skinning under independent affine transforms")
{
	MeshSource source;
	std::vector<Vertex> vertices(96);
	for (uint32_t i = 0; i < vertices.size(); ++i)
	{
		auto& vertex = vertices[i];
		vertex.Position = { static_cast<float>(i % 7) - 3, static_cast<float>(i % 13), static_cast<float>(i % 5) - 2 };
		vertex.Joints = { i % 4, (i + 1) % 4, (i + 2) % 4, (i + 3) % 4 };
		// Include non-unit positive sums to exercise homogeneous weight bounds.
		vertex.Weights = i == 0 ? glm::vec4(0) : glm::vec4(.1f, .2f, .3f, .4f) * (i % 2 ? 1.0f : .999f);
	}
	source.AddSubmesh(vertices, {}, 0);
	const auto& submesh = source.Submeshes[0];
	CHECK(submesh.JointBounds.size() == 4);
	for (uint32_t frame = 0; frame < 30; ++frame)
	{
		std::vector<glm::mat4> palette;
		for (uint32_t joint = 0; joint < 4; ++joint)
		{
			auto matrix = glm::translate(glm::mat4(1), glm::vec3(static_cast<float>(joint), -static_cast<float>(frame), 3));
			matrix = glm::rotate(matrix, static_cast<float>(frame + joint) * .11f, glm::normalize(glm::vec3(1, 2, 3)));
			matrix = glm::scale(matrix, glm::vec3(joint % 2 ? -2.0f : .001f, 3, .7f));
			palette.push_back(matrix);
		}
		const auto exact = SkinBounds(source, submesh, palette);
		const auto cached = CachedSkinBounds(source, submesh, palette);
		CHECK(glm::all(glm::lessThanEqual(cached.Min, exact.Min)));
		CHECK(glm::all(glm::greaterThanEqual(cached.Max, exact.Max)));
	}
}

TEST_CASE("Distance animation LOD holds poses while clocks advance and catches up near the camera")
{
	Scene scene;
	auto camera = scene.CreateEntity("Camera");
	camera.AddComponent<CameraComponent>();
	auto rig = scene.CreateEntity("Rig");
	auto& animator = rig.AddComponent<AnimatorComponent>();
	animator.Mesh = (std::filesystem::path(BASALT_TEST_DATA_DIR) / "Animation/TwoJoint.gltf").string();
	animator.LODDistance = 10;
	animator.LODInterval = 3;
	rig.GetTransform().Translation.x = 100;
	scene.OnSimulationStart();
	auto* system = scene.GetAnimationSystem();
	REQUIRE(system);
	scene.OnUpdate(.1f);
	const auto first = *system->GetPose(rig);
	scene.OnUpdate(.1f);
	CHECK(*system->GetPose(rig) == first);
	CHECK(system->GetTime(rig) == doctest::Approx(.2f));
	scene.OnUpdate(.1f);
	CHECK(*system->GetPose(rig) == first);
	scene.OnUpdate(.1f);
	CHECK(*system->GetPose(rig) != first);
	const auto far = *system->GetPose(rig);
	camera.GetTransform().Translation.x = 100;
	scene.OnUpdate(.1f);
	CHECK(*system->GetPose(rig) != far);
	REQUIRE(system->Seek(rig, 1));
	const auto sought = *system->GetPose(rig);
	CHECK(sought != far);
	scene.OnSimulationStop();
}

TEST_CASE("Crowd pose jobs match a serial single-rig scene through fades and layers")
{
	Scene crowd, serial;
	const auto fixture = (std::filesystem::path(BASALT_TEST_DATA_DIR) / "Animation/TwoJoint.gltf").string();
	auto reference = serial.CreateEntity("Reference");
	auto& config = reference.AddComponent<AnimatorComponent>();
	config.Mesh = fixture;
	AnimationLayer layer;
	layer.Name = "Overlay";
	layer.Clip = "Step";
	layer.Weight = .3f;
	config.Layers.push_back(layer);
	std::vector<Entity> rigs;
	for (uint32_t i = 0; i < 80; ++i)
	{
		auto rig = crowd.CreateEntity("Crowd");
		rig.AddComponent<AnimatorComponent>() = config;
		rigs.push_back(rig);
	}
	crowd.OnSimulationStart();
	serial.OnSimulationStart();
	for (uint32_t frame = 0; frame < 25; ++frame)
	{
		if (frame == 8)
		{
			REQUIRE(serial.GetAnimationSystem()->CrossFade(reference, "Cubic", .3f));
			for (auto rig : rigs)
				REQUIRE(crowd.GetAnimationSystem()->CrossFade(rig, "Cubic", .3f));
		}
		crowd.OnUpdate(.05f);
		serial.OnUpdate(.05f);
		for (auto rig : rigs)
			CHECK(*crowd.GetAnimationSystem()->GetPose(rig) == *serial.GetAnimationSystem()->GetPose(reference));
	}
	crowd.OnSimulationStop();
	serial.OnSimulationStop();
}

TEST_CASE("Animation LOD configuration validates and round trips through the component registry")
{
	const auto* info = ComponentRegistry::Find("Animator");
	REQUIRE(info);
	Scene scene;
	auto rig = scene.CreateEntity("Rig");
	rig.AddComponent<AnimatorComponent>();
	std::string error;
	for (const auto& invalid : std::vector<nlohmann::json>{ { { "LODDistance", -1 } }, { { "LODInterval", 0 } }, { { "LODInterval", 61 } }, { { "LODDistance", "far" } } })
		CHECK_FALSE(info->Deserialize(rig, invalid, error));
	REQUIRE(info->Deserialize(rig, { { "LODDistance", 20 }, { "LODInterval", 4 } }, error));
	CHECK(info->Serialize(rig)["LODDistance"] == 20);
	CHECK(info->Serialize(rig)["LODInterval"] == 4);
}

TEST_CASE("Distant LOD preserves animation event crossings and has no effect without a camera")
{
	Scene scene;
	auto camera = scene.CreateEntity("Camera");
	camera.AddComponent<CameraComponent>();
	auto rig = scene.CreateEntity("Rig");
	auto& animator = rig.AddComponent<AnimatorComponent>();
	animator.Mesh = (std::filesystem::path(BASALT_TEST_DATA_DIR) / "Animation/TwoJoint.gltf").string();
	animator.LODDistance = 1;
	animator.LODInterval = 60;
	animator.Events = { { "Bend", .15f, "Marker" } };
	rig.GetTransform().Translation.x = 100;
	AnimationSystem system(scene);
	system.Update(.1f);
	const auto first = *system.GetPose(rig);
	system.Update(.1f);
	CHECK(*system.GetPose(rig) == first);
	const auto events = system.TakeEvents();
	REQUIRE(events.size() == 1);
	CHECK(events[0].Name == "Marker");
	CHECK(system.GetTime(rig) == doctest::Approx(.2f));
	camera.RemoveComponent<CameraComponent>();
	system.Update(.1f);
	CHECK(*system.GetPose(rig) != first);
	const auto current = *system.GetPose(rig);
	camera.AddComponent<CameraComponent>();
	system.Update(0);
	CHECK(*system.GetPose(rig) == current);
	system.Pause(rig, true);
	system.Update(.1f);
	CHECK(system.GetTime(rig) == doctest::Approx(.3f));
	CHECK(*system.GetPose(rig) == current);
}

TEST_CASE("Distance pose LOD keeps root displacement identical to full-rate animation")
{
	Scene scene;
	auto camera = scene.CreateEntity("Camera");
	camera.AddComponent<CameraComponent>();
	auto distant = scene.CreateEntity("Distant"), reference = scene.CreateEntity("Reference");
	auto& config = distant.AddComponent<AnimatorComponent>();
	config.Mesh = (std::filesystem::path(BASALT_TEST_DATA_DIR) / "Animation/TwoJoint.gltf").string();
	config.Clip = "Cubic";
	config.RootMotion = true;
	config.RootJoint = "RootJoint";
	config.LODDistance = 1;
	config.LODInterval = 5;
	reference.AddComponent<AnimatorComponent>() = config;
	reference.GetComponent<AnimatorComponent>().LODInterval = 1;
	distant.GetTransform().Translation.x = reference.GetTransform().Translation.x = 100;
	AnimationSystem system(scene);
	for (uint32_t frame = 0; frame < 30; ++frame)
	{
		system.Update(.1f);
		CHECK(distant.GetTransform().Translation == reference.GetTransform().Translation);
		CHECK(distant.GetTransform().Rotation == reference.GetTransform().Rotation);
		CHECK(system.GetTime(distant) == system.GetTime(reference));
	}
	CHECK(distant.GetTransform().Translation.x != 100);
}
