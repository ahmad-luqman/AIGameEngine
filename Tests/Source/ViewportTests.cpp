#include <doctest/doctest.h>

#include <Basalt/Animation/AnimationSystem.h>
#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Automation/CommandSchema.h>
#include <Basalt/Automation/EditorCommandSchemas.h>
#include <Basalt/Automation/GameView.h>
#include <Basalt/Automation/ViewportSettings.h>
#include <Basalt/Project/Project.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>
#include <Basalt/Scene/VertexSnap.h>

#include <glm/gtc/matrix_transform.hpp>

#include <limits>
#include <utility>

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

TEST_CASE("The Game view letterboxes the game resolution into the panel")
{
	// Wider panel: bars left and right.
	GameViewRect rect = FitLetterbox({ 1000.0f, 300.0f }, { 1600, 900 });
	CHECK(rect.Size.y == doctest::Approx(300.0f));
	CHECK(rect.Size.x == doctest::Approx(300.0f * 16.0f / 9.0f));
	CHECK(rect.Origin.x == doctest::Approx((1000.0f - rect.Size.x) * 0.5f));
	CHECK(rect.Origin.y == doctest::Approx(0.0f));
	// Taller panel: bars above and below.
	rect = FitLetterbox({ 400.0f, 1000.0f }, { 1600, 900 });
	CHECK(rect.Size.x == doctest::Approx(400.0f));
	CHECK(rect.Size.y == doctest::Approx(225.0f));
	CHECK(rect.Origin.y == doctest::Approx(387.5f));
	// Same aspect: no bars; a larger panel scales up.
	rect = FitLetterbox({ 3200.0f, 1800.0f }, { 1600, 900 });
	CHECK(rect.Origin == glm::vec2(0.0f));
	CHECK(rect.Size == glm::vec2(3200.0f, 1800.0f));
	// Portrait games keep their aspect too.
	rect = FitLetterbox({ 800.0f, 800.0f }, { 1080, 1920 });
	CHECK(rect.Size.x / rect.Size.y == doctest::Approx(1080.0f / 1920.0f));
	CHECK(rect.Size.y == doctest::Approx(800.0f));
	// Degenerate input never divides by zero.
	for (const auto& [panel, game] : { std::pair{ glm::vec2(0.0f, 100.0f), glm::uvec2(16, 9) }, std::pair{ glm::vec2(100.0f, -1.0f), glm::uvec2(16, 9) }, std::pair{ glm::vec2(100.0f), glm::uvec2(0, 9) } })
	{
		rect = FitLetterbox(panel, game);
		CHECK(rect.Size == glm::vec2(0.0f));
	}
}

TEST_CASE("The Game view gives the game the cursor it asks for while focused, and Escape hands it back")
{
	const auto decide = [](CursorMode requested, bool focused, bool hovered, bool escape, bool clicked, bool released) {
		return DecideGameCursor({ requested, focused, hovered, escape, clicked, released });
	};
	// Normal never captures anything and forgets an earlier release.
	GameCursorDecision d = decide(CursorMode::Normal, true, true, true, false, true);
	CHECK_FALSE(d.LockWindow);
	CHECK_FALSE(d.HideOverView);
	CHECK_FALSE(d.Released);
	CHECK_FALSE(d.ConsumeEscape);
	// Locked captures the window cursor only while the Game view has focus.
	CHECK(decide(CursorMode::Locked, true, false, false, false, false).LockWindow);
	CHECK_FALSE(decide(CursorMode::Locked, false, true, false, false, false).LockWindow);
	// Escape releases it (and is the editor's, not the game's) ...
	d = decide(CursorMode::Locked, true, true, true, false, false);
	CHECK_FALSE(d.LockWindow);
	CHECK(d.Released);
	CHECK(d.ConsumeEscape);
	// ... until the Game view is clicked (that click is the editor's too): a click elsewhere, or Escape again, does not
	// take it back, and the game gets the second Escape.
	d = decide(CursorMode::Locked, true, false, false, true, true);
	CHECK(d.Released);
	CHECK_FALSE(d.ConsumeClick);
	d = decide(CursorMode::Locked, true, true, true, false, true);
	CHECK(d.Released);
	CHECK_FALSE(d.ConsumeEscape);
	d = decide(CursorMode::Locked, true, true, false, true, true);
	CHECK_FALSE(d.Released);
	CHECK(d.ConsumeClick);
	CHECK(d.LockWindow);
	// Escape without focus is not the editor's to take.
	CHECK_FALSE(decide(CursorMode::Locked, false, false, true, false, false).ConsumeEscape);
	// Hidden never captures the window; it hides the cursor over the view while focused, and Escape stays the game's.
	d = decide(CursorMode::Hidden, true, true, false, false, false);
	CHECK_FALSE(d.LockWindow);
	CHECK(d.HideOverView);
	CHECK_FALSE(decide(CursorMode::Hidden, false, true, false, false, false).HideOverView);
	d = decide(CursorMode::Hidden, true, true, true, false, false);
	CHECK_FALSE(d.ConsumeEscape);
	CHECK_FALSE(d.Released);
	CHECK(d.HideOverView);
	// Never both.
	for (CursorMode mode : { CursorMode::Normal, CursorMode::Hidden, CursorMode::Locked })
		for (int bits = 0; bits < 32; bits++)
		{
			d = decide(mode, bits & 1, bits & 2, bits & 4, bits & 8, bits & 16);
			CHECK_FALSE((d.LockWindow && d.HideOverView));
		}
}

TEST_CASE("The Game view gives the game the keyboard while focused and the mouse while it is also over the image")
{
	const auto route = [](bool image, bool focused, bool hovered, bool over, bool locked) {
		return RouteGameView({ image, focused, hovered, over, locked });
	};
	GameViewRouting r = route(true, true, true, true, false);
	CHECK(r.Focus);
	CHECK(r.Pointer);
	// Hovered on the last frame but the mouse has left the image (a click on another panel): not the game's.
	CHECK_FALSE(route(true, true, true, false, false).Pointer);
	// Over the image but the view is not focused (the click that focuses it): neither.
	r = route(true, false, true, true, false);
	CHECK_FALSE(r.Focus);
	CHECK_FALSE(r.Pointer);
	// A locked cursor stays the game's wherever the virtual cursor wanders.
	CHECK(route(true, true, false, false, true).Pointer);
	// No image: nothing.
	r = route(false, true, true, true, true);
	CHECK_FALSE(r.Focus);
	CHECK_FALSE(r.Pointer);
	// The rectangle includes its minimum edge and excludes its maximum.
	CHECK(IsInsideRect({ 10.0f, 20.0f }, { 10.0f, 20.0f }, { 100.0f, 50.0f }));
	CHECK(IsInsideRect({ 109.9f, 69.9f }, { 10.0f, 20.0f }, { 100.0f, 50.0f }));
	CHECK_FALSE(IsInsideRect({ 110.0f, 30.0f }, { 10.0f, 20.0f }, { 100.0f, 50.0f }));
	CHECK_FALSE(IsInsideRect({ 50.0f, 70.0f }, { 10.0f, 20.0f }, { 100.0f, 50.0f }));
	CHECK_FALSE(IsInsideRect({ 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }));
}

TEST_CASE("The Game view renders at the project's window size unless a session override is set")
{
	GameViewSettings settings;
	CHECK(settings.Resolve(nullptr) == GameViewSettings::FallbackResolution);
	ProjectConfig config;
	config.WindowWidth = 1280;
	config.WindowHeight = 720;
	CHECK(settings.Resolve(&config) == glm::uvec2(1280, 720));
	// Hand-edited project files are clamped to what the runtime accepts.
	config.WindowWidth = 10;
	config.WindowHeight = 100000;
	CHECK(settings.Resolve(&config) == glm::uvec2(GameViewSettings::MinResolution, GameViewSettings::MaxResolution));

	std::string error;
	CHECK(settings.Apply({ { "resolution", { 640, 360 } } }, error));
	CHECK(settings.Resolve(&config) == glm::uvec2(640, 360));
	CHECK(settings.Apply({ { "focus", true } }, error));
	CHECK(settings.GetOverride() == glm::uvec2(640, 360));
	for (const nlohmann::json& bad : { nlohmann::json{ 63, 100 }, nlohmann::json{ 100, 16385 }, nlohmann::json{ 100 }, nlohmann::json{ 100, 100, 100 }, nlohmann::json("640x360"), nlohmann::json{ 1.5, 100 } })
	{
		CHECK_FALSE(settings.Apply({ { "resolution", bad } }, error));
		CHECK(error.find("resolution") != std::string::npos);
		CHECK(settings.GetOverride() == glm::uvec2(640, 360));
	}
	CHECK(settings.Apply({ { "resolution", nullptr } }, error));
	CHECK_FALSE(settings.GetOverride());
	CHECK(CommandSchema::CheckDefinition(GameViewSettings::Schema(), error));
}
