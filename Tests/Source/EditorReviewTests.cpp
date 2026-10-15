#include "EditorContext.h"
#include "TestUtils.h"

#include <Basalt/Automation/CommandSchema.h>
#include <Basalt/Automation/EditorCommandSchemas.h>
#include <Basalt/Automation/EditorSelection.h>
#include <Basalt/Math/Math.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/SceneSerializer.h>

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <random>

using namespace Basalt;
using json = nlohmann::json;

namespace {
	json MatrixJson(const glm::mat4& matrix)
	{
		auto result = json::array();
		for (int c = 0; c < 4; ++c)
			for (int r = 0; r < 4; ++r)
				result.push_back(matrix[c][r]);
		return result;
	}

	struct EditorFixture
	{
		CommandRegistry Commands;
		AutomationSession Session;
		EditorHistory History;
		EditorContext Context{ Commands, Session, History };

		EditorFixture()
		{
			RegisterEditorSelectionCommands(Commands);
			History.Reset(SceneSerializer::SerializeScene(*Session.GetScene()));
		}
		json Run(const std::string& command, const json& params = json::object())
		{
			const auto response = Commands.HandleRequest(Session, { { "command", command }, { "params", params } });
			REQUIRE_MESSAGE(response["ok"] == true, response.dump());
			return response["result"];
		}
		EditorHistory::Restore Restore()
		{
			return [this](json& state, std::string& error) {
				auto scene = CreateRef<Scene>();
				if (!SceneSerializer::DeserializeScene(*scene, state, error))
					return false;
				state = SceneSerializer::SerializeScene(*scene);
				const auto selected = Session.GetSelections();
				Session.SetEditScene(scene, Session.GetScenePath());
				Session.MarkDirty();
				Session.SetSelections(selected);
				return true;
			};
		}
	};
}

TEST_CASE("Editor affine transforms accept randomized parent and pivot inverses but reject real shear")
{
	EditorFixture test;
	Scene& scene = *test.Session.GetScene();
	auto parent = scene.CreateEntity("Parent"), child = scene.CreateChildEntity(parent, "Child"), other = scene.CreateEntity("Other");
	test.Session.SetSelection(child.GetUUID());
	std::mt19937 random(512);
	const auto number = [&]() { return static_cast<float>(random() % 10000) / 1000.0f - 5.0f; };
	for (int iteration = 0; iteration < 512; ++iteration)
	{
		INFO(iteration);
		parent.GetTransform().Translation = { number(), number(), number() };
		parent.GetTransform().Rotation = Math::QuatFromEuler({ number(), number(), number() });
		parent.GetTransform().Scale = glm::vec3(.5f + std::abs(number()));
		child.GetTransform().Translation = { number(), number(), number() };
		child.GetTransform().Rotation = Math::QuatFromEuler({ number(), number(), number() });
		const glm::mat4 before = scene.GetWorldTransform(child);
		test.Run("editor.transform", { { "delta", MatrixJson(glm::translate(glm::mat4(1), { .25f, 0, 0 })) } });
		CHECK(glm::length(glm::vec3(scene.GetWorldTransform(child)[3] - before[3]) - glm::vec3(.25f, 0, 0)) < .0001f);
		const auto pivot = SelectionPivot(scene, test.Session.GetSelections(), true);
		const auto next = glm::translate(glm::mat4(1), { 0, .25f, 0 }) * pivot;
		const auto delta = SelectionDelta(pivot, next);
		test.Run("editor.transform", { { "delta", MatrixJson(delta) } });
		CHECK(glm::length(glm::vec3(scene.GetWorldTransform(child)[3] - before[3]) - glm::vec3(.25f, .25f, 0)) < .0002f);
	}
	parent.GetTransform().Rotation = glm::quat(1, 0, 0, 0);
	parent.GetTransform().Scale = { 2, 1, 1 };
	child.GetTransform().Rotation = glm::quat(1, 0, 0, 0);
	test.Session.SetSelections({ other.GetUUID(), child.GetUUID() });
	const auto before = SceneSerializer::SerializeScene(scene);
	const auto rotation = glm::rotate(glm::mat4(1), .4f, glm::vec3(0, 0, 1));
	CHECK_THROWS_AS(test.Commands.Execute(test.Session, "editor.transform", { { "delta", MatrixJson(rotation) } }), CommandError);
	CHECK(SceneSerializer::SerializeScene(scene) == before);
	glm::mat4 perspective(1);
	perspective[0][3] = .01f;
	CHECK_THROWS_AS(test.Commands.Execute(test.Session, "editor.transform", { { "delta", MatrixJson(perspective) } }), CommandError);
	CHECK(SceneSerializer::SerializeScene(scene) == before);
	// A degenerate active transform uses identity axes, never a previous selected entity's rotation.
	other.GetTransform().Rotation = Math::QuatFromEuler({ .5f, .2f, .1f });
	child.GetTransform().Scale = glm::vec3(0);
	const auto pivot = SelectionPivot(scene, test.Session.GetSelections(), true);
	CHECK(glm::mat3(pivot) == glm::mat3(1));
}

TEST_CASE("Editor restored rotations preserve redo through history commands empty transactions and no-op writes")
{
	EditorFixture test;
	const auto id = test.Run("entity.create", { { "name", "Rotated" }, { "components", { { "Transform", { { "Rotation", { 7, 35, 13 } } } } } } })["id"];
	test.Run("component.set", { { "entity", id }, { "component", "Transform" }, { "data", { { "Translation", { 2, 0, 0 } } } } });
	test.Commands.Register({ "test.undo", "Restores through command dispatch", CommandSchema::Object(), [&](AutomationSession&, const json&) {
								REQUIRE(test.History.Undo(test.Restore()));
								return test.History.Describe();
							},
							 CommandPolicy::HistoryControl });
	test.Run("test.undo");
	REQUIRE(test.History.CanRedo());
	const auto entries = test.History.Describe()["entries"];
	REQUIRE(test.Context.BeginTransaction("Empty"));
	REQUIRE(test.Context.EndTransaction());
	CHECK(test.History.Describe()["entries"] == entries);
	CHECK(test.History.CanRedo());
	test.Run("component.set", { { "entity", id }, { "component", "Transform" }, { "data", { { "Translation", { 0, 0, 0 } } } } });
	CHECK(test.History.CanRedo());
	REQUIRE(test.Context.BeginTransaction("Cancel"));
	test.Run("entity.rename", { { "entity", id }, { "name", "Changed" } });
	REQUIRE(test.Context.CancelTransaction(test.Restore()));
	REQUIRE(test.Context.BeginTransaction("Empty after cancel"));
	REQUIRE(test.Context.EndTransaction());
	REQUIRE(test.History.Redo(test.Restore()));
	CHECK(test.Run("component.get", { { "entity", id }, { "component", "Transform" } })["Translation"] == json::array({ 2, 0, 0 }));
}

TEST_CASE("Editor history structural patches retain the correct base cursor and byte accounting")
{
	EditorHistory history;
	json state = { { "items", { "base" } } };
	const auto restore = [&](json& next, std::string&) { state = next; return true; };
	REQUIRE(history.Reset(state));
	REQUIRE(history.Commit({ { "items", { "base", "added" } }, { "property", true } }, "Add"));
	REQUIRE(history.Commit({ { "items", { "added" } } }, "Remove"));
	REQUIRE(history.Undo(restore));
	CHECK(state == json{ { "items", { "base", "added" } }, { "property", true } });
	const auto beforeFailure = history.Describe();
	CHECK_FALSE(history.Redo([](json&, std::string& error) { error = "broken reference"; return false; }));
	CHECK(history.GetError() == "Redo failed: broken reference");
	CHECK(history.Describe() == beforeFailure);
	REQUIRE(history.Undo(restore));
	CHECK(state == json{ { "items", { "base" } } });
	REQUIRE(history.Commit({ { "branch", { 1, 2, 3 } } }, "Branch"));
	CHECK(history.GetSize() == 1);
	CHECK(history.GetPatchBytes() == history.Describe()["entries"][0]["bytes"].get<size_t>());
	CHECK_FALSE(history.CanRedo());
	CHECK_FALSE(history.End(state));
	CHECK_FALSE(history.Cancel(restore));
	const auto valid = history.Describe();
	CHECK_FALSE(history.Reset({ { "Entities", json::array({ { { "ID", "invalid" } } }) } }));
	CHECK(history.Describe() == valid);
	CHECK_FALSE(history.Commit({ { "Entities", json::array({ { { "ID", "invalid" } } }) } }, "Bad"));
	CHECK(history.Describe() == valid);
	REQUIRE(history.Begin("Throwing cancel"));
	CHECK_FALSE(history.Commit(state, "Blocked"));
	CHECK_FALSE(history.Cancel([](json&, std::string&) -> bool { throw std::runtime_error("callback failure"); }));
	CHECK(history.IsActive());
	CHECK(history.GetError() == "Cancel failed: callback failure");
	REQUIRE(history.Cancel(restore));
}

TEST_CASE("Editor failures capture partial writes preserve their error and restore command nesting")
{
	EditorFixture test;
	test.Commands.Register({ "test.partial", "Fails after a nested mutation", CommandSchema::Object(), [&](AutomationSession& session, const json&) -> json {
								test.Commands.Execute(session, "entity.create", { { "name", "Partial" } });
								throw CommandError("original failure");
							} });
	const auto result = test.Commands.HandleRequest(test.Session, { { "command", "test.partial" } });
	CHECK(result["error"] == "original failure");
	CHECK(test.History.GetSize() == 1);
	CHECK(test.History.UndoName() == "test.partial");
	test.Run("entity.rename", { { "entity", "Partial" }, { "name", "Renamed" } });
	CHECK(test.History.GetSize() == 2);
	REQUIRE(test.History.Undo(test.Restore()));
	REQUIRE(test.History.Undo(test.Restore()));
	CHECK(test.Session.GetScene()->GetEntityCount() == 0);
}

TEST_CASE("Editor transaction ownership dirty cancellation boundaries and gesture failure recovery")
{
	EditorFixture test;
	test.Run("entity.create", { { "name", "A" } });
	test.Session.ClearDirty();
	const auto baseline = SceneSerializer::SerializeScene(*test.Session.GetScene());
	REQUIRE(test.Context.BeginTransaction("Client work"));
	CHECK_FALSE(test.Context.Execute("entity.rename", { { "entity", "A" }, { "name", "GUI" } }));
	CHECK(test.Context.StatusMessage.find("Client work") != std::string::npos);
	CHECK(SceneSerializer::SerializeScene(*test.Session.GetScene()) == baseline);
	const std::vector<std::pair<std::string, json>> boundaries = {
		{ "scene.new", json::object() }, { "scene.open", { { "path", "missing.bscene" } } }, { "scene.save", json::object() }, { "project.open", { { "path", "missing" } } }, { "project.create", { { "path", "missing" } } }, { "play.start", json::object() }, { "replay.run", { { "path", "missing.json" } } }
	};
	for (const auto& [name, params] : boundaries)
	{
		INFO(name);
		const auto response = test.Commands.HandleRequest(test.Session, { { "command", name }, { "params", params } });
		CHECK(response["ok"] == false);
		CHECK(response["error"].get<std::string>().find("Client work") != std::string::npos);
	}
	std::string error;
	CHECK_FALSE(test.Session.StartPlay(error, true));
	CHECK(error.find("Client work") != std::string::npos);
	test.Run("entity.rename", { { "entity", "A" }, { "name", "API" } });
	REQUIRE(test.Context.CancelTransaction(test.Restore()));
	CHECK_FALSE(test.Session.IsDirty());
	CHECK(SceneSerializer::SerializeScene(*test.Session.GetScene()) == baseline);
	CHECK_FALSE(test.Context.EndTransaction());
	CHECK_FALSE(test.Context.CancelTransaction(test.Restore()));
	test.Session.MarkDirty();
	REQUIRE(test.Context.BeginTransaction("Dirty cancellation"));
	REQUIRE(test.Context.CancelTransaction(test.Restore()));
	CHECK(test.Session.IsDirty());
	REQUIRE(test.Context.BeginTransaction("Gesture", EditorContext::TransactionKind::Gizmo));
	test.Run("entity.rename", { { "entity", "A" }, { "name", "Moved" } });
	test.Context.FinishGizmoTransaction(true, [](json&, std::string& reason) { reason = "unavailable asset"; return false; });
	CHECK_FALSE(test.History.IsActive());
	CHECK(test.History.UndoName() == "Gesture");
	CHECK(test.Context.StatusMessage.find("gesture committed instead") != std::string::npos);
	CHECK(test.History.Undo(test.Restore()));
	// Explicit cancellation must not auto-commit a pending inspector gesture in BeforeCommand.
	test.Commands.Register({ "editor.transaction", "Transaction control", EditorCommandSchemas::Transaction(), [&](AutomationSession&, const json&) {
								REQUIRE(test.Context.CancelTransaction(test.Restore()));
								return test.History.Describe();
							},
							 CommandPolicy::HistoryControl });
	test.Context.EditField("component.set", { { "entity", "A" }, { "component", "Transform" }, { "data", { { "Translation", { 3, 0, 0 } } } } }, true, false);
	REQUIRE(test.History.IsActive());
	test.Run("editor.transaction", { { "action", "cancel" } });
	CHECK_FALSE(test.History.IsActive());
	CHECK(test.Run("component.get", { { "entity", "A" }, { "component", "Transform" } })["Translation"] == json::array({ 0, 0, 0 }));
	// Invalid continuous edits stop after the first failure, without leaving an empty transaction open.
	const json invalid{ { "entity", "A" }, { "component", "Transform" }, { "data", { { "Scale", { 1e100, 1, 1 } } } } };
	test.Context.EditField("component.set", invalid, true, false);
	CHECK_FALSE(test.History.IsActive());
	const auto status = test.Context.StatusMessage;
	test.Context.EditField("component.set", invalid, true, false);
	CHECK(test.Context.StatusMessage == status);
	test.Context.FinishFieldEdit();
	int attempts = 0;
	test.Commands.Register({ "test.field", "Rejects a field gesture", CommandSchema::Object(), [&](AutomationSession&, const json&) -> json { ++attempts; throw CommandError("bad field"); } });
	test.Context.EditField("test.field", json::object(), true, false);
	test.Context.EditField("test.field", json::object(), true, false);
	CHECK(attempts == 1);
	CHECK_FALSE(test.History.IsActive());
	test.Context.FinishFieldEdit();
	test.Context.OnSceneChanged();
	test.History.Reset(SceneSerializer::SerializeScene(*test.Session.GetScene()));
	test.Context.CommitHistory();
	CHECK(test.History.GetSize() == 0);
}

TEST_CASE("Editor selection modes and play-copy forest operations preserve edit state")
{
	EditorFixture test;
	const auto a = test.Run("entity.create", { { "name", "A" } })["id"].get<uint64_t>();
	const auto b = test.Run("entity.create", { { "name", "B" } })["id"].get<uint64_t>();
	test.Run("editor.select", { { "entity", a } });
	test.Run("editor.select", { { "entity", b }, { "mode", "toggle" } });
	CHECK(test.Session.GetSelections() == std::vector<UUID>{ UUID(a), UUID(b) });
	test.Run("editor.select", { { "entity", a }, { "mode", "add" } });
	CHECK(test.Session.GetSelection() == UUID(b));
	test.Run("editor.select", { { "mode", "add" } });
	CHECK(test.Session.GetSelection() == UUID(b));
	CHECK_THROWS_AS(test.Commands.Execute(test.Session, "editor.select", { { "entity", a }, { "entities", { b } } }), CommandError);
	const auto before = SceneSerializer::SerializeScene(*test.Session.GetScene());
	CHECK_THROWS_AS(test.Commands.Execute(test.Session, "editor.component_set", { { "component", "Transform" }, { "data", { { "Translation", { 1e100, 0, 0 } } } } }), CommandError);
	CHECK(SceneSerializer::SerializeScene(*test.Session.GetScene()) == before);
	test.Session.ClearDirty();
	const auto revision = test.Session.GetEditRevision();
	const auto history = test.History.Describe();
	for (bool simulate : { false, true })
	{
		std::string error;
		REQUIRE(test.Session.StartPlay(error, simulate));
		test.Run("editor.select", { { "entities", { a, b } } });
		test.Run("editor.duplicate");
		CHECK(test.Session.GetScene()->GetEntityCount() == 4);
		test.Run("editor.delete");
		CHECK(test.Session.GetScene()->GetEntityCount() == 2);
		test.Session.StopPlay();
		CHECK(test.Session.GetEditRevision() == revision);
		CHECK_FALSE(test.Session.IsDirty());
		CHECK(test.History.Describe() == history);
		CHECK(SceneSerializer::SerializeScene(*test.Session.GetScene()) == before);
	}
	test.Run("editor.select", { { "entity", b } });
	REQUIRE(test.History.Undo(test.Restore()));
	CHECK(test.Session.GetSelections().empty());
}

TEST_CASE("Editor every scene-mutating command increments revision and creates one named entry")
{
	BasaltTest::TempDirectory directory("EditorRevision");
	EditorFixture test;
	test.Run("project.create", { { "path", directory.GetPath().string() }, { "name", "Revisions" } });
	test.Context.OnSceneChanged();
	test.History.Reset(SceneSerializer::SerializeScene(*test.Session.GetScene()));
	const auto mutate = [&](const std::string& name, const json& params) {
		const auto revision = test.Session.GetEditRevision();
		const auto entries = test.History.GetSize();
		const auto result = test.Run(name, params);
		INFO(name);
		CHECK(test.Session.GetEditRevision() > revision);
		CHECK(test.History.GetSize() == entries + 1);
		CHECK(test.History.UndoName() == EditorHistory::CommandName(name, params));
		return result;
	};
	const auto a = mutate("entity.create", { { "name", "A" } })["id"];
	mutate("entity.create", { { "name", "B" } });
	mutate("entity.rename", { { "entity", "A" }, { "name", "Renamed" } });
	mutate("entity.set_parent", { { "entity", "Renamed" }, { "parent", "B" } });
	mutate("component.set", { { "entity", "B" }, { "component", "Material" }, { "data", { { "Roughness", .3 } } } });
	mutate("component.remove", { { "entity", "B" }, { "component", "Material" } });
	mutate("scene.settings", { { "Name", "Changed" } });
	mutate("entity.duplicate", { { "entity", "Renamed" }, { "name", "Copy" } });
	mutate("entity.destroy", { { "entity", "Copy" } });
	mutate("asset.import_model", { { "path", "builtin://Cube" } });
	test.Run("prefab.create", { { "entity", "B" }, { "path", "Assets/Prefabs/Test.bprefab" } });
	mutate("prefab.instantiate", { { "path", "Assets/Prefabs/Test.bprefab" } });
	test.Run("editor.select", { { "entity", a } });
	mutate("editor.component_set", { { "component", "Transform" }, { "data", { { "Translation", { 2, 0, 0 } } } } });
	mutate("editor.transform", { { "delta", MatrixJson(glm::translate(glm::mat4(1), { 1, 0, 0 })) } });
	mutate("editor.duplicate", json::object());
	mutate("editor.delete", json::object());
	Project::SetActive(nullptr);
}

TEST_CASE("Editor duplicate during a gizmo drag commits movement before changing selection")
{
	EditorFixture test;
	const auto original = test.Run("entity.create", { { "name", "Original" } })["id"].get<uint64_t>();
	test.Run("editor.select", { { "entity", original } });
	const auto entries = test.History.GetSize();
	REQUIRE(test.Context.BeginTransaction("Move Selection", EditorContext::TransactionKind::Gizmo));
	REQUIRE(test.Context.Execute("editor.transform", { { "delta", MatrixJson(glm::translate(glm::mat4(1), { 1, 0, 0 })) } }));
	REQUIRE(test.Context.Execute("editor.duplicate"));
	test.Context.CommitHistory();
	CHECK_FALSE(test.History.IsActive());
	CHECK(test.History.GetSize() == entries + 2);
	CHECK(test.History.Describe()["entries"][entries]["name"] == "Move Selection");
	CHECK(test.History.UndoName() == "Duplicate Entities");
	const auto copy = test.Session.GetSelection();
	REQUIRE(copy != UUID(original));
	// The viewport observes the changed selection later in the frame and abandons the old gizmo.
	test.Context.FinishGizmoTransaction(true, test.Restore());
	REQUIRE(test.Session.GetScene()->GetEntityCount() == 2);
	CHECK(test.Session.GetScene()->GetEntityByUUID(copy).GetTransform().Translation.x == doctest::Approx(1));
	REQUIRE(test.History.Undo(test.Restore()));
	CHECK(test.Session.GetScene()->GetEntityCount() == 1);
	CHECK(test.Session.GetScene()->GetEntityByUUID(original).GetTransform().Translation.x == doctest::Approx(1));
	REQUIRE(test.History.Undo(test.Restore()));
	CHECK(test.Session.GetScene()->GetEntityByUUID(original).GetTransform().Translation.x == doctest::Approx(0));
}
