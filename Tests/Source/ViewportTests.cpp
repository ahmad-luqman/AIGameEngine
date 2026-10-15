#include <doctest/doctest.h>

#include <Basalt/Animation/AnimationSystem.h>
#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Automation/CommandSchema.h>
#include <Basalt/Automation/EditorCommandSchemas.h>
#include <Basalt/Automation/ViewportSettings.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>
#include <Basalt/Scene/VertexSnap.h>

#include <glm/gtc/matrix_transform.hpp>

#include <limits>

using namespace Basalt;

TEST_CASE("Viewport preferences validate patches atomically and retain independent overlay switches")
{
	ViewportSettings settings;
	std::string error;
	REQUIRE(CommandSchema::CheckDefinition(ViewportSettings::Schema(), error));
	const auto defaults = settings.Get();
	CHECK(CommandSchema::Validate(ViewportSettings::Schema(), defaults, error));
	CHECK(defaults.size() == ViewportSettings::Schema()["properties"].size());
	for (const auto& [key, value] : defaults.items())
		CHECK(ViewportSettings::Schema()["properties"].contains(key));
	for (const auto& bad : { nlohmann::json::array(), nlohmann::json{ { "grid", 1 } }, nlohmann::json{ { "translationStep", "1" } }, nlohmann::json{ { "space", "world" } }, nlohmann::json{ { "translationStep", 0 } }, nlohmann::json{ { "rotationStep", 361 } }, nlohmann::json{ { "scaleStep", -1 } }, nlohmann::json{ { "grid", nullptr } }, nlohmann::json{ { "snap", true }, { "typo", true } } })
	{
		CHECK_FALSE(settings.Apply(bad, error));
		CHECK(settings.Get() == defaults);
		CHECK_FALSE(error.empty());
	}
	CHECK(settings.Apply({ { "space", "World" }, { "operation", "Scale" }, { "translationStep", 2.5 }, { "rotationStep", 30 }, { "scaleStep", 0.25 }, { "lightGizmos", false }, { "debugLines", false } }, error));
	CHECK(settings.Options().Space == GizmoSpace::World);
	CHECK(settings.Options().Operation == GizmoOperation::Scale);
	CHECK(settings.Options().StepFor(GizmoOperation::Scale) == doctest::Approx(.25));
	CHECK(settings.Get()["translationStep"] == 2.5);
	CHECK(settings.Get()["cameraGizmos"] == true);
	CHECK(settings.Get()["grid"] == true);
	CHECK_FALSE(settings.Apply({ { "translationStep", std::numeric_limits<double>::infinity() } }, error));
	CHECK(settings.Get()["translationStep"] == 2.5);
	CHECK(settings.Apply({ { "rotationStep", 360 } }, error));
	CHECK(settings.Options().StepFor(GizmoOperation::Rotate) == 360);
}

TEST_CASE("Viewport command schemas distinguish integer output pixels from logical vertex coordinates")
{
	std::string error;
	CHECK(CommandSchema::Validate(EditorCommandSchemas::Pick(), { { "x", 0 }, { "y", 100 }, { "select", false } }, error));
	CHECK_FALSE(CommandSchema::Validate(EditorCommandSchemas::Pick(), { { "x", -1 }, { "y", 0 } }, error));
	CHECK_FALSE(CommandSchema::Validate(EditorCommandSchemas::Pick(), { { "x", 0.5 }, { "y", 0 } }, error));
	CHECK(CommandSchema::Validate(EditorCommandSchemas::VertexSnap(), { { "source", { 0, 0 } }, { "target", { 1, 2 } } }, error));
	CHECK_FALSE(CommandSchema::Validate(EditorCommandSchemas::VertexSnap(), { { "source", { 0, 0 } } }, error));
	CHECK_FALSE(CommandSchema::Validate(EditorCommandSchemas::VertexSnap(), { { "source", { 0, 0, 0 } }, { "target", { 1, 2 } } }, error));
}

TEST_CASE("Vertex snapping uses projected geometry and preserves parented rotation and scale")
{
	Scene scene;
	auto source = scene.CreateEntity("Source");
	source.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	const auto view = glm::lookAtRH(glm::vec3(0, 0, 4), glm::vec3(0), glm::vec3(0, 1, 0));
	const auto projection = glm::orthoRH_ZO(-2.0f, 2.0f, -2.0f, 2.0f, 0.1f, 10.0f);
	const glm::vec2 size(200, 200);
	const auto vertex = FindViewportVertex(source, projection * view, size, { 125, 75 });
	REQUIRE(vertex);
	CHECK(vertex->World.x == doctest::Approx(0.5));
	CHECK(vertex->World.y == doctest::Approx(0.5));
	CHECK(vertex->World.z == doctest::Approx(0.5));
	CHECK_FALSE(FindViewportVertex(source, projection * view, size, { 100, 100 }));
	CHECK(FindViewportVertex(source, projection * view, size, { 141, 75 }, 16));
	CHECK_FALSE(FindViewportVertex(source, projection * view, size, { 141, 75 }, 15.99f));
	CHECK_FALSE(FindViewportVertex(source, projection * view, { 0, 200 }, { 125, 75 }));
	auto child = scene.CreateEntity("Child");
	child.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	REQUIRE(scene.SetParent(child, source));
	CHECK_FALSE(FindVertexSnapTarget(scene, source, projection * view, size, { 125, 75 }));
	auto target = scene.CreateEntity("Target");
	target.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	target.GetTransform().Translation.x = 1;
	const auto hit = FindVertexSnapTarget(scene, source, projection * view, size, { 175, 75 });
	REQUIRE(hit);
	CHECK(hit->EntityId == target.GetUUID());
	auto parent = scene.CreateEntity("Parent");
	parent.GetTransform().Scale = { 2, 3, 4 };
	parent.GetTransform().Rotation = glm::quat(glm::radians(glm::vec3(20, 35, 10)));
	REQUIRE(scene.SetParent(source, parent, false));
	const glm::vec3 from(scene.GetWorldTransform(source) * glm::vec4(0.5f, 0.5f, 0.5f, 1));
	const glm::vec3 to(3, 2, -1);
	const auto translation = VertexSnapTranslation(scene, source, from, to);
	REQUIRE(translation);
	source.GetTransform().Translation = *translation;
	const glm::vec3 result(scene.GetWorldTransform(source) * glm::vec4(0.5f, 0.5f, 0.5f, 1));
	CHECK(glm::length(result - to) < 1e-5f);
	parent.GetTransform().Scale = glm::vec3(0.001f);
	CHECK(VertexSnapTranslation(scene, source, from, to));
	parent.GetTransform().Scale.x = 0;
	CHECK_FALSE(VertexSnapTranslation(scene, source, from, to));
	parent.GetTransform().Scale.x = 2;
	CHECK_FALSE(VertexSnapTranslation(scene, source, from, glm::vec3(std::numeric_limits<float>::infinity())));
	source.GetTransform().Translation = { 0, 0, 100 };
	CHECK_FALSE(FindViewportVertex(source, projection * view, size, { 125, 75 }));
}

TEST_CASE("Vertex snap targets resolve overlapping screen vertices by depth across entities")
{
	Scene scene;
	auto moved = scene.CreateEntity("Moved");
	auto far = scene.CreateEntityWithUUID(UUID(1), "Far");
	auto near = scene.CreateEntityWithUUID(UUID(2), "Near");
	for (Entity entity : { far, near })
		entity.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	near.GetTransform().Translation.z = 1;
	const auto vp = glm::orthoRH_ZO(-2.0f, 2.0f, -2.0f, 2.0f, .1f, 10.0f) * glm::lookAtRH(glm::vec3(0, 0, 4), glm::vec3(0), glm::vec3(0, 1, 0));
	const auto hit = FindVertexSnapTarget(scene, moved, vp, { 200, 200 }, { 125, 75 });
	REQUIRE(hit);
	CHECK(hit->EntityId == near.GetUUID());
}

TEST_CASE("Vertex snapping rejects perspective points behind the camera and uses an inclusive radius")
{
	Scene scene;
	auto entity = scene.CreateEntity("Cube");
	entity.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	entity.GetTransform().Translation.z = -2;
	const auto vp = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, .1f, 10.0f);
	const glm::vec2 pixel(100 + 100.0f / 3, 100 - 100.0f / 3);
	const auto hit = FindViewportVertex(entity, vp, { 200, 200 }, pixel);
	REQUIRE(hit);
	CHECK(hit->World.z == doctest::Approx(-1.5));
	CHECK(FindViewportVertex(entity, vp, { 200, 200 }, pixel + glm::vec2(16, 0), 16.001f));
	CHECK_FALSE(FindViewportVertex(entity, vp, { 200, 200 }, pixel + glm::vec2(16, 0), 15.99f));
	entity.GetTransform().Translation.z = 2;
	CHECK_FALSE(FindViewportVertex(entity, vp, { 200, 200 }, pixel));
	CHECK_FALSE(FindViewportVertex(entity, vp, { 200, 200 }, { -1, 100 }));
	CHECK_FALSE(FindViewportVertex(entity, vp, { 200, 200 }, { 200, 100 }));
}

TEST_CASE("Vertex snapping follows the current skinned pose and skips vertices without valid influences")
{
	Scene scene;
	auto rig = scene.CreateEntity("Rig");
	const std::string path = std::string(BASALT_TEST_DATA_DIR) + "/Animation/TwoJoint.gltf";
	rig.AddComponent<AnimatorComponent>().Mesh = path;
	const auto vp = glm::orthoRH_ZO(-2.0f, 2.0f, -2.0f, 2.0f, .1f, 10.0f) * glm::lookAtRH(glm::vec3(0, 1, 4), glm::vec3(0, 1, 0), glm::vec3(0, 1, 0));
	const auto rest = FindViewportVertex(rig, vp, { 200, 200 }, { 112.5f, 50 });
	REQUIRE(rest);
	CHECK(rest->World.x == doctest::Approx(.25));
	CHECK(rest->World.y == doctest::Approx(2));
	scene.OnSimulationStart();
	REQUIRE(scene.GetAnimationSystem()->Play(rig, "Bend"));
	REQUIRE(scene.GetAnimationSystem()->Seek(rig, 1));
	const auto posed = FindViewportVertex(rig, vp, { 200, 200 }, { 50, 87.5f });
	REQUIRE(posed);
	CHECK(posed->World.x == doctest::Approx(-1));
	CHECK(posed->World.y == doctest::Approx(1.25));
	CHECK_FALSE(FindViewportVertex(rig, vp, { 200, 200 }, rest->Screen));
	scene.OnSimulationStop();
	CHECK(FindViewportVertex(rig, vp, { 200, 200 }, rest->Screen));
	// Import already rejects these influences; defend against malformed in-memory geometry as well.
	auto mesh = AssetManager::GetMesh(path);
	REQUIRE(mesh);
	const auto saved = mesh->Vertices;
	for (auto& vertex : mesh->Vertices)
		vertex.Weights = glm::vec4(0);
	CHECK_FALSE(FindViewportVertex(rig, vp, { 200, 200 }, { 100, 150 }));
	mesh->Vertices = saved;
}

TEST_CASE("Vertex snapping ignores unused accessor vertices and follows indexed submesh ranges")
{
	Scene scene;
	auto entity = scene.CreateEntity("Indexed");
	entity.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	auto mesh = AssetManager::GetMesh("builtin://Cube");
	REQUIRE(mesh);
	REQUIRE(mesh->Submeshes.size() == 1);
	const auto savedVertices = mesh->Vertices;
	const auto savedIndices = mesh->Indices;
	const auto savedSubmesh = mesh->Submeshes[0];
	auto& submesh = mesh->Submeshes[0];
	const uint32_t unused = submesh.VertexCount;
	// Offset both ranges and place an unused vertex at the center of the visible face.
	mesh->Vertices.insert(mesh->Vertices.begin(), Vertex{});
	submesh.BaseVertex++;
	mesh->Indices.insert(mesh->Indices.begin(), 0);
	submesh.BaseIndex++;
	mesh->Vertices.push_back(Vertex{ .Position = { 0, 0, .5f } });
	submesh.VertexCount++;
	const auto vp = glm::orthoRH_ZO(-2.0f, 2.0f, -2.0f, 2.0f, .1f, 10.0f) * glm::lookAtRH(glm::vec3(0, 0, 4), glm::vec3(0), glm::vec3(0, 1, 0));
	CHECK_FALSE(FindViewportVertex(entity, vp, { 200, 200 }, { 100, 100 }));
	CHECK(FindViewportVertex(entity, vp, { 200, 200 }, { 125, 75 }));
	mesh->Indices.insert(mesh->Indices.end(), { unused, 0, 1 });
	submesh.IndexCount += 3;
	const auto referenced = FindViewportVertex(entity, vp, { 200, 200 }, { 100, 100 });
	CHECK(referenced.has_value());
	if (referenced)
		CHECK(referenced->World == glm::vec3(0, 0, .5f));
	mesh->Vertices = savedVertices;
	mesh->Indices = savedIndices;
	submesh = savedSubmesh;
}
