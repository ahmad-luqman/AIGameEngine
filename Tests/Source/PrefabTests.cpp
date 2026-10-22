#include "AutomationTestUtils.h"
#include "TestUtils.h"

#include <doctest/doctest.h>

#include <Basalt/Core/FileSystem.h>
#include <Basalt/Math/Math.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Prefab.h>
#include <Basalt/Scene/SceneSerializer.h>
#include <Basalt/Scripting/LuaJson.h>

#include <glm/gtc/quaternion.hpp>

#include <functional>

using namespace Basalt;
using namespace BasaltTest;
using json = nlohmann::json;

namespace {
	uint64_t IdOf(Entity entity)
	{
		return static_cast<uint64_t>(entity.GetUUID());
	}

	// An authoring tree Source > Child saved as Assets/Prefabs/Test.bprefab. prefab.create links the tree as an instance
	// of its prefab, so SaveSource writes it again with the same IDs, which merges it into the scene's other instances
	// like any prefab write (and the authoring tree counts among the instances when others change the file).
	struct PrefabFixture
	{
		TempProject Directory{ "PrefabEditing" };
		AutomationSession Session;
		CommandRegistry Commands;
		Entity Source;
		Entity Child;
		std::string Path = "Assets/Prefabs/Test.bprefab";

		PrefabFixture()
		{
			auto scene = Session.GetScene();
			Source = scene->CreateEntity("Source");
			Source.AddComponent<PointLightComponent>();
			Child = scene->CreateEntity("Child");
			scene->SetParent(Child, Source, false);
			SaveSource();
		}

		json Run(const std::string& command, const json& params = json::object())
		{
			return BasaltTest::Run(Commands, Session, command, params);
		}

		std::string RunError(const std::string& command, const json& params = json::object())
		{
			return BasaltTest::RunError(Commands, Session, command, params);
		}

		Entity EntityFrom(const json& result)
		{
			return Session.GetScene()->GetEntityByUUID(UUID(result["id"].get<uint64_t>()));
		}

		Entity Instantiate(const json& params = { { "position", { 5, 0, 0 } } })
		{
			json all = params;
			all["path"] = Path;
			return EntityFrom(Run("prefab.instantiate", all));
		}

		json SaveSource()
		{
			return Run("prefab.create", { { "entity", IdOf(Source) }, { "path", Path } });
		}

		json Overrides(Entity instance)
		{
			return Run("prefab.overrides", { { "entity", IdOf(instance) } })["overrides"];
		}

		json ReadSource() const
		{
			json source;
			std::string error;
			REQUIRE_MESSAGE(Prefab::ReadSource(Basalt::Project::ResolvePath(Path), source, error), error);
			return source;
		}

		// The prefab file's ID for the entity named name.
		uint64_t SourceId(const std::string& name) const
		{
			for (const json& record : ReadSource())
				if (record["Name"] == name)
					return record["ID"].get<uint64_t>();
			FAIL("no source entity named " << name);
			return 0;
		}

		std::string EntityPath(const std::string& name) const
		{
			return "/" + std::to_string(SourceId(name));
		}

		std::string FieldPath(const std::string& name, const std::string& component, const std::string& field) const
		{
			return EntityPath(name) + "/Components/" + component + "/" + field;
		}

		// Writes the prefab file directly and reports the change, as the file watcher does for an external edit.
		AssetReloadReport WriteExternally(const std::function<void(json& file)>& edit)
		{
			json file = ParseJson(*FileSystem::ReadTextFile(Basalt::Project::ResolvePath(Path)));
			edit(file);
			Directory.WriteFile(Path, file.dump());
			return Session.ReloadAssets({ { Path, FileChangeType::Modified } });
		}

		static Entity ChildNamed(Entity parent, const std::string& name)
		{
			for (Entity child : parent.GetChildren())
				if (child.GetName() == name)
					return child;
			return {};
		}
	};
}

TEST_SUITE("PrefabEditing")
{
	TEST_CASE("Source edits merge by field and keep instance IDs, added components and placement")
	{
		PrefabFixture f;
		Entity instance = f.Instantiate();
		const auto childID = instance.GetChildren().front().GetUUID();
		instance.GetComponent<PointLightComponent>().Intensity = 7;
		instance.AddComponent<AudioListenerComponent>();
		f.Source.GetComponent<PointLightComponent>().Intensity = 4;
		f.Source.GetComponent<PointLightComponent>().Radius = 13;
		f.Child.GetComponent<TagComponent>().Tag = "Renamed";
		CHECK(f.SaveSource()["instances"] == 1);
		CHECK(instance.GetComponent<PointLightComponent>().Intensity == 7);
		CHECK(instance.GetComponent<PointLightComponent>().Radius == 13);
		CHECK(instance.GetTransform().Translation.x == 5);
		CHECK(instance.HasComponent<AudioListenerComponent>());
		CHECK(instance.GetChildren().front().GetUUID() == childID);
		CHECK(instance.GetChildren().front().GetName() == "Renamed");
		const json overrides = f.Overrides(instance);
		CHECK(overrides.contains(f.FieldPath("Source", "PointLight", "Intensity")));
		CHECK(overrides.contains(f.EntityPath("Source") + "/Components/AudioListener"));
		CHECK_FALSE(overrides.contains(f.FieldPath("Source", "PointLight", "Radius")));
		f.Run("prefab.revert", { { "entity", IdOf(instance) } });
		CHECK(instance.GetComponent<PointLightComponent>().Intensity == 4);
		CHECK_FALSE(instance.HasComponent<AudioListenerComponent>());
		CHECK(instance.GetTransform().Translation.x == 5);
		CHECK(f.Overrides(instance).empty());
	}

	TEST_CASE("Prefab files get fresh IDs that no scene entity has, and keep them when written again")
	{
		PrefabFixture f;
		const json source = f.ReadSource();
		CHECK(source[0]["ID"] != IdOf(f.Source));
		CHECK_FALSE(f.Session.GetScene()->GetEntityByUUID(UUID(source[0]["ID"].get<uint64_t>())));
		CHECK(source[1]["Parent"] == source[0]["ID"]);
		f.SaveSource();
		CHECK(f.ReadSource() == source);

		// Saving an instance over its own prefab maps its members back to their source IDs; additions get new ones.
		Entity instance = f.Instantiate();
		Entity added = f.Session.GetScene()->CreateEntity("Added");
		f.Session.GetScene()->SetParent(added, instance, false);
		f.Run("prefab.create", { { "entity", IdOf(instance) }, { "path", f.Path } });
		const json rewritten = f.ReadSource();
		REQUIRE(rewritten.size() == 3);
		CHECK(rewritten[0]["ID"] == source[0]["ID"]);
		CHECK(rewritten[1]["ID"] == source[1]["ID"]);
		CHECK(rewritten[2]["Parent"] == source[0]["ID"]);
		CHECK(instance.HasComponent<PrefabComponent>());
	}

	TEST_CASE("Overwriting a prefab with an unrelated tree keeps its IDs by name, so instances keep merging")
	{
		PrefabFixture f;
		Entity instance = f.Instantiate();
		instance.GetComponent<PointLightComponent>().Intensity = 9;
		auto scene = f.Session.GetScene();
		Entity other = scene->CreateEntity("Other");
		other.AddComponent<PointLightComponent>().Radius = 31;
		Entity otherChild = scene->CreateEntity("Child");
		scene->SetParent(otherChild, other, false);
		const json result = f.Run("prefab.create", { { "entity", IdOf(other) }, { "path", f.Path } });
		CHECK(result["linked"] == true);
		CHECK(result["instances"] == 2);
		CHECK(result["warnings"].empty());
		CHECK(instance.GetComponent<PointLightComponent>().Radius == 31);
		CHECK(instance.GetComponent<PointLightComponent>().Intensity == 9);
		CHECK(instance.GetName() == "Other");
	}

	TEST_CASE("The root's position and rotation are placement; its scale is an override")
	{
		PrefabFixture f;
		Entity first = f.Instantiate();
		Entity second = f.Instantiate();
		Entity unmoved = f.Instantiate(json::object());
		first.GetTransform().Translation = { 1, 2, 3 };
		first.GetTransform().Rotation = glm::angleAxis(1.0f, glm::vec3(0, 1, 0));
		first.GetTransform().Scale = glm::vec3(2.0f);
		const json overrides = f.Overrides(first);
		CHECK_FALSE(overrides.contains(f.FieldPath("Source", "Transform", "Translation")));
		CHECK_FALSE(overrides.contains(f.FieldPath("Source", "Transform", "Rotation")));
		CHECK(overrides.contains(f.FieldPath("Source", "Transform", "Scale")));
		f.RunError("prefab.revert", { { "entity", IdOf(first) }, { "field", f.FieldPath("Source", "Transform", "Translation") } });

		f.Run("prefab.apply", { { "entity", IdOf(first) } });
		CHECK(second.GetTransform().Scale.x == doctest::Approx(2));
		CHECK(second.GetTransform().Translation.x == doctest::Approx(5));
		CHECK(f.ReadSource()[0]["Components"]["Transform"]["Translation"] == json::array({ 0.0, 0.0, 0.0 }));

		// Moving the prefab's root moves no instance, not even one still at the old root position.
		f.Source.GetTransform().Translation = { 0, 7, 0 };
		f.Source.GetTransform().Rotation = glm::angleAxis(0.5f, glm::vec3(1, 0, 0));
		f.SaveSource();
		CHECK(unmoved.GetTransform().Translation.y == doctest::Approx(0));
		CHECK(unmoved.GetTransform().Rotation.w == doctest::Approx(1));
		CHECK(second.GetTransform().Translation.y == doctest::Approx(0));

		second.GetTransform().Scale = glm::vec3(3.0f);
		f.Run("prefab.revert", { { "entity", IdOf(second) } });
		CHECK(second.GetTransform().Scale.x == doctest::Approx(2));
		CHECK(second.GetTransform().Translation.x == doctest::Approx(5));
	}

	TEST_CASE("Source adds and removes children; local children survive in order and references remap")
	{
		PrefabFixture f;
		Entity instance = f.Instantiate();
		auto scene = f.Session.GetScene();
		Entity local = scene->CreateEntity("Local");
		scene->SetParent(local, instance.GetChildren().front(), false);
		Entity rootLocal = scene->CreateEntity("RootLocal");
		scene->SetParent(rootLocal, instance, false);
		scene->DestroyEntity(f.Child);
		Entity added = scene->CreateEntity("Added");
		scene->SetParent(added, f.Source, false);
		added.AddComponent<BoneAttachmentComponent>().Target = f.Source.GetUUID();
		f.SaveSource();
		CHECK(local);
		CHECK(local.GetParent() == instance);
		const auto children = instance.GetChildren();
		REQUIRE(children.size() == 3);
		// Prefab entities first, in source order, then local additions in their order.
		CHECK(children[0].GetName() == "Added");
		CHECK(children[1] == rootLocal);
		CHECK(children[2] == local);
		CHECK(children[0].GetComponent<BoneAttachmentComponent>().Target == instance.GetUUID());
	}

	TEST_CASE("References to entities the instance deleted stay inside the instance")
	{
		PrefabFixture f;
		auto scene = f.Session.GetScene();
		Entity grandchild = scene->CreateEntity("Grandchild");
		scene->SetParent(grandchild, f.Child, false);
		f.SaveSource();
		Entity instance = f.Instantiate();
		scene->DestroyEntity(PrefabFixture::ChildNamed(instance, "Child"));

		// The source adds a child under Child (deleted by the instance) and a reference to Child.
		Entity late = scene->CreateEntity("Late");
		scene->SetParent(late, f.Child, false);
		Entity pointer = scene->CreateEntity("Pointer");
		scene->SetParent(pointer, f.Source, false);
		pointer.AddComponent<BoneAttachmentComponent>().Target = f.Child.GetUUID();
		const auto roots = scene->GetRootEntities().size();
		f.SaveSource();
		CHECK(scene->GetRootEntities().size() == roots);
		CHECK(f.Child.GetChildren().size() == 2);
		// The instance root itself took Late.
		CHECK(PrefabFixture::ChildNamed(instance, "Late"));
		// A reference to the deleted Child keeps its reserved ID, so reverting the deletion restores it.
		const uint64_t reserved = instance.GetComponent<PrefabComponent>().Members[std::to_string(f.SourceId("Child"))].get<uint64_t>();
		Entity pointerInstance = PrefabFixture::ChildNamed(instance, "Pointer");
		CHECK(pointerInstance.GetComponent<BoneAttachmentComponent>().Target == UUID(reserved));
		CHECK_FALSE(f.Overrides(instance).contains(f.FieldPath("Pointer", "BoneAttachment", "Target")));
		f.Run("prefab.revert", { { "entity", IdOf(instance) }, { "field", f.EntityPath("Child") } });
		CHECK(pointerInstance.GetComponent<BoneAttachmentComponent>().Target == UUID(reserved));
		CHECK(PrefabFixture::ChildNamed(instance, "Child").GetUUID() == UUID(reserved));

		// An overridden member whose source parent is deleted stays under the instance root.
		scene->DestroyEntity(PrefabFixture::ChildNamed(instance, "Child"));
		Entity second = f.Instantiate();
		Entity kept = PrefabFixture::ChildNamed(PrefabFixture::ChildNamed(second, "Child"), "Grandchild");
		REQUIRE(kept);
		kept.AddComponent<PointLightComponent>().Intensity = 8;
		scene->DestroyEntity(f.Child);
		f.SaveSource();
		REQUIRE(kept.IsValid());
		CHECK(kept.GetParent() == second);
		CHECK(scene->GetRootEntities().size() == roots + 1);
	}

	TEST_CASE("References to entities outside the instance never become references to its own entities")
	{
		PrefabFixture f;
		auto scene = f.Session.GetScene();
		// A prefab written before source IDs were fresh: its IDs are the authoring entities' UUIDs.
		f.Directory.WriteFile(f.Path, json{ { "Format", "BasaltPrefab" }, { "Version", 3 }, { "Entities", SceneSerializer::SerializeEntityTree(f.Source) } }.dump());
		Entity instance = f.Instantiate();
		Entity child = PrefabFixture::ChildNamed(instance, "Child");
		child.AddComponent<BoneAttachmentComponent>().Target = f.Source.GetUUID();
		Entity moved = f.Instantiate();
		Entity movedChild = PrefabFixture::ChildNamed(moved, "Child");
		scene->SetParent(movedChild, f.Source, false);
		const std::string childPath = "/" + std::to_string(IdOf(f.Child));
		CHECK(f.Overrides(instance)[childPath + "/Components/BoneAttachment"]["Target"] == json{ { "External", IdOf(f.Source) } });
		// A member moved out of its instance's tree leaves the instance: it counts as deleted, and stays where it is.
		CHECK(f.Overrides(moved)[childPath].is_null());
		f.WriteExternally([](json& file) { file["Entities"][0]["Components"]["PointLight"]["Radius"] = 21; });
		CHECK(instance.GetComponent<PointLightComponent>().Radius == 21);
		CHECK(child.GetComponent<BoneAttachmentComponent>().Target == f.Source.GetUUID());
		CHECK(f.Child.GetParent() == f.Source);
		CHECK(movedChild.GetParent() == f.Source);
		CHECK(f.Source.GetChildren().size() == 2);
	}

	TEST_CASE("Scene saves keep the baseline, and copies track their own entities")
	{
		PrefabFixture f;
		Entity instance = f.Instantiate();
		instance.GetComponent<PointLightComponent>().Intensity = 8;
		std::string error;
		auto data = SceneSerializer::SerializeScene(*f.Session.GetScene());
		auto restored = CreateRef<Scene>();
		REQUIRE_MESSAGE(SceneSerializer::DeserializeScene(*restored, data, error), error);
		Entity root = restored->GetEntityByUUID(instance.GetUUID());
		CHECK(root.GetComponent<PrefabComponent>().Baseline == instance.GetComponent<PrefabComponent>().Baseline);
		CHECK(root.GetComponent<PrefabComponent>().Members == instance.GetComponent<PrefabComponent>().Members);
		CHECK(Prefab::ComputeOverrides(root).contains(f.FieldPath("Source", "PointLight", "Intensity")));
		Entity duplicate = SceneSerializer::DeserializeEntityTree(*restored, SceneSerializer::SerializeEntityTree(root), {}, error);
		REQUIRE_MESSAGE(duplicate, error);
		CHECK(duplicate.GetComponent<PrefabComponent>().Members != root.GetComponent<PrefabComponent>().Members);
		CHECK(Prefab::Refresh(duplicate, f.ReadSource(), Prefab::RefreshMode::RevertAll, error) == Prefab::RefreshResult::Merged);
		CHECK(duplicate.GetComponent<PointLightComponent>().Intensity == PointLightComponent{}.Intensity);
		CHECK(root.GetComponent<PointLightComponent>().Intensity == 8);
		CHECK(root.GetChildren().front().GetUUID() != duplicate.GetChildren().front().GetUUID());
	}

	TEST_CASE("entity.duplicate copies an instance that tracks its own entities")
	{
		PrefabFixture f;
		Entity instance = f.Instantiate();
		Entity copy = f.EntityFrom(f.Run("entity.duplicate", { { "entity", IdOf(instance) } }));
		REQUIRE(copy.HasComponent<PrefabComponent>());
		for (const auto& [key, value] : copy.GetComponent<PrefabComponent>().Members.items())
			CHECK(value != instance.GetComponent<PrefabComponent>().Members[key]);
		f.Source.GetComponent<PointLightComponent>().Radius = 17;
		CHECK(f.SaveSource()["instances"] == 2);
		CHECK(copy.GetComponent<PointLightComponent>().Radius == 17);
		CHECK(instance.GetChildren().size() == 1);
		CHECK(copy.GetChildren().size() == 1);
		CHECK(instance.GetChildren().front().GetParent() == instance);
	}

	TEST_CASE("Deleted entities keep their IDs: Revert recreates them, also after a save, and copies get their own")
	{
		PrefabFixture f;
		Entity instance = f.Instantiate();
		auto scene = f.Session.GetScene();
		const UUID childID = instance.GetChildren().front().GetUUID();
		scene->DestroyEntity(instance.GetChildren().front());
		CHECK(f.Overrides(instance).contains(f.EntityPath("Child")));
		std::string error;
		Entity copy = SceneSerializer::DeserializeEntityTree(*scene, SceneSerializer::SerializeEntityTree(instance), {}, error);
		REQUIRE_MESSAGE(copy, error);
		const UUID instanceID = instance.GetUUID();
		const UUID copyID = copy.GetUUID();
		f.Run("scene.save", { { "path", "Assets/Main.bscene" } });
		f.Run("scene.open", { { "path", "Assets/Main.bscene" } });
		scene = f.Session.GetScene();
		instance = scene->GetEntityByUUID(instanceID);
		copy = scene->GetEntityByUUID(copyID);
		f.Run("prefab.revert", { { "entity", IdOf(instance) } });
		f.Run("prefab.revert", { { "entity", IdOf(copy) } });
		REQUIRE(instance.GetChildren().size() == 1);
		REQUIRE(copy.GetChildren().size() == 1);
		CHECK(instance.GetChildren().front().GetUUID() == childID);
		CHECK(copy.GetChildren().front() != instance.GetChildren().front());
	}

	TEST_CASE("Editing a prefab in isolation keeps the waiting scene and merges on save")
	{
		PrefabFixture f;
		Entity instance = f.Instantiate();
		f.Run("scene.save", { { "path", "Assets/Main.bscene" } });
		const auto scene = f.Session.GetEditScene();
		const auto id = instance.GetUUID();
		f.Session.SetSelection(id);
		REQUIRE_FALSE(f.Session.IsDirty());
		f.Run("prefab.open", { { "path", f.Path } });
		CHECK(f.Session.IsEditingPrefab());
		CHECK(f.Session.GetEditScene() != scene);
		CHECK_FALSE(f.Session.IsWaitingSceneDirty());
		std::string error;
		CHECK_FALSE(f.Session.StartPlay(error));
		f.Run("component.set", { { "entity", "Source" }, { "component", "PointLight" }, { "data", { { "Radius", 20 } } } });
		f.RunError("prefab.close");
		// The instance and the authoring tree.
		CHECK(f.Run("prefab.save")["instances"] == 2);
		CHECK(instance.GetComponent<PointLightComponent>().Radius == 20);
		CHECK(f.Session.IsWaitingSceneDirty());
		// A second save in the same session compares with what the first one wrote.
		f.Run("component.set", { { "entity", "Source" }, { "component", "PointLight" }, { "data", { { "Radius", 22 } } } });
		CHECK(f.Run("prefab.save")["instances"] == 2);
		CHECK(f.Run("prefab.close")["path"] == "Assets/Main.bscene");
		CHECK_FALSE(f.Session.IsEditingPrefab());
		CHECK(f.Session.GetEditScene() == scene);
		CHECK(f.Session.GetScenePath() == "Assets/Main.bscene");
		CHECK(f.Session.GetSelection() == id);
		CHECK(f.Session.IsDirty());
		CHECK(instance.GetComponent<PointLightComponent>().Radius == 22);

		// Discarded edits leave a clean waiting scene clean.
		f.Run("scene.save");
		f.Run("prefab.open", { { "path", f.Path } });
		f.Run("entity.rename", { { "entity", "Source" }, { "name", "Discarded" } });
		f.Run("prefab.close", { { "discard", true } });
		CHECK_FALSE(f.Session.IsDirty());
		CHECK(instance.GetName() == "Source");
	}

	TEST_CASE("While a prefab is open, every edit boundary except prefab.save/close is rejected")
	{
		PrefabFixture f;
		f.Run("prefab.open", { { "path", f.Path } });
		const auto edit = f.Session.GetEditScene();
		std::vector<std::string> boundaries;
		for (const auto& [name, info] : f.Commands.GetCommands())
			if (info.Policy == CommandPolicy::EditBoundary && name != "prefab.save" && name != "prefab.close")
				boundaries.push_back(name);
		boundaries.emplace_back("project.export");
		CHECK(std::find(boundaries.begin(), boundaries.end(), "asset.move") != boundaries.end());
		for (const char* pinned : { "play.start", "scene.save", "scene.open", "project.open", "asset.delete" })
			CHECK(std::find(boundaries.begin(), boundaries.end(), pinned) != boundaries.end());
		for (const std::string& name : boundaries)
		{
			INFO(name);
			const json response = f.Commands.HandleRequest(f.Session, { { "command", name }, { "params", json::object() } });
			CHECK(response["ok"] == false);
			CHECK(Contains(response["error"].get<std::string>(), "unavailable while 'Assets/Prefabs/Test.bprefab' is open"));
		}
		CHECK(f.Session.GetEditScene() == edit);
		CHECK(f.Session.GetPrefabPath() == f.Path);
		f.Run("prefab.close");
	}

	TEST_CASE("Saving rejects external edits unless forced, nested instances and extra roots; failed opens change nothing")
	{
		PrefabFixture f;
		f.Run("prefab.open", { { "path", f.Path } });
		f.Run("prefab.instantiate", { { "path", f.Path }, { "parent", "Source" } });
		CHECK(Contains(f.RunError("prefab.save"), "unpack the nested prefab instance"));
		f.Run("prefab.close", { { "discard", true } });

		f.Run("prefab.open", { { "path", f.Path } });
		f.Run("entity.create", { { "name", "SecondRoot" } });
		CHECK(Contains(f.RunError("prefab.save"), "exactly one root"));
		f.Run("prefab.close", { { "discard", true } });

		f.Run("prefab.open", { { "path", f.Path } });
		const auto edit = f.Session.GetEditScene();
		const std::string original = *FileSystem::ReadTextFile(Basalt::Project::ResolvePath(f.Path));
		f.Directory.WriteFile(f.Path, original + " ");
		CHECK(Contains(f.RunError("prefab.save"), "changed on disk"));
		CHECK(f.Session.GetEditScene() == edit);
		f.Run("prefab.save", { { "force", true } });
		f.Run("prefab.close");
		f.Directory.WriteFile(f.Path, "{}");
		const auto scene = f.Session.GetEditScene();
		f.RunError("prefab.open", { { "path", f.Path } });
		CHECK(f.Session.GetEditScene() == scene);
		CHECK_FALSE(f.Session.IsEditingPrefab());
	}

	TEST_CASE("Removed source components: untouched ones go, edited ones stay as overrides; unpack keeps the tree")
	{
		PrefabFixture f;
		Entity removed = f.Instantiate();
		Entity untouched = f.Instantiate();
		Entity edited = f.Instantiate();
		removed.RemoveComponent<PointLightComponent>();
		edited.GetComponent<PointLightComponent>().Intensity = 6;
		f.Source.GetComponent<PointLightComponent>().Radius = 30;
		f.SaveSource();
		CHECK_FALSE(removed.HasComponent<PointLightComponent>());
		f.Run("prefab.revert", { { "entity", IdOf(removed) } });
		CHECK(removed.GetComponent<PointLightComponent>().Radius == 30);

		f.Source.RemoveComponent<PointLightComponent>();
		f.SaveSource();
		CHECK_FALSE(untouched.HasComponent<PointLightComponent>());
		REQUIRE(edited.HasComponent<PointLightComponent>());
		CHECK(edited.GetComponent<PointLightComponent>().Intensity == 6);
		CHECK(f.Overrides(edited).contains(f.EntityPath("Source") + "/Components/PointLight"));
		f.Run("prefab.revert", { { "entity", IdOf(edited) } });
		CHECK_FALSE(edited.HasComponent<PointLightComponent>());

		f.Run("prefab.unpack", { { "entity", IdOf(removed) } });
		CHECK_FALSE(removed.HasComponent<PrefabComponent>());
		CHECK(removed.GetChildren().size() == 1);
		f.Source.AddComponent<AudioListenerComponent>();
		CHECK(f.SaveSource()["instances"] == 2);
	}

	TEST_CASE("An entity the source deletes stays when the instance edited it, and is not duplicated later")
	{
		PrefabFixture f;
		Entity instance = f.Instantiate();
		Entity child = instance.GetChildren().front();
		child.AddComponent<PointLightComponent>().Intensity = 2;
		f.Session.GetScene()->DestroyEntity(f.Child);
		f.SaveSource();
		REQUIRE(child.IsValid());
		f.Source.GetComponent<PointLightComponent>().Radius = 2;
		f.SaveSource();
		CHECK(instance.GetChildren().size() == 1);
		f.Run("prefab.revert", { { "entity", IdOf(instance) } });
		CHECK_FALSE(child.IsValid());
		CHECK(instance.GetChildren().empty());
	}

	TEST_CASE("Apply shares overrides with other instances, and reverting one field keeps the rest")
	{
		PrefabFixture f;
		Entity first = f.Instantiate();
		Entity second = f.Instantiate();
		first.GetComponent<PointLightComponent>().Intensity = 11;
		first.GetComponent<PointLightComponent>().Radius = 25;
		const auto id = IdOf(first);
		f.Run("prefab.revert", { { "entity", id }, { "field", f.FieldPath("Source", "PointLight", "Radius") } });
		CHECK(first.GetComponent<PointLightComponent>().Radius == f.Source.GetComponent<PointLightComponent>().Radius);
		CHECK(first.GetComponent<PointLightComponent>().Intensity == 11);
		CHECK(Contains(f.RunError("prefab.revert", { { "entity", id }, { "field", "/1/Components/PointLight/Radius" } }), "not an override"));
		// first, second and the authoring tree.
		CHECK(f.Run("prefab.apply", { { "entity", id } })["instances"] == 3);
		CHECK(second.GetComponent<PointLightComponent>().Intensity == 11);
		CHECK(second.GetTransform().Translation.x == 5);
		CHECK_FALSE(f.Overrides(first).contains(f.FieldPath("Source", "PointLight", "Intensity")));
	}

	TEST_CASE("Apply writes deletions and component changes, merges with external edits, and keeps outside references local")
	{
		PrefabFixture f;
		auto scene = f.Session.GetScene();
		Entity first = f.Instantiate();
		Entity second = f.Instantiate();
		Entity secondLocal = scene->CreateEntity("SecondLocal");
		scene->SetParent(secondLocal, PrefabFixture::ChildNamed(second, "Child"), false);
		Entity added = scene->CreateEntity("Added");
		scene->SetParent(added, first, false);
		first.AddComponent<AudioListenerComponent>();
		first.RemoveComponent<PointLightComponent>();
		first.AddComponent<BoneAttachmentComponent>().Target = added.GetUUID();
		scene->DestroyEntity(PrefabFixture::ChildNamed(first, "Child"));
		// Someone else edited the file meanwhile; Apply merges into it rather than overwriting it.
		json file = ParseJson(*FileSystem::ReadTextFile(Basalt::Project::ResolvePath(f.Path)));
		file["Entities"][0]["Name"] = "Renamed";
		f.Directory.WriteFile(f.Path, file.dump());
		const json result = f.Run("prefab.apply", { { "entity", IdOf(first) } });
		REQUIRE(result["warnings"].size() == 1);
		CHECK(Contains(result["warnings"][0].get<std::string>(), "outside the prefab"));
		const json source = f.ReadSource();
		REQUIRE(source.size() == 1);
		CHECK(source[0]["Name"] == "Renamed");
		CHECK(source[0]["Components"].contains("AudioListener"));
		CHECK_FALSE(source[0]["Components"].contains("PointLight"));
		CHECK(source[0]["Components"]["BoneAttachment"]["Target"] == 0);
		CHECK(second.HasComponent<AudioListenerComponent>());
		CHECK_FALSE(second.HasComponent<PointLightComponent>());
		CHECK_FALSE(PrefabFixture::ChildNamed(second, "Child"));
		CHECK(secondLocal.GetParent() == second);
		CHECK(added.GetParent() == first);
		CHECK(first.GetComponent<BoneAttachmentComponent>().Target == added.GetUUID());
	}

	TEST_CASE("Apply puts a member under an added entity back under its source parent; moving one out deletes it")
	{
		PrefabFixture f;
		auto scene = f.Session.GetScene();
		Entity first = f.Instantiate();
		Entity added = scene->CreateEntity("Added");
		scene->SetParent(added, first, false);
		Entity child = PrefabFixture::ChildNamed(first, "Child");
		scene->SetParent(child, added, false);
		f.Run("prefab.apply", { { "entity", IdOf(first) } });
		json source = f.ReadSource();
		REQUIRE(source.size() == 2);
		CHECK(source[1]["Parent"] == source[0]["ID"]);
		CHECK(child.GetParent() == added);

		Entity outside = scene->CreateEntity("Outside");
		scene->SetParent(child, outside, false);
		f.Run("prefab.apply", { { "entity", IdOf(first) } });
		CHECK(f.ReadSource().size() == 1);
		CHECK(child.GetParent() == outside);
	}

	TEST_CASE("Asset moves migrate instance baselines without creating false overrides, including moving the prefab")
	{
		PrefabFixture f;
		f.Directory.WriteFile("Assets/Script.lua", "return {}");
		f.Source.AddComponent<ScriptComponent>().Script = "Assets/Script.lua";
		f.SaveSource();
		Entity instance = f.Instantiate();
		const auto id = IdOf(instance);
		f.Run("scene.save", { { "path", "Assets/Main.bscene" } });
		f.Run("asset.move", { { "from", "Assets/Script.lua" }, { "to", "Assets/Moved.lua" } });
		CHECK(instance.GetComponent<ScriptComponent>().Script == "Assets/Moved.lua");
		CHECK(f.Overrides(instance).empty());
		const json opened = f.Run("scene.open", { { "path", "Assets/Main.bscene" } });
		// Up to date, with asset references in its baseline: reopening merges nothing and leaves the scene clean.
		CHECK_FALSE(opened.contains("prefabInstances"));
		CHECK_FALSE(f.Session.IsDirty());
		Entity loaded = f.Session.GetScene()->GetEntityByUUID(UUID(id));
		CHECK(loaded.GetComponent<ScriptComponent>().Script == "Assets/Moved.lua");
		CHECK(f.Overrides(loaded).empty());
		f.Run("prefab.refresh", { { "entity", id } });
		CHECK(loaded.GetComponent<ScriptComponent>().Script == "Assets/Moved.lua");

		f.Run("asset.move", { { "from", f.Path }, { "to", "Assets/Prefabs/Renamed.bprefab" } });
		CHECK(loaded.GetComponent<PrefabComponent>().Prefab == "Assets/Prefabs/Renamed.bprefab");
		f.Path = "Assets/Prefabs/Renamed.bprefab";
		Entity source = f.Session.GetScene()->CreateEntity("Source");
		source.AddComponent<PointLightComponent>().Radius = 44;
		f.Source = source;
		// loaded and the original authoring tree, saved in Main.bscene; the new tree is linked before the merge.
		CHECK(f.SaveSource()["instances"] == 2);
		CHECK(loaded.GetComponent<PointLightComponent>().Radius == 44);
	}

	TEST_CASE("Invalid merged data leaves every instance unchanged and is reported per instance")
	{
		PrefabFixture f;
		Entity instance = f.Instantiate();
		f.Instantiate();
		std::string error;
		json source = f.ReadSource();
		source[0]["Components"]["PointLight"]["Radius"] = -5;
		const auto before = SceneSerializer::SerializeScene(*f.Session.GetScene());
		CHECK(Prefab::Refresh(instance, source, Prefab::RefreshMode::Merge, error) == Prefab::RefreshResult::Rejected);
		CHECK(SceneSerializer::SerializeScene(*f.Session.GetScene()) == before);
		CHECK_FALSE(error.empty());

		const auto report = f.WriteExternally([](json& file) { file["Entities"][0]["Components"]["PointLight"]["Radius"] = -5; });
		CHECK(report.PrefabInstances == 0);
		CHECK(report.PrefabErrors.size() == 3);
		CHECK(SceneSerializer::SerializeScene(*f.Session.GetScene()) == before);
	}

	TEST_CASE("Hierarchy merges: a real cycle is rejected unchanged, a reorder through cyclic intermediate states applies")
	{
		PrefabFixture f;
		auto scene = f.Session.GetScene();
		Entity second = scene->CreateEntity("Second");
		scene->SetParent(second, f.Source, false);
		f.SaveSource();
		Entity instance = f.Instantiate();
		Entity child = PrefabFixture::ChildNamed(instance, "Child");
		Entity instanceSecond = PrefabFixture::ChildNamed(instance, "Second");
		// The instance moves Second under Child; the source then moves Child under Second.
		scene->SetParent(instanceSecond, child, false);
		Entity other = f.Instantiate();
		json source = f.ReadSource();
		const uint64_t secondId = f.SourceId("Second");
		for (json& record : source)
			if (record["Name"] == "Child")
				record["Parent"] = secondId;
		std::string error;
		const auto before = SceneSerializer::SerializeScene(*scene);
		CHECK(Prefab::Refresh(instance, source, Prefab::RefreshMode::Merge, error) == Prefab::RefreshResult::Rejected);
		CHECK(Contains(error, "cycle"));
		CHECK(SceneSerializer::SerializeScene(*scene) == before);

		scene->SetParent(f.Child, second, false);
		const json result = f.SaveSource();
		CHECK(result["instances"] == 1);
		REQUIRE(result["warnings"].size() == 1);
		CHECK(Contains(result["warnings"][0].get<std::string>(), std::to_string(IdOf(instance))));
		CHECK(PrefabFixture::ChildNamed(PrefabFixture::ChildNamed(other, "Second"), "Child"));
		CHECK(instanceSecond.GetParent() == child);

		// A hand-written source swaps them back, listing Second (now under Child) first. Parenting one record at a time
		// would put Second under Child while Child is still under Second, and fail; the merge applies it.
		Entity otherChild = PrefabFixture::ChildNamed(PrefabFixture::ChildNamed(other, "Second"), "Child");
		json swapped = f.ReadSource();
		REQUIRE(swapped[1]["Name"] == "Second");
		swapped[1]["Parent"] = swapped[2]["ID"];
		swapped[2]["Parent"] = swapped[0]["ID"];
		CHECK(Prefab::Refresh(other, swapped, Prefab::RefreshMode::Merge, error) == Prefab::RefreshResult::Merged);
		CHECK(otherChild.GetParent() == other);
		CHECK(PrefabFixture::ChildNamed(otherChild, "Second"));
	}

	TEST_CASE("Instances saved before override tracking are adopted without changing them")
	{
		PrefabFixture f;
		auto scene = f.Session.GetScene();
		Entity legacy = scene->CreateEntity("Legacy");
		legacy.AddComponent<PointLightComponent>().Intensity = 9;
		Entity child = scene->CreateEntity("Child");
		scene->SetParent(child, legacy, false);
		Entity extra = scene->CreateEntity("Extra");
		scene->SetParent(extra, legacy, false);
		legacy.AddComponent<PrefabComponent>().Prefab = f.Path;
		const json result = f.Run("prefab.overrides", { { "entity", IdOf(legacy) } });
		CHECK(result["tracked"] == false);
		CHECK(result["overrides"].empty());

		// Saving the source adopts it: matching entities become members and differences become overrides.
		f.Source.GetComponent<PointLightComponent>().Radius = 17;
		CHECK(f.SaveSource()["instances"] == 1);
		const auto& link = legacy.GetComponent<PrefabComponent>();
		CHECK(link.Members[std::to_string(f.SourceId("Child"))] == IdOf(child));
		CHECK(legacy.GetComponent<PointLightComponent>().Intensity == 9);
		CHECK(legacy.GetName() == "Legacy");
		CHECK(extra.GetParent() == legacy);
		const json overrides = f.Overrides(legacy);
		CHECK(overrides.contains(f.FieldPath("Source", "PointLight", "Intensity")));
		CHECK(overrides.contains(f.FieldPath("Source", "PointLight", "Radius")));

		// From now on unchanged fields follow the source.
		f.Source.GetComponent<PointLightComponent>().Color = { 0.5f, 0.5f, 0.5f };
		f.SaveSource();
		CHECK(legacy.GetComponent<PointLightComponent>().Color.x == doctest::Approx(0.5f));
		CHECK(legacy.GetComponent<PointLightComponent>().Intensity == 9);

		// Reverting an untracked instance adopts it first, then discards its differences.
		Entity other = scene->CreateEntity("Source");
		other.AddComponent<PointLightComponent>().Intensity = 3;
		other.AddComponent<PrefabComponent>().Prefab = f.Path;
		f.Run("prefab.revert", { { "entity", IdOf(other) } });
		CHECK(other.GetComponent<PointLightComponent>().Intensity == f.Source.GetComponent<PointLightComponent>().Intensity);
		CHECK(PrefabFixture::ChildNamed(other, "Child"));
	}

	TEST_CASE("Opening a scene merges instances of prefabs saved since, and validation reports stale scenes")
	{
		PrefabFixture f;
		Entity instance = f.Instantiate();
		instance.GetComponent<PointLightComponent>().Intensity = 4;
		const UUID id = instance.GetUUID();
		f.Run("scene.save", { { "path", "Assets/Main.bscene" } });
		// Written while another scene is open, so no instance of Main.bscene merges.
		f.Run("scene.new");
		f.Source = f.Session.GetScene()->CreateEntity("Source");
		f.Source.AddComponent<PointLightComponent>().Radius = 12;
		Entity child = f.Session.GetScene()->CreateEntity("Child");
		f.Session.GetScene()->SetParent(child, f.Source, false);
		f.SaveSource();
		const json validation = f.Run("project.validate");
		bool reported = false;
		for (const json& issue : validation["issues"])
			reported |= issue["file"] == "Assets/Main.bscene" && Contains(issue["message"].get<std::string>(), "predate its last save");
		CHECK(reported);
		const json opened = f.Run("scene.open", { { "path", "Assets/Main.bscene" } });
		// The instance and the authoring tree saved with it.
		CHECK(opened["prefabInstances"] == 2);
		Entity loaded = f.Session.GetScene()->GetEntityByUUID(id);
		CHECK(loaded.GetComponent<PointLightComponent>().Radius == 12);
		CHECK(loaded.GetComponent<PointLightComponent>().Intensity == 4);
		CHECK(f.Session.IsDirty());
		f.Run("scene.save");
		CHECK_FALSE(f.Run("scene.open", { { "path", "Assets/Main.bscene" } }).contains("prefabInstances"));
		CHECK_FALSE(f.Session.IsDirty());
		const json current = f.Run("project.validate");
		for (const json& issue : current["issues"])
			CHECK_FALSE(Contains(issue["message"].get<std::string>(), "predate its last save"));

		// A deleted prefab: opening reports each instance, changes nothing and validation adds no outdated warning.
		std::filesystem::remove(Basalt::Project::ResolvePath(f.Path));
		const json missing = f.Run("scene.open", { { "path", "Assets/Main.bscene" } });
		CHECK(missing["warnings"].size() == 2);
		CHECK_FALSE(f.Session.IsDirty());
		const json afterDelete = f.Run("project.validate");
		for (const json& issue : afterDelete["issues"])
			CHECK_FALSE(Contains(issue["message"].get<std::string>(), "predate its last save"));
	}

	TEST_CASE("Instances created during Play track membership only; prefab writes during Play merge into the edit scene")
	{
		PrefabFixture f;
		Entity edit = f.Instantiate();
		std::string error;
		REQUIRE_MESSAGE(f.Session.StartPlay(error), error);
		Entity instance = f.EntityFrom(f.Run("prefab.instantiate", { { "path", f.Path } }));
		const auto& link = instance.GetComponent<PrefabComponent>();
		CHECK(link.Members.size() == 2);
		CHECK(link.Baseline.empty());
		CHECK(Contains(f.RunError("prefab.refresh", { { "entity", IdOf(instance) } }), "stop Play"));
		f.RunError("prefab.apply", { { "entity", IdOf(instance) } });
		Entity playCopy = f.Session.GetScene()->GetEntityByUUID(edit.GetUUID());
		const auto report = f.WriteExternally([](json& file) { file["Entities"][0]["Components"]["PointLight"]["Radius"] = 33; });
		CHECK(report.PrefabInstances == 2);
		CHECK(playCopy.GetComponent<PointLightComponent>().Radius != 33);
		f.Session.StopPlay();
		CHECK(edit.GetComponent<PointLightComponent>().Radius == 33);
	}

	TEST_CASE("Prefab files need unique nonzero IDs, one root first and a supported version; nested links are dropped")
	{
		PrefabFixture f;
		const std::string badPath = "Assets/Prefabs/Bad.bprefab";
		auto writeBadPrefab = [&](const json& entities, int version = 3) {
			f.Directory.WriteFile(badPath, json{ { "Format", "BasaltPrefab" }, { "Version", version }, { "Entities", entities } }.dump());
		};
		const auto before = f.Session.GetScene()->GetEntityCount();
		const json root = { { "ID", 10 }, { "Name", "Root" }, { "Parent", 0 }, { "Components", json::object() } };
		const json child = { { "ID", 11 }, { "Name", "Child" }, { "Parent", 10 }, { "Components", { { "Prefab", { { "Prefab", "Assets/Prefabs/Other.bprefab" } } } } } };
		writeBadPrefab(json::array({ root, json{ { "ID", 0 }, { "Name", "Zero" }, { "Parent", 10 } } }));
		CHECK(Contains(f.RunError("prefab.instantiate", { { "path", badPath } }), "unique nonzero 'ID'"));
		writeBadPrefab(json::array({ root, json{ { "ID", 10 }, { "Name", "Twin" }, { "Parent", 10 } } }));
		CHECK(Contains(f.RunError("prefab.instantiate", { { "path", badPath } }), "duplicate ID 10"));
		writeBadPrefab(json::array({ root, json{ { "ID", 12 }, { "Name", "Second root" }, { "Parent", 0 } } }));
		CHECK(Contains(f.RunError("prefab.instantiate", { { "path", badPath } }), "exactly one root"));
		writeBadPrefab(json::array({ child, root }));
		f.RunError("prefab.instantiate", { { "path", badPath } });
		writeBadPrefab(json::array({ root }), 99);
		CHECK(Contains(f.RunError("prefab.instantiate", { { "path", badPath } }), "newer than the supported version"));
		f.Directory.WriteFile(badPath, json{ { "Format", "BasaltScene" }, { "Entities", json::array({ root }) } }.dump());
		CHECK(Contains(f.RunError("prefab.instantiate", { { "path", badPath } }), "expected \"BasaltPrefab\""));
		// These pass the structure checks and fail while creating entities: nothing created is left behind.
		writeBadPrefab(json::array({ root, child, json{ { "ID", 13 }, { "Name", "Lost" }, { "Parent", 999 } } }));
		CHECK(Contains(f.RunError("prefab.instantiate", { { "path", badPath } }), "unknown parent"));
		writeBadPrefab(json::array({ root, json{ { "ID", 14 }, { "Name", "Bad light" }, { "Parent", 10 }, { "Components", { { "PointLight", { { "Radius", -5 } } } } } } }));
		f.RunError("prefab.instantiate", { { "path", badPath } });
		CHECK(f.Session.GetScene()->GetEntityCount() == before);
		CHECK_FALSE(f.Session.GetScene()->FindEntityByName("Root"));

		writeBadPrefab(json::array({ root, child }));
		Entity instance = f.EntityFrom(f.Run("prefab.instantiate", { { "path", badPath } }));
		CHECK_FALSE(instance.GetChildren().front().HasComponent<PrefabComponent>());
	}

	TEST_CASE("prefab.create saves nested instances unpacked, with a warning; nested local instances are left alone")
	{
		PrefabFixture f;
		Entity holder = f.Session.GetScene()->CreateEntity("Holder");
		Entity nested = f.Instantiate();
		f.Session.GetScene()->SetParent(nested, holder, false);
		const json result = f.Run("prefab.create", { { "entity", IdOf(holder) }, { "path", "Assets/Prefabs/Holder.bprefab" } });
		REQUIRE(result["warnings"].size() == 1);
		CHECK(Contains(result["warnings"][0].get<std::string>(), "unpacked"));
		CHECK(result["linked"] == false);
		CHECK_FALSE(holder.HasComponent<PrefabComponent>());
		const auto text = FileSystem::ReadTextFile(Basalt::Project::ResolvePath("Assets/Prefabs/Holder.bprefab"));
		REQUIRE(text);
		CHECK_FALSE(Contains(*text, "\"Prefab\""));

		// A nested instance placed as a local child of another instance is left alone by its merges.
		Entity outer = f.Instantiate();
		Entity inner = f.Instantiate();
		f.Session.GetScene()->SetParent(inner, PrefabFixture::ChildNamed(outer, "Child"), false);
		inner.GetComponent<PointLightComponent>().Intensity = 5;
		f.Run("prefab.revert", { { "entity", IdOf(outer) } });
		CHECK(inner.GetParent() == PrefabFixture::ChildNamed(outer, "Child"));
		CHECK(inner.GetComponent<PointLightComponent>().Intensity == 5);
		CHECK(f.Overrides(outer).empty());
	}

	TEST_CASE("External prefab edits merge when the session reloads changed files, and results report them")
	{
		PrefabFixture f;
		Entity instance = f.Instantiate();
		f.Session.ClearDirty();
		const auto report = f.WriteExternally([](json& file) { file["Entities"][0]["Components"]["PointLight"]["Radius"] = 42; });
		// The instance and the authoring tree.
		CHECK(report.PrefabInstances == 2);
		CHECK(report.PrefabErrors.empty());
		CHECK(instance.GetComponent<PointLightComponent>().Radius == 42);
		CHECK(f.Session.IsDirty());

		json file = ParseJson(*FileSystem::ReadTextFile(Basalt::Project::ResolvePath(f.Path)));
		file["Entities"][0]["Components"]["PointLight"]["Radius"] = 43;
		CHECK(f.Run("asset.write", { { "path", f.Path }, { "content", file.dump() } })["prefabInstances"] == 2);
		file["Entities"][0]["Components"]["PointLight"]["Radius"] = -1;
		const json invalid = f.Run("asset.write", { { "path", f.Path }, { "content", file.dump() } });
		CHECK(invalid["prefabInstances"] == 0);
		CHECK(invalid["warnings"].size() == 2);
		CHECK(instance.GetComponent<PointLightComponent>().Radius == 43);

		// A deleted prefab leaves its instances as they are, with a warning.
		std::filesystem::remove(Basalt::Project::ResolvePath(f.Path));
		const auto removed = f.Session.ReloadAssets({ { f.Path, FileChangeType::Removed } });
		CHECK(removed.PrefabInstances == 0);
		REQUIRE(removed.PrefabErrors.size() == 2);
		CHECK(instance.GetComponent<PointLightComponent>().Radius == 43);
	}

	TEST_CASE("Merging an unchanged source changes nothing")
	{
		PrefabFixture f;
		auto scene = f.Session.GetScene();
		Entity instance = f.Instantiate();
		instance.GetComponent<PointLightComponent>().Intensity = 2;
		instance.AddComponent<AudioListenerComponent>();
		Entity local = scene->CreateEntity("Local");
		scene->SetParent(local, instance, false);
		scene->DestroyEntity(PrefabFixture::ChildNamed(instance, "Child"));
		const json before = SceneSerializer::SerializeScene(*scene);
		std::string error;
		CHECK(Prefab::Refresh(instance, f.ReadSource(), Prefab::RefreshMode::Merge, error) == Prefab::RefreshResult::Unchanged);
		CHECK(f.Run("prefab.refresh", { { "entity", IdOf(instance) } })["overrides"].size() == 3);
		CHECK(SceneSerializer::SerializeScene(*scene) == before);
	}

	TEST_CASE("Rotations that survive a save and load as other Euler angles are not overrides")
	{
		PrefabFixture f;
		auto scene = f.Session.GetScene();
		f.Child.GetTransform().Rotation = Math::QuatFromEuler(glm::radians(glm::vec3(30.0f, 45.0f, 60.0f)));
		Entity gimbal = scene->CreateEntity("Gimbal");
		scene->SetParent(gimbal, f.Source, false);
		gimbal.GetTransform().Rotation = Math::QuatFromEuler(glm::radians(glm::vec3(10.0f, 90.0f, 20.0f)));
		f.SaveSource();
		Entity instance = f.Instantiate();
		const UUID id = instance.GetUUID();
		f.Run("scene.save", { { "path", "Assets/Main.bscene" } });
		for (int i = 0; i < 3; ++i)
		{
			f.Run("scene.open", { { "path", "Assets/Main.bscene" } });
			f.Run("scene.save");
		}
		Entity loaded = f.Session.GetScene()->GetEntityByUUID(id);
		CHECK(f.Overrides(loaded).empty());
		std::string error;
		CHECK(Prefab::Refresh(loaded, f.ReadSource(), Prefab::RefreshMode::Merge, error) == Prefab::RefreshResult::Unchanged);
	}

	TEST_CASE("Tracking from Lua (signed IDs) round-trips; malformed tracking is rejected")
	{
		PrefabFixture f;
		Entity instance = f.Instantiate();
		json data = ComponentRegistry::Find("Prefab")->Serialize(instance);
		// Lua hands IDs back as signed integers; large UUIDs become negative.
		for (auto& value : data["Members"])
			value = static_cast<int64_t>(value.get<uint64_t>());
		for (auto& record : data["Baseline"])
			record["ID"] = static_cast<int64_t>(record["ID"].get<uint64_t>());
		std::string error;
		REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(instance, "Prefab", data, error), error);
		CHECK(instance.GetComponent<PrefabComponent>().Members == ComponentRegistry::Find("Prefab")->Serialize(instance)["Members"]);
		CHECK(f.Overrides(instance).empty());
		const json bigRecord = { { "ID", -1 }, { "Name", "Big" }, { "Parent", 0 }, { "Components", json::object() } };
		Entity big = f.Session.GetScene()->CreateEntity("Big");
		REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(big, "Prefab", { { "Prefab", f.Path }, { "Members", { { "18446744073709551615", -1 } } }, { "Baseline", { { "18446744073709551615", bigRecord } } } }, error), error);
		CHECK(big.GetComponent<PrefabComponent>().Members["18446744073709551615"] == UINT64_MAX);

		Entity probe = f.Session.GetScene()->CreateEntity("Probe");
		const auto rejected = [&](json members, json baseline) {
			std::string problem;
			const bool ok = ComponentRegistry::AddOrPatch(probe, "Prefab", { { "Prefab", f.Path }, { "Members", std::move(members) }, { "Baseline", std::move(baseline) } }, problem);
			return !ok && !problem.empty();
		};
		const json record = { { "ID", 5 }, { "Name", "R" }, { "Parent", 0 }, { "Components", json::object() } };
		const auto with = [&](const json& patch) {
			json copy = record;
			copy.merge_patch(patch);
			return copy;
		};
		CHECK(rejected({ { "01", 7 } }, json::object()));
		CHECK(rejected({ { "abc", 7 } }, json::object()));
		CHECK(rejected({ { "18446744073709551616", 7 } }, json::object()));
		CHECK(rejected({ { "5", 7 }, { "6", 7 } }, json::object()));
		CHECK(rejected({ { "5", 0 } }, json::object()));
		CHECK(rejected({ { "5", 7 } }, { { "6", record } }));
		CHECK(rejected({ { "5", 7 } }, { { "5", with({ { "ID", 6 } }) } }));
		CHECK(rejected({ { "5", 7 } }, { { "5", with({ { "Components", { { "Prefab", json::object() } } } }) } }));
		CHECK(rejected({ { "5", 7 } }, { { "5", with({ { "Parent", 9 } }) } }));
		CHECK(rejected(json::array({ 1 }), json::object()));
		CHECK(rejected({ { "5", 7 }, { "6", 8 } }, { { "5", record }, { "6", with({ { "ID", 6 } }) } }));

		// Another spelling of the same path keeps the tracking; naming another prefab without new tracking restarts it.
		REQUIRE(ComponentRegistry::AddOrPatch(instance, "Prefab", { { "Prefab", "Assets/Prefabs/./Test.bprefab" } }, error));
		CHECK(instance.GetComponent<PrefabComponent>().Prefab == f.Path);
		CHECK_FALSE(instance.GetComponent<PrefabComponent>().Members.empty());
		REQUIRE(ComponentRegistry::AddOrPatch(instance, "Prefab", { { "Prefab", "Assets/Prefabs/Other.bprefab" } }, error));
		CHECK(instance.GetComponent<PrefabComponent>().Members.empty());
		CHECK(instance.GetComponent<PrefabComponent>().Baseline.empty());
	}

	TEST_CASE("Prefab paths are normalized, so every spelling of a path merges")
	{
		PrefabFixture f;
		Entity instance = f.EntityFrom(f.Run("prefab.instantiate", { { "path", "Assets/Prefabs/./Test.bprefab" } }));
		CHECK(instance.GetComponent<PrefabComponent>().Prefab == f.Path);
		f.Source.GetComponent<PointLightComponent>().Radius = 19;
		CHECK(f.SaveSource()["instances"] == 1);
		CHECK(instance.GetComponent<PointLightComponent>().Radius == 19);
	}

	TEST_CASE("A prefab whose root was replaced is reported per instance and changes nothing")
	{
		PrefabFixture f;
		Entity instance = f.Instantiate();
		const json before = SceneSerializer::SerializeScene(*f.Session.GetScene());
		const auto report = f.WriteExternally([](json& file) {
			file["Entities"][0]["ID"] = 777;
			file["Entities"][1]["Parent"] = 777;
		});
		REQUIRE(report.PrefabErrors.size() == 2);
		CHECK(Contains(report.PrefabErrors[0], "root entity changed"));
		CHECK(SceneSerializer::SerializeScene(*f.Session.GetScene()) == before);
		CHECK(Contains(f.RunError("prefab.refresh", { { "entity", IdOf(instance) } }), "root entity changed"));
	}

	TEST_CASE("A merge never destroys or overwrites entities outside its instance, even with hand-edited members")
	{
		PrefabFixture f;
		auto scene = f.Session.GetScene();
		Entity first = f.Instantiate();
		Entity second = f.Instantiate();
		Entity outside = scene->CreateEntity("Outside");
		auto& link = first.GetComponent<PrefabComponent>();
		link.Members["999"] = IdOf(outside);
		link.Members[std::to_string(f.SourceId("Child"))] = IdOf(second);
		f.Run("prefab.revert", { { "entity", IdOf(first) } });
		CHECK(outside.IsValid());
		CHECK(outside.GetName() == "Outside");
		REQUIRE(second.IsValid());
		CHECK(second.HasComponent<PrefabComponent>());
		CHECK_FALSE(second.GetParent());
		CHECK(second.GetChildren().size() == 1);

		// Another instance's child named as a member is not claimed either.
		Entity third = f.Instantiate();
		Entity secondChild = PrefabFixture::ChildNamed(second, "Child");
		secondChild.AddComponent<AudioListenerComponent>();
		third.GetComponent<PrefabComponent>().Members[std::to_string(f.SourceId("Child"))] = IdOf(secondChild);
		f.Run("prefab.revert", { { "entity", IdOf(third) } });
		CHECK(secondChild.GetParent() == second);
		CHECK(secondChild.HasComponent<AudioListenerComponent>());
		CHECK(PrefabFixture::ChildNamed(third, "Child") != secondChild);

		// A member the instance moved out of its tree stays in the scene when the source deletes it.
		Entity child = PrefabFixture::ChildNamed(second, "Child");
		scene->SetParent(child, outside, false);
		scene->DestroyEntity(f.Child);
		f.SaveSource();
		CHECK(child.IsValid());
		CHECK(child.GetParent() == outside);
	}

	TEST_CASE("Revert All keeps the instance's additions and parent, and moved members leave the instance")
	{
		PrefabFixture f;
		auto scene = f.Session.GetScene();
		Entity holder = scene->CreateEntity("Holder");
		Entity instance = f.Instantiate();
		scene->SetParent(instance, holder, false);
		Entity child = PrefabFixture::ChildNamed(instance, "Child");
		Entity underChild = scene->CreateEntity("UnderChild");
		scene->SetParent(underChild, child, false);
		Entity underRoot = scene->CreateEntity("UnderRoot");
		scene->SetParent(underRoot, instance, false);
		Entity outside = scene->CreateEntity("Outside");
		Entity second = f.Instantiate();
		Entity secondChild = PrefabFixture::ChildNamed(second, "Child");
		scene->SetParent(secondChild, outside, false);
		f.Run("prefab.revert", { { "entity", IdOf(instance) } });
		CHECK(child.GetParent() == instance);
		CHECK(underChild.GetParent() == child);
		CHECK(underRoot.GetParent() == instance);
		CHECK(instance.GetParent() == holder);
		// A member moved out of the tree is not the instance's any more: Revert recreates its own, the moved one stays.
		f.Run("prefab.revert", { { "entity", IdOf(second) } });
		CHECK(secondChild.GetParent() == outside);
		REQUIRE(PrefabFixture::ChildNamed(second, "Child"));
		CHECK(PrefabFixture::ChildNamed(second, "Child") != secondChild);
	}

	TEST_CASE("Reverting single overrides restores removed components, drops added ones and recreates deleted entities")
	{
		PrefabFixture f;
		Entity instance = f.Instantiate();
		const UUID childID = instance.GetChildren().front().GetUUID();
		instance.RemoveComponent<PointLightComponent>();
		instance.AddComponent<AudioListenerComponent>();
		f.Session.GetScene()->DestroyEntity(instance.GetChildren().front());
		f.Run("prefab.revert", { { "entity", IdOf(instance) }, { "field", f.EntityPath("Source") + "/Components/PointLight" } });
		CHECK(instance.HasComponent<PointLightComponent>());
		f.Run("prefab.revert", { { "entity", IdOf(instance) }, { "field", f.EntityPath("Source") + "/Components/AudioListener" } });
		CHECK_FALSE(instance.HasComponent<AudioListenerComponent>());
		f.Run("prefab.revert", { { "entity", IdOf(instance) }, { "field", f.EntityPath("Child") } });
		REQUIRE(instance.GetChildren().size() == 1);
		CHECK(instance.GetChildren().front().GetUUID() == childID);
		CHECK(f.Overrides(instance).empty());
	}

	TEST_CASE("Saving a prefab merges each of many instances")
	{
		PrefabFixture f;
		constexpr int Count = 120;
		std::vector<Entity> instances;
		instances.reserve(Count);
		for (int i = 0; i < Count; ++i)
			instances.push_back(f.Instantiate());
		instances[7].GetComponent<PointLightComponent>().Intensity = 3;
		f.Source.GetComponent<PointLightComponent>().Intensity = 6;
		CHECK(f.SaveSource()["instances"] == Count);
		for (int i = 0; i < Count; ++i)
			CHECK(instances[static_cast<size_t>(i)].GetComponent<PointLightComponent>().Intensity == (i == 7 ? 3 : 6));
	}

	TEST_CASE("prefab.create keeps a tree that belongs to another instance or prefab as it is")
	{
		PrefabFixture f;
		auto scene = f.Session.GetScene();
		Entity instance = f.Instantiate();
		Entity child = PrefabFixture::ChildNamed(instance, "Child");
		const json part = f.Run("prefab.create", { { "entity", IdOf(child) }, { "path", "Assets/Prefabs/Part.bprefab" } });
		CHECK(part["linked"] == false);
		REQUIRE(part["warnings"].size() == 1);
		CHECK(Contains(part["warnings"][0].get<std::string>(), "stays part of the prefab instance"));
		CHECK_FALSE(child.HasComponent<PrefabComponent>());
		CHECK(f.Overrides(instance).empty());

		// An instance of Test written over Other stays an instance of Test; Other keeps its IDs by name.
		Entity other = scene->CreateEntity("Source");
		Entity otherChild = scene->CreateEntity("Child");
		scene->SetParent(otherChild, other, false);
		f.Run("prefab.create", { { "entity", IdOf(other) }, { "path", "Assets/Prefabs/Other.bprefab" } });
		Entity otherInstance = f.EntityFrom(f.Run("prefab.instantiate", { { "path", "Assets/Prefabs/Other.bprefab" } }));
		json otherSource;
		std::string error;
		REQUIRE(Prefab::ReadSource(Basalt::Project::ResolvePath("Assets/Prefabs/Other.bprefab"), otherSource, error));
		instance.GetComponent<PointLightComponent>().Radius = 23;
		const json result = f.Run("prefab.create", { { "entity", IdOf(instance) }, { "path", "Assets/Prefabs/Other.bprefab" } });
		CHECK(result["linked"] == false);
		REQUIRE(result["warnings"].size() == 1);
		CHECK(Contains(result["warnings"][0].get<std::string>(), "stays an instance of Assets/Prefabs/Test.bprefab"));
		CHECK(instance.GetComponent<PrefabComponent>().Prefab == f.Path);
		json rewritten;
		REQUIRE(Prefab::ReadSource(Basalt::Project::ResolvePath("Assets/Prefabs/Other.bprefab"), rewritten, error));
		CHECK(rewritten[0]["ID"] == otherSource[0]["ID"]);
		CHECK(otherInstance.GetComponent<PointLightComponent>().Radius == 23);
	}

	TEST_CASE("Overwriting a prefab from an untracked instance of it keeps the file's IDs")
	{
		PrefabFixture f;
		auto scene = f.Session.GetScene();
		Entity tracked = f.Instantiate();
		Entity legacy = scene->CreateEntity("Source");
		legacy.AddComponent<PointLightComponent>().Radius = 27;
		Entity child = scene->CreateEntity("Child");
		scene->SetParent(child, legacy, false);
		legacy.AddComponent<PrefabComponent>().Prefab = f.Path;
		const json before = f.ReadSource();
		const json result = f.Run("prefab.create", { { "entity", IdOf(legacy) }, { "path", f.Path } });
		CHECK(result["warnings"].empty());
		CHECK(result["linked"] == true);
		const json after = f.ReadSource();
		CHECK(after[0]["ID"] == before[0]["ID"]);
		CHECK(after[1]["ID"] == before[1]["ID"]);
		CHECK(tracked.GetComponent<PointLightComponent>().Radius == 27);
	}

	TEST_CASE("Hand-edited members never claim an ordinary entity outside the instance")
	{
		PrefabFixture f;
		auto scene = f.Session.GetScene();
		Entity instance = f.Instantiate();
		Entity holder = scene->CreateEntity("Holder");
		Entity outsider = scene->CreateEntity("Outsider");
		outsider.AddComponent<AudioListenerComponent>();
		scene->SetParent(outsider, holder, false);
		instance.GetComponent<PrefabComponent>().Members[std::to_string(f.SourceId("Child"))] = IdOf(outsider);
		f.Run("prefab.revert", { { "entity", IdOf(instance) } });
		CHECK(outsider.GetName() == "Outsider");
		CHECK(outsider.GetParent() == holder);
		CHECK(outsider.HasComponent<AudioListenerComponent>());
		CHECK(instance.GetComponent<PrefabComponent>().Members[std::to_string(f.SourceId("Child"))] != IdOf(outsider));
	}

	TEST_CASE("Entities added under removed members move to the root in tree order")
	{
		PrefabFixture f;
		auto scene = f.Session.GetScene();
		Entity second = scene->CreateEntity("Second");
		scene->SetParent(second, f.Source, false);
		f.SaveSource();
		Entity instance = f.Instantiate();
		std::vector<Entity> locals;
		for (const std::string name : { "Child", "Second" })
			for (int i = 0; i < 3; ++i)
			{
				Entity local = scene->CreateEntity(name + "Local" + std::to_string(i));
				scene->SetParent(local, PrefabFixture::ChildNamed(instance, name), false);
				locals.push_back(local);
			}
		scene->DestroyEntity(f.Child);
		scene->DestroyEntity(second);
		f.SaveSource();
		const auto children = instance.GetChildren();
		REQUIRE(children.size() == locals.size());
		for (size_t i = 0; i < locals.size(); ++i)
			CHECK(children[i] == locals[i]);
	}

	TEST_CASE("Small real edits are overrides; Euler triples naming the same rotation are not")
	{
		PrefabFixture f;
		Entity instance = f.Instantiate();
		Entity child = PrefabFixture::ChildNamed(instance, "Child");
		child.GetTransform().Rotation = glm::angleAxis(glm::radians(0.5f), glm::vec3(0, 1, 0));
		instance.GetComponent<PointLightComponent>().Intensity *= 1.0001f;
		const json overrides = f.Overrides(instance);
		CHECK(overrides.contains(f.FieldPath("Child", "Transform", "Rotation")));
		CHECK(overrides.contains(f.FieldPath("Source", "PointLight", "Intensity")));
		f.Child.GetTransform().Rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(1, 0, 0));
		f.SaveSource();
		CHECK(child.GetTransform().Rotation.y == doctest::Approx(std::sin(glm::radians(0.25f))));

		Entity other = f.Instantiate();
		json& rotation = other.GetComponent<PrefabComponent>().Baseline[std::to_string(f.SourceId("Child"))]["Components"]["Transform"]["Rotation"];
		rotation[0] = rotation[0].get<double>() + 360.0;
		CHECK(f.Overrides(other).empty());
		std::string error;
		CHECK(Prefab::Refresh(other, f.ReadSource(), Prefab::RefreshMode::Merge, error) == Prefab::RefreshResult::Unchanged);
	}

	TEST_CASE("A merge that would exceed the hierarchy depth limit is rejected unchanged")
	{
		PrefabFixture f;
		auto scene = f.Session.GetScene();
		Entity parent;
		for (uint32_t i = 0; i + 1 < Scene::MaxHierarchyDepth; ++i)
		{
			Entity level = scene->CreateEntity("Level");
			if (parent)
				REQUIRE(scene->SetParent(level, parent, false));
			parent = level;
		}
		Entity instance = f.Instantiate();
		REQUIRE(scene->SetParent(instance, parent, false));
		json source = f.ReadSource();
		source.push_back({ { "ID", 4242 }, { "Name", "Deep" }, { "Parent", source[1]["ID"] }, { "Components", json::object() } });
		const json before = SceneSerializer::SerializeScene(*scene);
		std::string error;
		CHECK(Prefab::Refresh(instance, source, Prefab::RefreshMode::Merge, error) == Prefab::RefreshResult::Rejected);
		CHECK(Contains(error, std::to_string(Scene::MaxHierarchyDepth) + " levels"));
		CHECK(SceneSerializer::SerializeScene(*scene) == before);
	}

	TEST_CASE("Tracking round-trips through Lua, signed IDs and entity references included")
	{
		PrefabFixture f;
		f.Source.AddComponent<BoneAttachmentComponent>().Target = f.Child.GetUUID();
		f.SaveSource();
		Entity instance = f.Instantiate();
		sol::state lua;
		const json original = ComponentRegistry::Find("Prefab")->Serialize(instance);
		const json back = LuaToJson(JsonToLua(lua, original));
		std::string error;
		REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(instance, "Prefab", back, error), error);
		CHECK(instance.GetComponent<PrefabComponent>().Members == original["Members"]);
		CHECK(f.Overrides(instance).empty());
		std::string refreshError;
		CHECK(Prefab::Refresh(instance, f.ReadSource(), Prefab::RefreshMode::Merge, refreshError) == Prefab::RefreshResult::Unchanged);
	}

	TEST_CASE("scene.open leaves untracked instances for an explicit merge")
	{
		PrefabFixture f;
		auto scene = f.Session.GetScene();
		scene->DestroyEntity(f.Source);
		Entity legacy = scene->CreateEntity("Legacy");
		legacy.AddComponent<PrefabComponent>().Prefab = f.Path;
		const UUID id = legacy.GetUUID();
		f.Run("scene.save", { { "path", "Assets/Main.bscene" } });
		f.Run("scene.new");
		f.Source = f.Session.GetScene()->CreateEntity("Source");
		f.Source.AddComponent<PointLightComponent>().Radius = 3;
		f.SaveSource();
		const json opened = f.Run("scene.open", { { "path", "Assets/Main.bscene" } });
		CHECK_FALSE(opened.contains("prefabInstances"));
		CHECK_FALSE(f.Session.IsDirty());
		CHECK(f.Run("prefab.overrides", { { "entity", static_cast<uint64_t>(id) } })["tracked"] == false);
	}

	TEST_CASE("asset.move over a scene file with a malformed baseline migrates it or fails cleanly")
	{
		PrefabFixture f;
		f.Directory.WriteFile("Assets/Script.lua", "return {}");
		const json record = { { "Name", "NoID" }, { "Parent", 0 }, { "Components", { { "Script", { { "Script", "Assets/Script.lua" } } } } } };
		json file = { { "Format", "BasaltScene" }, { "Version", 3 }, { "Entities", json::array({ { { "ID", 5 }, { "Name", "Holder" }, { "Parent", 0 }, { "Components", { { "Prefab", { { "Prefab", f.Path }, { "Members", { { "9", 5 } } }, { "Baseline", { { "9", record } } } } } } } } }) } };
		f.Directory.WriteFile("Assets/Broken.bscene", file.dump());
		// The record has no ID, which the migration never reads.
		f.Run("asset.move", { { "from", "Assets/Script.lua" }, { "to", "Assets/Moved.lua" } });
		CHECK(Contains(*FileSystem::ReadTextFile(Basalt::Project::ResolvePath("Assets/Broken.bscene")), "\"UUID\""));
		file["Entities"][0]["Components"]["Prefab"]["Baseline"]["9"] = 7;
		f.Directory.WriteFile("Assets/Broken.bscene", file.dump());
		CHECK(Contains(f.RunError("asset.move", { { "from", "Assets/Moved.lua" }, { "to", "Assets/Again.lua" } }), "is not an entity record"));
		CHECK(std::filesystem::exists(Basalt::Project::ResolvePath("Assets/Moved.lua")));
	}

	TEST_CASE("prefab.create leaves trees unlinked while a prefab is open or the game runs")
	{
		PrefabFixture f;
		f.Run("prefab.open", { { "path", f.Path } });
		CHECK(f.Run("prefab.create", { { "entity", "Child" }, { "path", "Assets/Prefabs/Part.bprefab" } })["linked"] == false);
		f.Run("prefab.save");
		f.Run("prefab.close");
		std::string error;
		REQUIRE_MESSAGE(f.Session.StartPlay(error), error);
		const json played = f.Run("entity.create", { { "name", "Played" } });
		CHECK(f.Run("prefab.create", { { "entity", played["id"] }, { "path", "Assets/Prefabs/Played.bprefab" } })["linked"] == false);
		f.Session.StopPlay();
	}

	TEST_CASE("Apply on an untracked instance applies its differences and keeps source entities it lacks")
	{
		PrefabFixture f;
		auto scene = f.Session.GetScene();
		Entity legacy = scene->CreateEntity("Source");
		legacy.AddComponent<PointLightComponent>().Intensity = 9;
		Entity renamed = scene->CreateEntity("Renamed");
		scene->SetParent(renamed, legacy, false);
		legacy.AddComponent<PrefabComponent>().Prefab = f.Path;
		const json result = f.Run("prefab.apply", { { "entity", IdOf(legacy) } });
		REQUIRE(result["warnings"].size() == 1);
		CHECK(Contains(result["warnings"][0].get<std::string>(), "the prefab keeps it"));
		const json source = f.ReadSource();
		REQUIRE(source.size() == 2);
		CHECK(source[0]["Components"]["PointLight"]["Intensity"] == 9);
		CHECK(source[1]["Name"] == "Child");
		CHECK(f.Run("prefab.overrides", { { "entity", IdOf(legacy) } })["tracked"] == true);
	}

	TEST_CASE("Apply writes references to the prefab's own entities as source IDs")
	{
		PrefabFixture f;
		Entity first = f.Instantiate();
		Entity second = f.Instantiate();
		PrefabFixture::ChildNamed(first, "Child").AddComponent<BoneAttachmentComponent>().Target = first.GetUUID();
		CHECK(f.Run("prefab.apply", { { "entity", IdOf(first) } })["warnings"].empty());
		CHECK(f.ReadSource()[1]["Components"]["BoneAttachment"]["Target"] == f.SourceId("Source"));
		CHECK(PrefabFixture::ChildNamed(second, "Child").GetComponent<BoneAttachmentComponent>().Target == second.GetUUID());
	}

	TEST_CASE("project.open merges outdated instances of its start scene")
	{
		PrefabFixture f;
		Entity instance = f.Instantiate();
		const UUID id = instance.GetUUID();
		f.Run("scene.save", { { "path", "Assets/Main.bscene" } });
		f.Run("project.set", { { "startScene", "Assets/Main.bscene" } });
		f.Run("scene.new");
		f.Source = f.Session.GetScene()->CreateEntity("Source");
		f.Source.AddComponent<PointLightComponent>().Radius = 14;
		Entity child = f.Session.GetScene()->CreateEntity("Child");
		f.Session.GetScene()->SetParent(child, f.Source, false);
		f.SaveSource();
		const json opened = f.Run("project.open", { { "path", FileSystem::PathToUtf8(f.Directory.GetDirectory()) } });
		CHECK(opened["prefabInstances"].get<size_t>() >= 1);
		CHECK(f.Session.GetScene()->GetEntityByUUID(id).GetComponent<PointLightComponent>().Radius == 14);
		CHECK(f.Session.IsDirty());
	}

	TEST_CASE("scene.hash ignores prefab tracking")
	{
		PrefabFixture f;
		Entity instance = f.Instantiate();
		const json hash = f.Run("scene.hash")["hash"];
		auto& link = instance.GetComponent<PrefabComponent>();
		link.Baseline = json::object();
		link.Members = { { "1", 99 } };
		CHECK(f.Run("scene.hash")["hash"] == hash);
		instance.GetComponent<PointLightComponent>().Radius = 99;
		CHECK(f.Run("scene.hash")["hash"] != hash);
	}

	TEST_CASE("Reloads report edit-scene changes, and asset.delete reports instances of the deleted prefab")
	{
		PrefabFixture f;
		f.Instantiate();
		const auto report = f.WriteExternally([](json& file) { file["Entities"][0]["Components"]["PointLight"]["Radius"] = 8; });
		// The instance and the authoring tree.
		CHECK(report.PrefabEditSceneChanges == 2);
		CHECK(f.Run("asset.delete", { { "path", f.Path } })["warnings"].size() == 2);
	}
}
