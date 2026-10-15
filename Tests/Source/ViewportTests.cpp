#include <doctest/doctest.h>

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
	for (const auto& bad : { nlohmann::json{ { "space", "world" } }, nlohmann::json{ { "translationStep", 0 } }, nlohmann::json{ { "rotationStep", 361 } }, nlohmann::json{ { "scaleStep", -1 } }, nlohmann::json{ { "grid", nullptr } }, nlohmann::json{ { "snap", true }, { "typo", true } } })
	{
		CHECK_FALSE(settings.Apply(bad, error));
		CHECK(settings.Get() == defaults);
		CHECK_FALSE(error.empty());
	}
	CHECK(settings.Apply({ { "space", "World" }, { "operation", "Scale" }, { "translationStep", 2.5 }, { "rotationStep", 30 }, { "scaleStep", 0.25 }, { "lightGizmos", false }, { "debugLines", false } }, error));
	CHECK(settings.Get()["space"] == "World");
	CHECK(settings.Get()["translationStep"] == 2.5);
	CHECK(settings.Get()["cameraGizmos"] == true);
	CHECK(settings.Get()["grid"] == true);
	CHECK_FALSE(settings.Apply({ { "translationStep", std::numeric_limits<double>::infinity() } }, error));
	CHECK(settings.Get()["translationStep"] == 2.5);
	CHECK(CommandSchema::Validate(EditorCommandSchemas::Pick(), { { "x", 0 }, { "y", 100 }, { "select", false } }, error));
	CHECK_FALSE(CommandSchema::Validate(EditorCommandSchemas::Pick(), { { "x", -1 }, { "y", 0 } }, error));
	CHECK_FALSE(CommandSchema::Validate(EditorCommandSchemas::Pick(), { { "x", 0.5 }, { "y", 0 } }, error));
	CHECK(CommandSchema::Validate(EditorCommandSchemas::VertexSnap(), { { "source", { 0, 0 } }, { "target", { 1, 2 } } }, error));
	CHECK_FALSE(CommandSchema::Validate(EditorCommandSchemas::VertexSnap(), { { "source", { 0, 0 } } }, error));
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
