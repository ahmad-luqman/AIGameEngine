#include "AutomationTestUtils.h"
#include "TestUtils.h"

#include <doctest/doctest.h>

#include <Basalt/Core/FileSystem.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Prefab.h>
#include <Basalt/Scene/SceneSerializer.h>

using namespace Basalt;
using namespace BasaltTest;
using json = nlohmann::json;

namespace {
	uint64_t IdOf(Entity entity)
	{
		return static_cast<uint64_t>(entity.GetUUID());
	}

	// A source tree Source > Child saved as Assets/Prefabs/Test.bprefab; SaveSource writes edits to it again, which
	// merges them into the scene's instances.
	struct PrefabFixture
	{
		TempProject Project{ "PrefabEditing" };
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

		Entity Instantiate()
		{
			auto result = Run(Commands, Session, "prefab.instantiate", { { "path", Path }, { "position", { 5, 0, 0 } } });
			return Session.GetScene()->GetEntityByUUID(UUID(result["id"].get<uint64_t>()));
		}

		json SaveSource()
		{
			return Run(Commands, Session, "prefab.create", { { "entity", IdOf(Source) }, { "path", Path } });
		}

		json Overrides(Entity instance)
		{
			return Run(Commands, Session, "prefab.overrides", { { "entity", IdOf(instance) } })["overrides"];
		}

		std::string FieldPath(Entity source, const std::string& component, const std::string& field) const
		{
			return "/" + std::to_string(IdOf(source)) + "/Components/" + component + "/" + field;
		}

		json ReadSource() const
		{
			json source;
			std::string error;
			REQUIRE_MESSAGE(Prefab::ReadSource(Basalt::Project::ResolvePath(Path), source, error), error);
			return source;
		}

		Entity ChildNamed(Entity parent, const std::string& name) const
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
		CHECK(overrides.contains(f.FieldPath(f.Source, "PointLight", "Intensity")));
		CHECK(overrides.contains("/" + std::to_string(IdOf(f.Source)) + "/Components/AudioListener"));
		CHECK_FALSE(overrides.contains(f.FieldPath(f.Source, "PointLight", "Radius")));
		Run(f.Commands, f.Session, "prefab.revert", { { "entity", IdOf(instance) } });
		CHECK(instance.GetComponent<PointLightComponent>().Intensity == 4);
		CHECK_FALSE(instance.HasComponent<AudioListenerComponent>());
		CHECK(instance.GetTransform().Translation.x == 5);
		CHECK(f.Overrides(instance).empty());
	}

	TEST_CASE("The root's position and rotation are placement; its scale is an override")
	{
		PrefabFixture f;
		Entity first = f.Instantiate();
		Entity second = f.Instantiate();
		first.GetTransform().Translation = { 1, 2, 3 };
		first.GetTransform().Rotation = glm::angleAxis(1.0f, glm::vec3(0, 1, 0));
		first.GetTransform().Scale = glm::vec3(2);
		const json overrides = f.Overrides(first);
		CHECK_FALSE(overrides.contains(f.FieldPath(f.Source, "Transform", "Translation")));
		CHECK_FALSE(overrides.contains(f.FieldPath(f.Source, "Transform", "Rotation")));
		CHECK(overrides.contains(f.FieldPath(f.Source, "Transform", "Scale")));
		RunError(f.Commands, f.Session, "prefab.revert", { { "entity", IdOf(first) }, { "field", f.FieldPath(f.Source, "Transform", "Translation") } });

		Run(f.Commands, f.Session, "prefab.apply", { { "entity", IdOf(first) } });
		CHECK(second.GetTransform().Scale.x == doctest::Approx(2));
		CHECK(second.GetTransform().Translation.x == doctest::Approx(5));
		CHECK(f.ReadSource()[0]["Components"]["Transform"]["Translation"] == json::array({ 0.0, 0.0, 0.0 }));

		second.GetTransform().Scale = glm::vec3(3);
		Run(f.Commands, f.Session, "prefab.revert", { { "entity", IdOf(second) } });
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
		CHECK_FALSE(root.GetComponent<PrefabComponent>().Overrides.empty());
		Entity duplicate = SceneSerializer::DeserializeEntityTree(*restored, SceneSerializer::SerializeEntityTree(root), {}, error);
		REQUIRE_MESSAGE(duplicate, error);
		CHECK(duplicate.GetComponent<PrefabComponent>().Members != root.GetComponent<PrefabComponent>().Members);
		REQUIRE_MESSAGE(Prefab::Refresh(duplicate, f.ReadSource(), Prefab::RefreshMode::Merge, error), error);
		CHECK(duplicate.GetComponent<PointLightComponent>().Intensity == 8);
		CHECK(root.GetChildren().front().GetUUID() != duplicate.GetChildren().front().GetUUID());
	}

	TEST_CASE("A copy of an instance with a deleted entity restores its own entity, not the original's")
	{
		PrefabFixture f;
		Entity instance = f.Instantiate();
		auto scene = f.Session.GetScene();
		scene->DestroyEntity(instance.GetChildren().front());
		CHECK(f.Overrides(instance).contains("/" + std::to_string(IdOf(f.Child))));
		std::string error;
		Entity copy = SceneSerializer::DeserializeEntityTree(*scene, SceneSerializer::SerializeEntityTree(instance), {}, error);
		REQUIRE_MESSAGE(copy, error);
		Run(f.Commands, f.Session, "prefab.revert", { { "entity", IdOf(instance) } });
		Run(f.Commands, f.Session, "prefab.revert", { { "entity", IdOf(copy) } });
		REQUIRE(instance.GetChildren().size() == 1);
		REQUIRE(copy.GetChildren().size() == 1);
		CHECK(instance.GetChildren().front() != copy.GetChildren().front());
		CHECK(instance.GetChildren().front().GetParent() == instance);
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
		link.Members[std::to_string(IdOf(f.Child))] = IdOf(second);
		Run(f.Commands, f.Session, "prefab.refresh", { { "entity", IdOf(first) } });
		CHECK(outside.IsValid());
		CHECK(outside.GetName() == "Outside");
		REQUIRE(second.IsValid());
		CHECK(second.HasComponent<PrefabComponent>());
		CHECK_FALSE(second.GetParent());
		CHECK(second.GetChildren().size() == 1);
		CHECK(f.ChildNamed(first, "Child"));

		// A member the instance moved out of its tree stays in the scene when the source deletes it.
		Entity child = f.ChildNamed(second, "Child");
		scene->SetParent(child, outside, false);
		scene->DestroyEntity(f.Child);
		f.SaveSource();
		CHECK(child.IsValid());
		CHECK(child.GetParent() == outside);
	}

	TEST_CASE("Editing a prefab in isolation keeps the waiting scene and merges on save")
	{
		PrefabFixture f;
		Entity instance = f.Instantiate();
		const auto scene = f.Session.GetEditScene();
		const auto id = instance.GetUUID();
		f.Session.SetSelection(id);
		f.Session.MarkDirty();
		Run(f.Commands, f.Session, "prefab.open", { { "path", f.Path } });
		CHECK(f.Session.IsEditingPrefab());
		CHECK(f.Session.GetEditScene() != scene);
		CHECK(f.Session.IsWaitingSceneDirty());
		CHECK_FALSE(f.Session.IsDirty());
		CHECK(Contains(RunError(f.Commands, f.Session, "scene.new"), "while 'Assets/Prefabs/Test.bprefab' is open"));
		RunError(f.Commands, f.Session, "scene.save");
		RunError(f.Commands, f.Session, "play.start");
		RunError(f.Commands, f.Session, "prefab.open", { { "path", f.Path } });
		RunError(f.Commands, f.Session, "asset.move", { { "from", f.Path }, { "to", "Assets/Prefabs/Moved.bprefab" } });
		std::string error;
		CHECK_FALSE(f.Session.StartPlay(error));
		Run(f.Commands, f.Session, "component.set", { { "entity", "Source" }, { "component", "PointLight" }, { "data", { { "Radius", 20 } } } });
		RunError(f.Commands, f.Session, "prefab.close");
		CHECK(Run(f.Commands, f.Session, "prefab.save")["instances"] == 1);
		CHECK(instance.GetComponent<PointLightComponent>().Radius == 20);
		Run(f.Commands, f.Session, "prefab.close");
		CHECK_FALSE(f.Session.IsEditingPrefab());
		CHECK(f.Session.GetEditScene() == scene);
		CHECK(f.Session.GetSelection() == id);
		CHECK(f.Session.IsDirty());
	}

	TEST_CASE("Saving rejects external edits, nested instances and extra roots; failed opens change nothing")
	{
		PrefabFixture f;
		Run(f.Commands, f.Session, "prefab.open", { { "path", f.Path } });
		Run(f.Commands, f.Session, "prefab.instantiate", { { "path", f.Path }, { "parent", "Source" } });
		CHECK(Contains(RunError(f.Commands, f.Session, "prefab.save"), "unpack the nested prefab instance"));
		Run(f.Commands, f.Session, "prefab.close", { { "discard", true } });

		Run(f.Commands, f.Session, "prefab.open", { { "path", f.Path } });
		Run(f.Commands, f.Session, "entity.create", { { "name", "SecondRoot" } });
		CHECK(Contains(RunError(f.Commands, f.Session, "prefab.save"), "exactly one root"));
		Run(f.Commands, f.Session, "prefab.close", { { "discard", true } });

		Run(f.Commands, f.Session, "prefab.open", { { "path", f.Path } });
		const auto edit = f.Session.GetEditScene();
		f.Project.WriteFile(f.Path, "{}");
		CHECK(Contains(RunError(f.Commands, f.Session, "prefab.save"), "changed on disk"));
		CHECK(f.Session.GetEditScene() == edit);
		Run(f.Commands, f.Session, "prefab.close", { { "discard", true } });
		const auto scene = f.Session.GetEditScene();
		RunError(f.Commands, f.Session, "prefab.open", { { "path", f.Path } });
		CHECK(f.Session.GetEditScene() == scene);
		CHECK_FALSE(f.Session.IsEditingPrefab());
	}

	TEST_CASE("Removing source components keeps component deletions; unpack keeps the tree")
	{
		PrefabFixture f;
		Entity instance = f.Instantiate();
		instance.RemoveComponent<PointLightComponent>();
		f.Source.GetComponent<PointLightComponent>().Radius = 30;
		f.SaveSource();
		CHECK_FALSE(instance.HasComponent<PointLightComponent>());
		Run(f.Commands, f.Session, "prefab.revert", { { "entity", IdOf(instance) } });
		CHECK(instance.GetComponent<PointLightComponent>().Radius == 30);
		Run(f.Commands, f.Session, "prefab.unpack", { { "entity", IdOf(instance) } });
		CHECK_FALSE(instance.HasComponent<PrefabComponent>());
		CHECK(instance.GetChildren().size() == 1);
		CHECK(f.SaveSource()["instances"] == 0);
	}

	TEST_CASE("Apply shares overrides with other instances, and reverting one field keeps the rest")
	{
		PrefabFixture f;
		Entity first = f.Instantiate();
		Entity second = f.Instantiate();
		first.GetComponent<PointLightComponent>().Intensity = 11;
		first.GetComponent<PointLightComponent>().Radius = 25;
		const auto id = IdOf(first);
		Run(f.Commands, f.Session, "prefab.revert", { { "entity", id }, { "field", f.FieldPath(f.Source, "PointLight", "Radius") } });
		CHECK(first.GetComponent<PointLightComponent>().Radius == f.Source.GetComponent<PointLightComponent>().Radius);
		CHECK(first.GetComponent<PointLightComponent>().Intensity == 11);
		CHECK(Contains(RunError(f.Commands, f.Session, "prefab.revert", { { "entity", id }, { "field", "/1/Components/PointLight/Radius" } }), "not an override"));
		CHECK(Run(f.Commands, f.Session, "prefab.apply", { { "entity", id } })["instances"] == 2);
		CHECK(second.GetComponent<PointLightComponent>().Intensity == 11);
		CHECK(second.GetTransform().Translation.x == 5);
		CHECK_FALSE(f.Overrides(first).contains(f.FieldPath(f.Source, "PointLight", "Intensity")));
	}

	TEST_CASE("Asset moves migrate instance baselines without creating false overrides")
	{
		PrefabFixture f;
		f.Project.WriteFile("Assets/Script.lua", "return {}");
		f.Source.AddComponent<ScriptComponent>().Script = "Assets/Script.lua";
		f.SaveSource();
		Entity instance = f.Instantiate();
		const auto id = IdOf(instance);
		Run(f.Commands, f.Session, "scene.save", { { "path", "Assets/Main.bscene" } });
		Run(f.Commands, f.Session, "asset.move", { { "from", "Assets/Script.lua" }, { "to", "Assets/Moved.lua" } });
		CHECK(instance.GetComponent<ScriptComponent>().Script == "Assets/Moved.lua");
		CHECK(f.Overrides(instance).empty());
		Run(f.Commands, f.Session, "scene.open", { { "path", "Assets/Main.bscene" } });
		Entity loaded = f.Session.GetScene()->GetEntityByUUID(UUID(id));
		CHECK(loaded.GetComponent<ScriptComponent>().Script == "Assets/Moved.lua");
		CHECK(f.Overrides(loaded).empty());
		Run(f.Commands, f.Session, "prefab.refresh", { { "entity", id } });
		CHECK(loaded.GetComponent<ScriptComponent>().Script == "Assets/Moved.lua");
	}

	TEST_CASE("Invalid merged data leaves every instance unchanged")
	{
		PrefabFixture f;
		Entity instance = f.Instantiate();
		std::string error;
		json source = f.ReadSource();
		source[0]["Components"]["PointLight"]["Radius"] = -5;
		const auto before = SceneSerializer::SerializeScene(*f.Session.GetScene());
		CHECK_FALSE(Prefab::Refresh(instance, source, Prefab::RefreshMode::Merge, error));
		CHECK(SceneSerializer::SerializeScene(*f.Session.GetScene()) == before);
		CHECK_FALSE(error.empty());
	}

	TEST_CASE("A merge that would create a hierarchy cycle is rejected before changing the scene")
	{
		PrefabFixture f;
		auto scene = f.Session.GetScene();
		Entity second = scene->CreateEntity("Second");
		scene->SetParent(second, f.Source, false);
		f.SaveSource();
		Entity instance = f.Instantiate();
		Entity child = f.ChildNamed(instance, "Child");
		Entity instanceSecond = f.ChildNamed(instance, "Second");
		// The instance moves Second under Child; the source then moves Child under Second.
		scene->SetParent(instanceSecond, child, false);
		Entity other = f.Instantiate();
		json source = f.ReadSource();
		for (json& record : source)
			if (record["Name"] == "Child")
				record["Parent"] = IdOf(second);
		std::string error;
		const auto before = SceneSerializer::SerializeScene(*scene);
		CHECK_FALSE(Prefab::Refresh(instance, source, Prefab::RefreshMode::Merge, error));
		CHECK(Contains(error, "cycle"));
		CHECK(SceneSerializer::SerializeScene(*scene) == before);

		// Saved through the editor flow, the conflicting instance is a warning and the other instance merges.
		scene->SetParent(f.Child, second, false);
		const json result = f.SaveSource();
		CHECK(result["instances"] == 1);
		REQUIRE(result["warnings"].size() == 1);
		CHECK(Contains(result["warnings"][0].get<std::string>(), std::to_string(IdOf(instance))));
		CHECK(f.ChildNamed(f.ChildNamed(other, "Second"), "Child"));
		CHECK(instanceSecond.GetParent() == child);
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
		const json result = Run(f.Commands, f.Session, "prefab.overrides", { { "entity", IdOf(legacy) } });
		CHECK(result["overrides"].empty());
		CHECK(result["members"].empty());

		// Saving the source adopts it: matching entities become members and differences become overrides.
		f.Source.GetComponent<PointLightComponent>().Radius = 17;
		CHECK(f.SaveSource()["instances"] == 1);
		const auto& link = legacy.GetComponent<PrefabComponent>();
		CHECK(link.Members[std::to_string(IdOf(f.Child))] == IdOf(child));
		CHECK(legacy.GetComponent<PointLightComponent>().Intensity == 9);
		CHECK(legacy.GetName() == "Legacy");
		CHECK(extra.GetParent() == legacy);
		const json overrides = f.Overrides(legacy);
		CHECK(overrides.contains(f.FieldPath(f.Source, "PointLight", "Intensity")));
		CHECK(overrides.contains(f.FieldPath(f.Source, "PointLight", "Radius")));

		// From now on unchanged fields follow the source.
		f.Source.GetComponent<PointLightComponent>().Color = { 0.5f, 0.5f, 0.5f };
		f.SaveSource();
		CHECK(legacy.GetComponent<PointLightComponent>().Color.x == doctest::Approx(0.5f));
		CHECK(legacy.GetComponent<PointLightComponent>().Intensity == 9);
	}

	TEST_CASE("Instances created during Play track membership only, and Play rejects instance edits")
	{
		PrefabFixture f;
		std::string error;
		REQUIRE_MESSAGE(f.Session.StartPlay(error), error);
		const auto result = Run(f.Commands, f.Session, "prefab.instantiate", { { "path", f.Path } });
		Entity instance = f.Session.GetScene()->GetEntityByUUID(UUID(result["id"].get<uint64_t>()));
		const auto& link = instance.GetComponent<PrefabComponent>();
		CHECK(link.Members.size() == 2);
		CHECK(link.Baseline.empty());
		CHECK(Contains(RunError(f.Commands, f.Session, "prefab.refresh", { { "entity", IdOf(instance) } }), "stop Play"));
		RunError(f.Commands, f.Session, "prefab.apply", { { "entity", IdOf(instance) } });
		f.Session.StopPlay();
	}

	TEST_CASE("Prefab files need unique nonzero IDs and one root; nested links are dropped")
	{
		PrefabFixture f;
		auto instantiate = [&](const json& entities) {
			f.Project.WriteFile("Assets/Prefabs/Bad.bprefab", json{ { "Format", "BasaltPrefab" }, { "Version", 3 }, { "Entities", entities } }.dump());
			return f.Session.GetScene()->GetEntityCount();
		};
		const json root = { { "ID", 10 }, { "Name", "Root" }, { "Parent", 0 }, { "Components", json::object() } };
		const json child = { { "ID", 11 }, { "Name", "Child" }, { "Parent", 10 }, { "Components", { { "Prefab", { { "Prefab", "Assets/Prefabs/Other.bprefab" } } } } } };
		const auto before = instantiate(json::array({ root, json{ { "ID", 0 }, { "Name", "Zero" }, { "Parent", 10 } } }));
		CHECK(Contains(RunError(f.Commands, f.Session, "prefab.instantiate", { { "path", "Assets/Prefabs/Bad.bprefab" } }), "unique nonzero 'ID'"));
		instantiate(json::array({ root, json{ { "ID", 12 }, { "Name", "Second root" }, { "Parent", 0 } } }));
		CHECK(Contains(RunError(f.Commands, f.Session, "prefab.instantiate", { { "path", "Assets/Prefabs/Bad.bprefab" } }), "exactly one root"));
		instantiate(json::array({ child, root }));
		RunError(f.Commands, f.Session, "prefab.instantiate", { { "path", "Assets/Prefabs/Bad.bprefab" } });
		CHECK(f.Session.GetScene()->GetEntityCount() == before);
		instantiate(json::array({ root, child }));
		const auto result = Run(f.Commands, f.Session, "prefab.instantiate", { { "path", "Assets/Prefabs/Bad.bprefab" } });
		Entity instance = f.Session.GetScene()->GetEntityByUUID(UUID(result["id"].get<uint64_t>()));
		CHECK_FALSE(instance.GetChildren().front().HasComponent<PrefabComponent>());
	}

	TEST_CASE("prefab.create saves nested instances unpacked, with a warning")
	{
		PrefabFixture f;
		Entity holder = f.Session.GetScene()->CreateEntity("Holder");
		Entity nested = f.Instantiate();
		f.Session.GetScene()->SetParent(nested, holder, false);
		const json result = Run(f.Commands, f.Session, "prefab.create", { { "entity", IdOf(holder) }, { "path", "Assets/Prefabs/Holder.bprefab" } });
		REQUIRE(result["warnings"].size() == 1);
		CHECK(Contains(result["warnings"][0].get<std::string>(), "unpacked"));
		const auto text = FileSystem::ReadTextFile(Basalt::Project::ResolvePath("Assets/Prefabs/Holder.bprefab"));
		REQUIRE(text);
		CHECK_FALSE(Contains(*text, "\"Prefab\""));
	}

	TEST_CASE("External prefab edits merge when the session reloads changed files")
	{
		PrefabFixture f;
		Entity instance = f.Instantiate();
		f.Session.ClearDirty();
		json file = ParseJson(*FileSystem::ReadTextFile(Basalt::Project::ResolvePath(f.Path)));
		file["Entities"][0]["Components"]["PointLight"]["Radius"] = 42;
		f.Project.WriteFile(f.Path, file.dump());
		const auto report = f.Session.ReloadAssets({ { f.Path, FileChangeType::Modified } });
		CHECK(report.PrefabInstances == 1);
		CHECK(report.PrefabErrors.empty());
		CHECK(instance.GetComponent<PointLightComponent>().Radius == 42);
		CHECK(f.Session.IsDirty());

		// A deleted prefab leaves its instances as they are, with a warning.
		std::filesystem::remove(Basalt::Project::ResolvePath(f.Path));
		const auto removed = f.Session.ReloadAssets({ { f.Path, FileChangeType::Removed } });
		CHECK(removed.PrefabInstances == 0);
		REQUIRE(removed.PrefabErrors.size() == 1);
		CHECK(instance.GetComponent<PointLightComponent>().Radius == 42);
	}

	TEST_CASE("Saving a prefab merges each of many instances once")
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
}
