#include "EditorContext.h"

#include <Basalt/Automation/AutomationSession.h>
#include <Basalt/Automation/CommandRegistry.h>
#include <Basalt/Automation/EditorHistory.h>
#include <Basalt/Automation/EditorSelection.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>
#include <Basalt/Scene/SceneSerializer.h>

#include <doctest/doctest.h>
#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

using namespace Basalt;
using json = nlohmann::json;

namespace {
	json Matrix(const glm::mat4& matrix)
	{
		auto result = json::array();
		for (int c = 0; c < 4; ++c)
			for (int r = 0; r < 4; ++r)
				result.push_back(matrix[c][r]);
		return result;
	}
}

TEST_CASE("Editor history transactions keep labels, branch correctly and restore only on success")
{
	EditorHistory history;
	json state{ { "value", 0 } };
	const auto restore = [&](const json& next) { state = next; return true; };
	history.Reset(state);
	CHECK_FALSE(history.Undo(restore));
	REQUIRE(history.Begin("Move Selection"));
	CHECK_FALSE(history.Begin("nested"));
	for (int i = 1; i <= 10; ++i)
		history.Commit({ { "value", i } }, "intermediate");
	CHECK(history.GetSize() == 0);
	CHECK_FALSE(history.CanUndo());
	REQUIRE(history.End({ { "value", 10 } }));
	CHECK(history.GetSize() == 1);
	CHECK(history.UndoName() == "Move Selection");
	CHECK_FALSE(history.Undo([](const json&) { return false; }));
	CHECK(history.UndoName() == "Move Selection");
	REQUIRE(history.Undo(restore));
	CHECK(state["value"] == 0);
	CHECK(history.RedoName() == "Move Selection");
	history.Commit(state, "no-op preserves redo");
	CHECK(history.CanRedo());
	REQUIRE(history.Begin("Cancelled"));
	CHECK_FALSE(history.Cancel([](const json&) { return false; }));
	CHECK(history.IsActive());
	REQUIRE(history.Cancel(restore));
	CHECK(history.CanRedo());
	REQUIRE(history.Begin("No-op"));
	REQUIRE(history.End(state));
	CHECK(history.CanRedo());
	REQUIRE(history.Redo(restore));
	CHECK(state["value"] == 10);
	REQUIRE(history.Undo(restore));
	history.Commit({ { "value", 5 } }, "Branch");
	CHECK_FALSE(history.CanRedo());
	CHECK(history.GetSize() == 1);
	CHECK(history.UndoName() == "Branch");
	CHECK(EditorHistory::CommandName("component.set", { { "component", "Material" }, { "data", { { "AlbedoColor", { 1, 0, 0, 1 } } } } }) == "Set Material AlbedoColor");
}

TEST_CASE("Editor history patches preserve hierarchy ordering, settings, references and component lifecycles")
{
	Scene scene;
	auto parent = scene.CreateEntityWithUUID(UUID(90), "Parent");
	auto child = scene.CreateEntityWithUUID(UUID(2), "Child");
	auto other = scene.CreateEntityWithUUID(UUID(50), "Other");
	REQUIRE(scene.SetParent(child, parent, false));
	child.AddComponent<JointComponent>().ConnectedEntity = other.GetUUID();
	const json before = SceneSerializer::SerializeScene(scene);
	EditorHistory history;
	history.Reset(before);
	parent.AddComponent<MaterialComponent>();
	scene.DestroyEntity(child);
	scene.GetRendererSettings().Exposure = 2;
	const json after = SceneSerializer::SerializeScene(scene);
	history.Commit(after, "Complex Edit");
	json restored;
	const auto restore = [&](const json& value) {
		Scene loaded;
		std::string error;
		REQUIRE(SceneSerializer::DeserializeScene(loaded, value, error));
		restored = SceneSerializer::SerializeScene(loaded);
		return true;
	};
	REQUIRE(history.Undo(restore));
	CHECK(restored == before);
	REQUIRE(history.Redo(restore));
	CHECK(restored == after);
}

TEST_CASE("Editor history retains bounded patch payloads instead of whole scene snapshots")
{
	EditorHistory history;
	json state{ { "Entities", json::array() } };
	for (uint64_t i = 1; i <= 1000; ++i)
		state["Entities"].push_back({ { "ID", i }, { "Name", std::string(1024, 'x') }, { "Value", 0 } });
	history.Reset(state);
	for (size_t i = 1; i <= EditorHistory::MaxEntries + 10; ++i)
	{
		state["Entities"][500]["Value"] = i;
		history.Commit(state, "Set Value");
	}
	CHECK(history.GetSize() == EditorHistory::MaxEntries);
	CHECK(history.GetPatchBytes() < 100000);
	state["Entities"].erase(state["Entities"].begin());
	const size_t bytes = history.GetPatchBytes();
	history.Commit(state, "Delete First");
	CHECK(history.GetPatchBytes() < bytes + 5000);
	size_t undos = 0;
	while (history.Undo([](const json&) { return true; }))
		++undos;
	CHECK(undos == EditorHistory::MaxEntries);
	// An edit larger than the entire budget is deliberately not retained.
	history.Reset(json::object());
	history.Commit({ { "huge", std::string(EditorHistory::MaxPatchBytes + 1, 'x') } }, "Oversized");
	CHECK(history.GetPatchBytes() <= EditorHistory::MaxPatchBytes);
	CHECK_FALSE(history.CanUndo());
	history.Commit({ { "huge", "small" } }, "Shrink");
	CHECK(history.GetPatchBytes() <= EditorHistory::MaxPatchBytes);
}

TEST_CASE("Editor selection resolves atomically, orders the active entity and prunes stale UUIDs")
{
	CommandRegistry commands;
	RegisterEditorSelectionCommands(commands);
	AutomationSession session;
	auto a = session.GetScene()->CreateEntity("A");
	auto b = session.GetScene()->CreateEntity("B");
	const auto result = commands.Execute(session, "editor.select", { { "entities", { "A", "B", "A" } } });
	CHECK(result["entities"].size() == 2);
	CHECK(session.GetSelection() == b.GetUUID());
	CHECK_THROWS_AS(commands.Execute(session, "editor.select", { { "entities", { "A", "missing" } } }), CommandError);
	CHECK(session.GetSelections().size() == 2);
	commands.Execute(session, "editor.select", { { "entity", "B" }, { "mode", "toggle" } });
	CHECK(session.GetSelection() == a.GetUUID());
	commands.Execute(session, "editor.select", { { "entity", "B" }, { "mode", "add" } });
	CHECK(session.GetSelection() == b.GetUUID());
	session.GetScene()->DestroyEntity(b);
	session.PruneSelection();
	CHECK(session.GetSelections() == std::vector<UUID>{ a.GetUUID() });
	commands.Execute(session, "editor.select", { { "entity", nullptr } });
	CHECK(session.GetSelections().empty());
}

TEST_CASE("Editor group transforms move selected descendants once and reject a bad member atomically")
{
	CommandRegistry commands;
	RegisterEditorSelectionCommands(commands);
	AutomationSession session;
	Scene& scene = *session.GetScene();
	auto parent = scene.CreateEntity("Parent"), child = scene.CreateEntity("Child"), other = scene.CreateEntity("Other");
	REQUIRE(scene.SetParent(child, parent, false));
	child.GetTransform().Translation = { 1, 0, 0 };
	other.GetTransform().Translation = { 4, 0, 0 };
	session.SetSelections({ child.GetUUID(), parent.GetUUID(), other.GetUUID() });
	CHECK(SelectionRoots(scene, session.GetSelections()).size() == 2);
	commands.Execute(session, "editor.transform", { { "delta", Matrix(glm::translate(glm::mat4(1), { 2, 0, 0 })) } });
	CHECK(parent.GetTransform().Translation.x == doctest::Approx(2));
	CHECK(child.GetTransform().Translation.x == doctest::Approx(1));
	CHECK(scene.GetWorldTransform(child)[3].x == doctest::Approx(3));
	CHECK(other.GetTransform().Translation.x == doctest::Approx(6));
	const json before = SceneSerializer::SerializeScene(scene);
	glm::mat4 shear(1);
	shear[1][0] = .5f;
	CHECK_THROWS_AS(commands.Execute(session, "editor.transform", { { "delta", Matrix(shear) } }), CommandError);
	CHECK(SceneSerializer::SerializeScene(scene) == before);
	auto singular = scene.CreateEntity("Singular");
	singular.GetTransform().Scale.x = 0;
	REQUIRE(scene.SetParent(other, singular, false));
	const json invalidParent = SceneSerializer::SerializeScene(scene);
	CHECK_THROWS_AS(commands.Execute(session, "editor.transform", { { "delta", Matrix(glm::translate(glm::mat4(1), { 2, 0, 0 })) } }), CommandError);
	CHECK(SceneSerializer::SerializeScene(scene) == invalidParent);
}

TEST_CASE("Editor group rotation and scale share a pivot under transformed parents")
{
	CommandRegistry commands;
	RegisterEditorSelectionCommands(commands);
	AutomationSession session;
	Scene& scene = *session.GetScene();
	auto a = scene.CreateEntity("A"), b = scene.CreateEntity("B");
	a.GetTransform().Translation = { -1, 0, 0 };
	b.GetTransform().Translation = { 1, 0, 0 };
	session.SetSelections({ a.GetUUID(), b.GetUUID() });
	CHECK(glm::length(glm::vec3(SelectionPivot(scene, session.GetSelections(), false)[3])) == doctest::Approx(0));
	commands.Execute(session, "editor.transform", { { "delta", Matrix(glm::rotate(glm::mat4(1), glm::radians(90.0f), { 0, 0, 1 })) } });
	CHECK(a.GetTransform().Translation.y == doctest::Approx(-1));
	CHECK(b.GetTransform().Translation.y == doctest::Approx(1));
	commands.Execute(session, "editor.transform", { { "delta", Matrix(glm::scale(glm::mat4(1), glm::vec3(2))) } });
	CHECK(a.GetTransform().Translation.y == doctest::Approx(-2));
	CHECK(b.GetTransform().Scale.x == doctest::Approx(2));
	auto parent = scene.CreateEntity("Parent");
	parent.GetTransform().Scale = { 2, 3, 4 };
	parent.GetTransform().Rotation = glm::quat(glm::radians(glm::vec3(20, 30, 40)));
	REQUIRE(scene.SetParent(a, parent, false));
	const glm::vec3 oldWorld(scene.GetWorldTransform(a)[3]);
	const glm::vec3 oldScale = a.GetTransform().Scale;
	commands.Execute(session, "editor.transform", { { "delta", Matrix(glm::translate(glm::mat4(1), { 3, 2, 1 })) } });
	CHECK(glm::length(glm::vec3(scene.GetWorldTransform(a)[3]) - oldWorld - glm::vec3(3, 2, 1)) < 1e-4f);
	CHECK(glm::length(a.GetTransform().Scale - oldScale) < 1e-4f);
}

TEST_CASE("Editor shared fields validate every entity before mutation and tree operations avoid duplicates")
{
	CommandRegistry commands;
	RegisterEditorSelectionCommands(commands);
	AutomationSession session;
	auto a = session.GetScene()->CreateEntity("A"), b = session.GetScene()->CreateEntity("B");
	a.AddComponent<MaterialComponent>();
	session.SetSelections({ a.GetUUID(), b.GetUUID() });
	const auto before = SceneSerializer::SerializeScene(*session.GetScene());
	const json patch{ { "component", "Material" }, { "data", { { "Roughness", .7 } } } };
	CHECK_THROWS_AS(commands.Execute(session, "editor.component_set", patch), CommandError);
	CHECK(SceneSerializer::SerializeScene(*session.GetScene()) == before);
	b.AddComponent<MaterialComponent>();
	commands.Execute(session, "editor.component_set", patch);
	CHECK(a.GetComponent<MaterialComponent>().Roughness == doctest::Approx(.7));
	CHECK(b.GetComponent<MaterialComponent>().Roughness == doctest::Approx(.7));
	CHECK_THROWS_AS(commands.Execute(session, "editor.component_set", { { "component", "Transform" }, { "data", { { "Translation", { 1e100, 0, 0 } } } } }), CommandError);
	REQUIRE(session.GetScene()->SetParent(b, a, false));
	const auto copies = commands.Execute(session, "editor.duplicate", json::object());
	CHECK(copies["entities"].size() == 1);
	CHECK(session.GetScene()->GetAllEntitiesOrdered().size() == 4);
	commands.Execute(session, "editor.delete", json::object());
	CHECK(session.GetScene()->GetAllEntitiesOrdered().size() == 2);
	CHECK(session.GetSelections().empty());
}

TEST_CASE("Editor box selection intersects bounds in either drag direction and clips behind the camera")
{
	Scene scene;
	auto a = scene.CreateEntity("A"), b = scene.CreateEntity("B"), behind = scene.CreateEntity("Behind");
	a.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	b.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	b.GetTransform().Translation = { 1.5f, 0, 0 };
	behind.GetTransform().Translation.z = 10;
	const auto view = glm::lookAtRH(glm::vec3(0, 0, 5), glm::vec3(0), glm::vec3(0, 1, 0));
	const auto projection = glm::perspectiveRH_ZO(glm::radians(50.0f), 1.0f, .1f, 20.0f);
	const auto ids = SelectViewportBox(scene, projection * view, { 200, 200 }, { 70, 70 }, { 120, 130 });
	CHECK(ids == std::vector<UUID>{ a.GetUUID() });
	CHECK(SelectViewportBox(scene, projection * view, { 200, 200 }, { 120, 130 }, { 70, 70 }) == ids);
	const auto all = SelectViewportBox(scene, projection * view, { 200, 200 }, { 0, 0 }, { 200, 200 });
	CHECK(all.size() == 2);
	CHECK(std::find(all.begin(), all.end(), behind.GetUUID()) == all.end());
	CHECK(SelectViewportBox(scene, projection * view, { 0, 200 }, { 0, 0 }, { 200, 200 }).empty());
}

TEST_CASE("Editor command dispatch names UI and TCP edits and scopes transactions across drags")
{
	CommandRegistry commands;
	RegisterEditorSelectionCommands(commands);
	AutomationSession session;
	EditorHistory history;
	EditorContext context(commands, session, history);
	history.Reset(SceneSerializer::SerializeScene(*session.GetScene()));
	ImGui::CreateContext();
	const auto execute = [&](const std::string& name, const json& params = json::object()) {
		const auto response = commands.HandleRequest(session, { { "command", name }, { "params", params } });
		REQUIRE(response["ok"] == true);
		return response["result"];
	};
	const uint64_t a = execute("entity.create", { { "name", "A" } })["id"].get<uint64_t>();
	CHECK(history.UndoName() == "Create Entity");
	CHECK(history.GetSize() == 1);
	execute("editor.select", { { "entity", a } });
	CHECK(history.GetSize() == 1);
	for (int i = 1; i <= 5; ++i)
		context.EditField("component.set", { { "entity", a }, { "component", "Transform" }, { "data", { { "Translation", { i, 0, 0 } } } } }, true, i == 5);
	CHECK_FALSE(history.IsActive());
	CHECK(history.GetSize() == 2);
	CHECK(history.UndoName() == "Set Transform Translation");
	REQUIRE(context.BeginTransaction("Group Move"));
	execute("editor.transform", { { "delta", Matrix(glm::translate(glm::mat4(1), { 1, 0, 0 })) } });
	execute("editor.transform", { { "delta", Matrix(glm::translate(glm::mat4(1), { 1, 0, 0 })) } });
	CHECK(history.GetSize() == 2);
	CHECK(commands.HandleRequest(session, { { "command", "play.start" } })["ok"] == false);
	CHECK(commands.HandleRequest(session, { { "command", "scene.new" } })["ok"] == false);
	CHECK_FALSE(session.IsPlaying());
	context.EndTransaction();
	CHECK(history.GetSize() == 3);
	CHECK(history.UndoName() == "Group Move");
	CHECK(commands.HandleRequest(session, { { "command", "component.set" }, { "params", { { "entity", a }, { "component", "Transform" }, { "data", { { "typo", 1 } } } } } })["ok"] == false);
	CHECK(history.GetSize() == 3);
	// Switching selection finishes a field edit before another entity can receive the same field.
	context.EditField("component.set", { { "entity", a }, { "component", "Transform" }, { "data", { { "Translation", { 10, 0, 0 } } } } }, true, false);
	REQUIRE(history.IsActive());
	context.Execute("editor.select");
	CHECK_FALSE(history.IsActive());
	CHECK(history.GetSize() == 4);
	CHECK(history.UndoName() == "Set Transform Translation");
	// Top-level batches preserve command boundaries. Nested component writes stay under the group label.
	const auto responses = commands.HandleRequest(session, json::array({ { { "command", "entity.rename" }, { "params", { { "entity", a }, { "name", "Renamed" } } } }, { { "command", "entity.create" }, { "params", { { "name", "B" } } } } }));
	CHECK(responses[0]["ok"] == true);
	CHECK(responses[1]["ok"] == true);
	CHECK(history.GetSize() == 6);
	CHECK(history.Describe()["entries"][4]["name"] == "Rename Entity");
	ImGui::DestroyContext();
}

TEST_CASE("Editor duplicated forests remap cross-root references and preserve external targets")
{
	CommandRegistry commands;
	RegisterEditorSelectionCommands(commands);
	AutomationSession session;
	Scene& scene = *session.GetScene();
	auto a = scene.CreateEntity("A"), b = scene.CreateEntity("B"), external = scene.CreateEntity("External");
	auto childA = scene.CreateChildEntity(a, "Child A"), childB = scene.CreateChildEntity(b, "Child B");
	a.AddComponent<JointComponent>().ConnectedEntity = childB.GetUUID();
	childA.AddComponent<JointComponent>().ConnectedEntity = b.GetUUID();
	childB.AddComponent<JointComponent>().ConnectedEntity = external.GetUUID();
	session.SetSelections({ childA.GetUUID(), a.GetUUID(), b.GetUUID() });
	const auto result = commands.Execute(session, "editor.duplicate", json::object());
	REQUIRE(result["entities"].size() == 2);
	const Entity copyA = scene.GetEntityByUUID(result["entities"][0].get<uint64_t>());
	const Entity copyB = scene.GetEntityByUUID(result["entities"][1].get<uint64_t>());
	REQUIRE(copyA.GetChildren().size() == 1);
	REQUIRE(copyB.GetChildren().size() == 1);
	CHECK(copyA.GetComponent<JointComponent>().ConnectedEntity == copyB.GetChildren()[0].GetUUID());
	CHECK(copyA.GetChildren()[0].GetComponent<JointComponent>().ConnectedEntity == copyB.GetUUID());
	CHECK(copyB.GetChildren()[0].GetComponent<JointComponent>().ConnectedEntity == external.GetUUID());
	CHECK(a.GetComponent<JointComponent>().ConnectedEntity == childB.GetUUID());
}

TEST_CASE("Editor box selection retains static mesh bounds with an empty animator")
{
	Scene scene;
	auto entity = scene.CreateEntity("Combined");
	entity.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	entity.AddComponent<AnimatorComponent>();
	entity.GetTransform().Scale = { 4, 1, 1 };
	const auto view = glm::lookAtRH(glm::vec3(0, 0, 5), glm::vec3(0), glm::vec3(0, 1, 0));
	const auto projection = glm::orthoRH_ZO(-3.0f, 3.0f, -3.0f, 3.0f, .1f, 20.0f);
	CHECK(SelectViewportBox(scene, projection * view, { 300, 300 }, { 230, 130 }, { 240, 170 }) == std::vector<UUID>{ entity.GetUUID() });
}

TEST_CASE("Editor polling does not capture unmarked viewport state or change history")
{
	CommandRegistry commands;
	AutomationSession session;
	EditorHistory history;
	EditorContext context(commands, session, history);
	auto entity = session.GetScene()->CreateEntity("Entity");
	history.Reset(SceneSerializer::SerializeScene(*session.GetScene()));
	// Rendering can update derived scene state without authoring an edit. Polling must not snapshot it.
	entity.GetTransform().Translation.x = 1;
	const auto revision = session.GetEditRevision();
	for (const char* command : { "scene.get", "scene.info", "log.get", "component.types" })
		REQUIRE(commands.HandleRequest(session, { { "command", command } })["ok"] == true);
	CHECK(session.GetEditRevision() == revision);
	CHECK(history.GetSize() == 0);
	entity.GetTransform().Translation.x = 0;
	REQUIRE(commands.HandleRequest(session, { { "command", "entity.rename" }, { "params", { { "entity", "Entity" }, { "name", "New" } } } })["ok"] == true);
	CHECK(history.GetSize() == 1);
	CHECK(history.UndoName() == "Rename Entity");
}
