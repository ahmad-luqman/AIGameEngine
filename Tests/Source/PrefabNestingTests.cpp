#include "AutomationTestUtils.h"
#include "TestUtils.h"

#include <doctest/doctest.h>

#include <Breccia/Asset/Thumbnails.h>
#include <Breccia/Core/FileSystem.h>
#include <Breccia/Scene/ComponentRegistry.h>
#include <Breccia/Scene/Prefab.h>
#include <Breccia/Scene/SceneSerializer.h>

#include <functional>

using namespace Breccia;
using namespace BrecciaTest;
using json = nlohmann::json;

namespace {
	// The first descendant of root named name, depth first.
	Entity Find(Entity root, const std::string& name)
	{
		for (Entity child : root.GetChildren())
		{
			if (child.GetName() == name)
				return child;
			if (Entity found = Find(child, name))
				return found;
		}
		return {};
	}

	float Radius(Entity entity)
	{
		return entity.GetComponent<PointLightComponent>().Radius;
	}

	float GunRadius(Entity root)
	{
		return Radius(Find(root, "Gun"));
	}

	PointLightComponent& GunLight(Scene& scene)
	{
		return scene.FindEntityByName("Gun").GetComponent<PointLightComponent>();
	}

	// Soldier > Gun saved as a prefab, and Squad > (an instance of Soldier) saved as another. The two authoring trees
	// stay in the scene as instances, as prefab.create leaves them.
	struct NestingFixture
	{
		TempProject Directory{ "PrefabNesting" };
		AutomationSession Session;
		CommandRegistry Commands;
		std::string Soldier = "Assets/Prefabs/Soldier.bprefab";
		std::string Squad = "Assets/Prefabs/Squad.bprefab";
		Entity SoldierTree;
		Entity SquadTree;
		std::string Elite = "Assets/Prefabs/Elite.bprefab";

		NestingFixture()
		{
			auto scene = Session.GetScene();
			SoldierTree = scene->CreateEntity("Soldier");
			SoldierTree.AddComponent<PointLightComponent>().Radius = 5;
			Entity gun = scene->CreateEntity("Gun");
			gun.AddComponent<PointLightComponent>().Radius = 7;
			scene->SetParent(gun, SoldierTree, false);
			Run("prefab.create", { { "entity", IdOf(SoldierTree) }, { "path", Soldier } });

			SquadTree = scene->CreateEntity("Squad");
			Run("prefab.instantiate", { { "path", Soldier }, { "parent", IdOf(SquadTree) }, { "position", { 1, 0, 0 } } });
			Run("prefab.create", { { "entity", IdOf(SquadTree) }, { "path", Squad } });
		}

		json Run(const std::string& command, const json& params = json::object())
		{
			return BrecciaTest::Run(Commands, Session, command, params);
		}

		std::string RunError(const std::string& command, const json& params = json::object())
		{
			return BrecciaTest::RunError(Commands, Session, command, params);
		}

		Entity EntityFrom(const json& result)
		{
			return Session.GetScene()->GetEntityByUUID(UUID(result["id"].get<uint64_t>()));
		}

		Entity Instantiate(const std::string& path, const json& position = { 9, 0, 0 })
		{
			return EntityFrom(Run("prefab.instantiate", { { "path", path }, { "position", position } }));
		}

		Entity InstantiateUnder(const std::string& path, Entity parent)
		{
			return EntityFrom(Run("prefab.instantiate", { { "path", path }, { "parent", IdOf(parent) } }));
		}

		// Opens path, runs edit on its scene and saves it; returns what prefab.save reported.
		json Edit(const std::string& path, const std::function<void(Scene&)>& edit)
		{
			Run("prefab.open", { { "path", path } });
			edit(*Session.GetEditScene());
			Session.MarkDirty();
			const json result = Run("prefab.save");
			Run("prefab.close");
			return result;
		}

		json Overrides(Entity instance)
		{
			return Run("prefab.overrides", { { "entity", IdOf(instance) } })["overrides"];
		}

		std::string FileText(const std::string& path) const
		{
			const auto text = FileSystem::ReadTextFile(Breccia::Project::ResolvePath(path));
			REQUIRE(text);
			return *text;
		}

		json FileEntities(const std::string& path) const
		{
			return ParseJson(FileText(path))["Entities"];
		}

		json Info(const std::string& path)
		{
			return Run("prefab.info", { { "path", path } });
		}

		json Variant(const std::string& base, const std::string& path)
		{
			return Run("prefab.variant", { { "base", base }, { "path", path } });
		}

		// Changes a file behind the session's back, as a pull would.
		void WriteRecords(const std::string& path, const std::function<void(json& record)>& edit)
		{
			json file = ParseJson(FileText(path));
			for (json& record : file["Entities"])
				edit(record);
			Directory.WriteFile(path, file.dump());
		}

		// A minimal file: a root that nests prefab count times.
		void WriteNesting(const std::string& path, const std::string& prefab, int count)
		{
			json entities = json::array({ json{ { "ID", 1 }, { "Name", "Root" }, { "Parent", 0 } } });
			for (int i = 0; i < count; ++i)
				entities.push_back({ { "ID", 2 + i }, { "Parent", 1 }, { "Components", { { "Prefab", { { "Prefab", prefab } } } } } });
			Directory.WriteFile(path, json{ { "Format", "BrecciaPrefab" }, { "Version", SceneSerializer::FormatVersion }, { "Entities", entities } }.dump());
		}
	};
}

TEST_SUITE("PrefabNesting")
{
	TEST_CASE("prefab.create saves an instance in the tree as one record holding only its differences")
	{
		NestingFixture f;
		const json entities = f.FileEntities(f.Squad);
		REQUIRE(entities.size() == 2);
		const json nested = entities[1]["Components"]["Prefab"];
		CHECK(entities[1]["Components"].size() == 1);
		CHECK(entities[1]["Parent"] == entities[0]["ID"]);
		CHECK(nested["Prefab"]["Path"] == f.Soldier);
		// The nested root's place in Squad is Squad's, whatever the Soldier's root does; nothing else differs.
		REQUIRE(nested["Overrides"].size() == 1);
		const json placement = nested["Overrides"].begin().value();
		CHECK(placement.size() == 1);
		CHECK(placement["Components"]["Transform"].size() == 2);
		CHECK(placement["Components"]["Transform"]["Translation"] == json{ 1.0, 0.0, 0.0 });

		// The tree is an instance of Squad, tracking everything: its nested root is only a marker.
		const auto& link = f.SquadTree.GetComponent<PrefabComponent>();
		CHECK(link.Prefab == f.Squad);
		CHECK(link.Members.size() == 3);
		Entity soldier = Find(f.SquadTree, "Soldier");
		const auto& marker = soldier.GetComponent<PrefabComponent>();
		CHECK(marker.Prefab == f.Soldier);
		CHECK(marker.Members.empty());
		CHECK(marker.Baseline.empty());
		CHECK(f.Overrides(f.SquadTree).empty());
		CHECK(Prefab::EnclosingInstance(soldier) == f.SquadTree);

		const json info = f.Info(f.Squad);
		CHECK(info["entities"] == 3);
		CHECK(info["base"].is_null());
		REQUIRE(info["nested"].size() == 1);
		CHECK(info["nested"][0]["prefab"] == f.Soldier);
		CHECK(info["dependencies"] == json::array({ f.Soldier }));
		CHECK(info["warnings"].empty());
	}

	TEST_CASE("An inner prefab's edit reaches instances of the prefabs holding it, and those files stay as they are")
	{
		NestingFixture f;
		Entity first = f.Instantiate(f.Squad);
		Entity second = f.Instantiate(f.Squad, { -9, 0, 0 });
		const std::string squadFile = f.FileText(f.Squad);

		const json result = f.Edit(f.Soldier, [](Scene& scene) { GunLight(scene).Radius = 30; });
		// Soldier's authoring tree, Squad's and the two instances; none of them nested on their own.
		CHECK(result["instances"] == 4);
		CHECK(f.FileText(f.Squad) == squadFile);
		for (Entity instance : { first, second, f.SquadTree })
		{
			CHECK(GunRadius(instance) == 30);
			CHECK(f.Overrides(instance).empty());
		}
		// Instantiating reads the same flat source.
		CHECK(GunRadius(f.Instantiate(f.Squad)) == 30);
	}

	TEST_CASE("An outer prefab's overrides and an instance's overrides both survive inner edits, each at its level")
	{
		NestingFixture f;
		f.Edit(f.Squad, [](Scene& scene) { GunLight(scene).Radius = 11; });
		Entity instance = f.Instantiate(f.Squad);
		Entity gun = Find(instance, "Gun");
		CHECK(Radius(gun) == 11);
		CHECK(f.Overrides(instance).empty());
		gun.GetComponent<PointLightComponent>().Intensity = 22;
		f.Session.MarkDirty();

		f.Edit(f.Soldier, [](Scene& scene) {
			auto& light = GunLight(scene);
			light.Radius = 33;
			light.Intensity = 44;
			light.Color = { 0.25f, 0.5f, 0.75f };
		});
		const auto& light = gun.GetComponent<PointLightComponent>();
		CHECK(light.Radius == 11);
		CHECK(light.Intensity == 22);
		CHECK(light.Color == glm::vec3(0.25f, 0.5f, 0.75f));
		// The bare Soldier takes everything; Squad's file still only differs in what it overrode.
		CHECK(GunRadius(f.SoldierTree) == 33);
		const json overrides = f.Overrides(instance);
		REQUIRE(overrides.size() == 1);
		CHECK(Contains(overrides.begin().key(), "/Components/PointLight/Intensity"));
	}

	TEST_CASE("A nested root's place belongs to the prefab holding it; a variant's root always follows its base")
	{
		NestingFixture f;
		f.Variant(f.Soldier, f.Elite);
		f.Edit(f.Soldier, [](Scene& scene) { scene.FindEntityByName("Soldier").GetTransform().Translation = { 0, 5, 0 }; });
		CHECK(Find(f.SquadTree, "Soldier").GetTransform().Translation == glm::vec3(1, 0, 0));
		CHECK(f.Overrides(f.SquadTree).empty());
		json entities;
		std::string error;
		REQUIRE(Prefab::ReadFile(Breccia::Project::ResolvePath(f.Elite), entities, error));
		CHECK(entities[0]["Components"]["Transform"]["Translation"] == json{ 0.0, 5.0, 0.0 });

		// A placed instance keeps its place whatever the file's root does, nested or not.
		Entity instance = f.Instantiate(f.Elite);
		CHECK(instance.GetTransform().Translation == glm::vec3(9, 0, 0));
		f.Edit(f.Soldier, [](Scene& scene) { scene.FindEntityByName("Soldier").GetTransform().Translation = { 0, 6, 0 }; });
		CHECK(instance.GetTransform().Translation == glm::vec3(9, 0, 0));
		CHECK(f.Overrides(instance).empty());

		// Saving a variant whose base moved meanwhile pins nothing, and is not a move of the variant's.
		f.Run("prefab.open", { { "path", f.Elite } });
		f.WriteRecords(f.Soldier, [](json& record) {
			if (record["Name"] == "Soldier")
				record["Components"]["Transform"]["Translation"] = { 0, 7, 0 };
		});
		f.Session.MarkDirty();
		CHECK(f.Run("prefab.save")["warnings"].empty());
		CHECK(f.Session.GetEditScene()->FindEntityByName("Soldier").GetTransform().Translation == glm::vec3(0, 7, 0));
		f.Run("prefab.close");
		CHECK_FALSE(f.FileEntities(f.Elite)[0]["Components"]["Prefab"].contains("Overrides"));

		// Moving an open variant's root is not saved, and it says so.
		f.Run("prefab.open", { { "path", f.Elite } });
		f.Session.GetEditScene()->FindEntityByName("Soldier").GetTransform().Translation = { 3, 3, 3 };
		f.Session.MarkDirty();
		const json saved = f.Run("prefab.save");
		f.Run("prefab.close");
		REQUIRE(saved["warnings"].size() == 1);
		CHECK(Contains(saved["warnings"][0].get<std::string>(), "not saved"));
		CHECK_FALSE(f.FileEntities(f.Elite)[0]["Components"]["Prefab"].contains("Overrides"));
	}

	TEST_CASE("Entities an inner prefab adds or removes appear and go in every instance, once each")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Squad);
		// The instance's own entity under a nested entity stays when that entity's prefab changes.
		auto scene = f.Session.GetScene();
		Entity local = scene->CreateEntity("Local");
		scene->SetParent(local, Find(instance, "Soldier"), false);
		const auto before = scene->GetEntityCount();
		const auto instances = 3; // The instance, Squad's tree and Soldier's tree each gain the entity.

		f.Edit(f.Soldier, [](Scene& edit) {
			Entity scope = edit.CreateEntity("Scope");
			edit.SetParent(scope, edit.FindEntityByName("Gun"), false);
		});
		CHECK(scene->GetEntityCount() == before + instances);
		Entity scope = Find(instance, "Scope");
		REQUIRE(scope);
		CHECK(scope.GetParent() == Find(instance, "Gun"));
		CHECK(f.Overrides(instance).empty());
		CHECK(local.GetParent() == Find(instance, "Soldier"));

		f.Edit(f.Soldier, [](Scene& edit) { edit.DestroyEntity(edit.FindEntityByName("Scope")); });
		CHECK(scene->GetEntityCount() == before);
		CHECK_FALSE(Find(instance, "Scope"));
		CHECK(f.Overrides(instance).empty());
	}

	TEST_CASE("Two nested copies of a prefab are separate entities with separate overrides")
	{
		NestingFixture f;
		Entity holder = f.Session.GetScene()->CreateEntity("Pair");
		Entity left = f.InstantiateUnder(f.Soldier, holder);
		Entity right = f.InstantiateUnder(f.Soldier, holder);
		left.GetComponent<TagComponent>().Tag = "Left";
		right.GetComponent<TagComponent>().Tag = "Right";
		Find(right, "Gun").GetComponent<PointLightComponent>().Radius = 2;
		const std::string path = "Assets/Prefabs/Pair.bprefab";
		f.Run("prefab.create", { { "entity", IdOf(holder) }, { "path", path } });
		CHECK(f.FileEntities(path).size() == 3);

		Entity instance = f.Instantiate(path);
		CHECK(f.Overrides(instance).empty());
		CHECK(instance.GetComponent<PrefabComponent>().Members.size() == 5);
		CHECK(GunRadius(Find(instance, "Left")) == 7);
		CHECK(GunRadius(Find(instance, "Right")) == 2);

		f.Edit(f.Soldier, [](Scene& edit) { GunLight(edit).Radius = 40; });
		CHECK(GunRadius(Find(instance, "Left")) == 40);
		CHECK(GunRadius(Find(instance, "Right")) == 2);
	}

	TEST_CASE("Prefabs nest through several levels, and references follow into nested entities")
	{
		NestingFixture f;
		auto scene = f.Session.GetScene();
		Entity army = scene->CreateEntity("Army");
		Entity squad = f.InstantiateUnder(f.Squad, army);
		Entity pointer = scene->CreateEntity("Pointer");
		scene->SetParent(pointer, army, false);
		pointer.AddComponent<BoneAttachmentComponent>().Target = Find(squad, "Gun").GetUUID();
		const std::string path = "Assets/Prefabs/Army.bprefab";
		const json result = f.Run("prefab.create", { { "entity", IdOf(army) }, { "path", path } });
		CHECK(result["linked"] == true);
		CHECK(result["warnings"].empty());
		// Army, its one nested record and Pointer: Squad's tree is Squad's business.
		CHECK(f.FileEntities(path).size() == 3);
		CHECK(f.Info(path)["dependencies"].size() == 2);
		CHECK(f.Overrides(army).empty());

		Entity instance = f.Instantiate(path);
		CHECK(f.Overrides(instance).empty());
		CHECK(instance.GetComponent<PrefabComponent>().Members.size() == 5);
		Entity gun = Find(instance, "Gun");
		REQUIRE(gun);
		CHECK(Find(instance, "Pointer").GetComponent<BoneAttachmentComponent>().Target == gun.GetUUID());
		// Both inner levels are markers inside it.
		CHECK(Find(instance, "Squad").GetComponent<PrefabComponent>().Prefab == f.Squad);
		CHECK(Find(instance, "Soldier").GetComponent<PrefabComponent>().Prefab == f.Soldier);

		f.Edit(f.Soldier, [](Scene& edit) { GunLight(edit).Radius = 50; });
		CHECK(Radius(gun) == 50);
		CHECK(Find(instance, "Pointer").GetComponent<BoneAttachmentComponent>().Target == gun.GetUUID());
		CHECK(f.Overrides(instance).empty());
	}

	TEST_CASE("An open prefab lists nested overrides and takes inner edits; saving keeps the nested record")
	{
		NestingFixture f;
		f.Run("prefab.open", { { "path", f.Squad } });
		Scene& edit = *f.Session.GetEditScene();
		Entity soldier = edit.FindEntityByName("Soldier");
		Entity gun = edit.FindEntityByName("Gun");
		REQUIRE(soldier);
		// Tracked here like an instance in a scene; a move of the nested root is placement, an edit below it an override.
		CHECK(Prefab::IsTracked(soldier.GetComponent<PrefabComponent>()));
		CHECK(f.Overrides(soldier).empty());
		gun.GetComponent<PointLightComponent>().Radius = 13;
		f.Session.MarkDirty();
		REQUIRE(f.Overrides(soldier).size() == 1);

		// Another prefab written meanwhile merges into the open one's nested instance, not over its edits.
		json soldierFile = ParseJson(f.FileText(f.Soldier));
		for (json& record : soldierFile["Entities"])
			if (record["Name"] == "Gun")
			{
				record["Components"]["PointLight"]["Radius"] = 70;
				record["Components"]["PointLight"]["Intensity"] = 71;
			}
		f.Run("asset.write", { { "path", f.Soldier }, { "content", soldierFile.dump() } });
		CHECK(gun.GetComponent<PointLightComponent>().Radius == 13);
		CHECK(gun.GetComponent<PointLightComponent>().Intensity == 71);

		// What the open prefab adds stays its own: an entity under a nested one, and a new nested instance.
		Entity added = edit.CreateEntity("Added");
		edit.SetParent(added, gun, false);
		Entity pointer = edit.CreateEntity("Pointer");
		edit.SetParent(pointer, edit.FindEntityByName("Squad"), false);
		pointer.AddComponent<BoneAttachmentComponent>().Target = added.GetUUID();
		Entity second = f.InstantiateUnder(f.Soldier, pointer);
		Entity secondGun = Find(second, "Gun");
		pointer.GetComponent<BoneAttachmentComponent>().Target = secondGun.GetUUID();
		second.GetComponent<TagComponent>().Tag = "Second";
		f.Session.MarkDirty();
		f.Run("prefab.save");
		f.Run("prefab.close");

		// Squad, the nested Soldier, Added, Pointer and the second nested Soldier.
		CHECK(f.FileEntities(f.Squad).size() == 5);
		Entity instance = f.Instantiate(f.Squad);
		CHECK(f.Overrides(instance).empty());
		Entity savedSecond = Find(instance, "Second");
		REQUIRE(savedSecond);
		CHECK(Find(instance, "Pointer").GetComponent<BoneAttachmentComponent>().Target == Find(savedSecond, "Gun").GetUUID());
		CHECK(Find(Find(instance, "Gun"), "Added"));
		CHECK(GunRadius(instance) == 13);

		// Opening and saving without edits changes no ID.
		const json entities = f.FileEntities(f.Squad);
		f.Edit(f.Squad, [](Scene&) {});
		CHECK(f.FileEntities(f.Squad) == entities);
	}

	TEST_CASE("Applying an instance writes nested overrides into the prefab holding them, as differences")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Squad);
		Find(instance, "Gun").GetComponent<PointLightComponent>().Radius = 99;
		f.Session.MarkDirty();
		f.Run("prefab.apply", { { "entity", IdOf(instance) } });
		CHECK(f.Overrides(instance).empty());
		CHECK(f.FileEntities(f.Squad).size() == 2);
		CHECK(Contains(f.FileText(f.Squad), "Radius"));
		// Squad's instances take it, Soldier's do not.
		CHECK(GunRadius(f.SquadTree) == 99);
		CHECK(GunRadius(f.SoldierTree) == 7);
		CHECK(Find(instance, "Soldier").HasComponent<PrefabComponent>());

		// Deleting a nested entity in an instance and applying it stores the deletion alone.
		f.Session.GetScene()->DestroyEntity(Find(instance, "Gun"));
		f.Session.MarkDirty();
		f.Run("prefab.apply", { { "entity", IdOf(instance) } });
		CHECK_FALSE(Find(f.SquadTree, "Gun"));
		const json entities = f.FileEntities(f.Squad);
		REQUIRE(entities.size() == 2);
		bool deleted = false;
		for (const auto& [key, patch] : entities[1]["Components"]["Prefab"]["Overrides"].items())
			deleted |= patch.is_null();
		CHECK(deleted);
		CHECK(f.Info(f.Squad)["entities"] == 2);
	}

	TEST_CASE("A nested root is part of the instance holding it: its commands say so, and unpacking one is an override")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Squad);
		Entity soldier = Find(instance, "Soldier");
		for (const char* command : { "prefab.apply", "prefab.refresh", "prefab.revert" })
			CHECK(Contains(f.RunError(command, { { "entity", IdOf(soldier) } }), "nested in the instance 'Squad'"));
		// The rule holds for any caller, not only the commands: alone, the merge would break the instance around it.
		json source;
		std::string error;
		REQUIRE(Prefab::ReadSource(Breccia::Project::ResolvePath(f.Soldier), source, error));
		CHECK(Prefab::Refresh(soldier, source, Prefab::RefreshMode::Merge, error) == Prefab::RefreshResult::Rejected);
		CHECK(Contains(error, "nested in the instance 'Squad'"));
		CHECK_FALSE(Prefab::ListsMembers(soldier.GetComponent<PrefabComponent>()));
		const json overrides = f.Run("prefab.overrides", { { "entity", IdOf(soldier) } });
		CHECK(overrides["tracked"] == false);
		CHECK(overrides["enclosing"] == IdOf(instance));

		f.Run("prefab.unpack", { { "entity", IdOf(soldier) } });
		CHECK_FALSE(soldier.HasComponent<PrefabComponent>());
		const json after = f.Overrides(instance);
		REQUIRE(after.size() == 1);
		CHECK(after.begin().value().is_null());
		CHECK(Contains(after.begin().key(), "/Components/Prefab"));
		f.Run("prefab.revert", { { "entity", IdOf(instance) } });
		CHECK(soldier.GetComponent<PrefabComponent>().Prefab == f.Soldier);
		CHECK(f.Overrides(instance).empty());
	}

	TEST_CASE("Unpacking an outer instance leaves its nested instances tracked, with their differences as overrides")
	{
		NestingFixture f;
		f.Edit(f.Squad, [](Scene& scene) { GunLight(scene).Radius = 11; });
		Entity instance = f.Instantiate(f.Squad);
		f.Run("prefab.unpack", { { "entity", IdOf(instance) } });
		CHECK_FALSE(instance.HasComponent<PrefabComponent>());
		Entity soldier = Find(instance, "Soldier");
		REQUIRE(Prefab::IsTracked(soldier.GetComponent<PrefabComponent>()));
		CHECK_FALSE(Prefab::EnclosingInstance(soldier));
		const json overrides = f.Overrides(soldier);
		REQUIRE(overrides.size() == 1);
		CHECK(overrides.begin().value() == 11);

		f.Edit(f.Soldier, [](Scene& edit) { GunLight(edit).Intensity = 8; });
		Entity gun = Find(soldier, "Gun");
		CHECK(gun.GetComponent<PointLightComponent>().Intensity == 8);
		CHECK(Radius(gun) == 11);
	}

	TEST_CASE("Unpacking keeps deeper nesting inside the instance it leaves tracked")
	{
		NestingFixture f;
		auto scene = f.Session.GetScene();
		Entity army = scene->CreateEntity("Army");
		f.InstantiateUnder(f.Squad, army);
		const std::string path = "Assets/Prefabs/Army.bprefab";
		f.Run("prefab.create", { { "entity", IdOf(army) }, { "path", path } });
		Entity instance = f.Instantiate(path);
		const auto before = scene->GetEntityCount();
		f.Run("prefab.unpack", { { "entity", IdOf(instance) } });
		// The Squad adopts what it finds: its nested Soldier and Gun are members, not additions or deletions.
		Entity squad = Find(instance, "Squad");
		REQUIRE(Prefab::IsTracked(squad.GetComponent<PrefabComponent>()));
		CHECK(f.Overrides(squad).empty());
		Entity soldier = Find(squad, "Soldier");
		CHECK(Prefab::EnclosingInstance(soldier) == squad);
		f.Edit(f.Soldier, [](Scene& edit) { GunLight(edit).Radius = 90; });
		CHECK(GunRadius(soldier) == 90);
		CHECK(scene->GetEntityCount() == before);
	}

	TEST_CASE("A prefab that stops nesting turns its instances' nested roots into plain members")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Squad);
		Entity soldier = Find(instance, "Soldier");
		Entity gun = Find(instance, "Gun");
		f.Run("prefab.open", { { "path", f.Squad } });
		f.Run("prefab.unpack", { { "entity", "Soldier" } });
		f.Run("prefab.save");
		f.Run("prefab.close");
		CHECK(f.FileEntities(f.Squad).size() == 3);
		CHECK_FALSE(Contains(f.FileText(f.Squad), "\"Prefab\""));
		// The same entities, no longer a link: an inner edit stops reaching them.
		CHECK_FALSE(soldier.HasComponent<PrefabComponent>());
		CHECK(Find(instance, "Gun") == gun);
		CHECK(f.Overrides(instance).empty());
		f.Edit(f.Soldier, [](Scene& edit) { GunLight(edit).Radius = 91; });
		CHECK(Radius(gun) == 7);
	}

	TEST_CASE("Ownership stops at an instance that tracks itself, even one a hand-edited Members value lists")
	{
		NestingFixture f;
		auto scene = f.Session.GetScene();
		Entity outer = f.Instantiate(f.Squad);
		Entity local = f.Instantiate(f.Soldier);
		scene->SetParent(local, Find(outer, "Gun"), false);
		CHECK_FALSE(Prefab::EnclosingInstance(local));
		const std::string gunKey = [&] {
			for (const auto& [key, value] : outer.GetComponent<PrefabComponent>().Members.items())
				if (value == IdOf(Find(outer, "Gun")))
					return key;
			return std::string();
		}();
		outer.GetComponent<PrefabComponent>().Members[gunKey + "1"] = IdOf(local);
		CHECK_FALSE(Prefab::EnclosingInstance(local));
		CHECK(local.GetComponent<PrefabComponent>().Prefab == f.Soldier);
	}

	TEST_CASE("An inner edit merges only into tracked instances of the prefabs holding it")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Squad);
		auto& link = instance.GetComponent<PrefabComponent>();
		link.Members = json::object();
		link.Baseline = json::object();
		f.Edit(f.Soldier, [](Scene& edit) { GunLight(edit).Radius = 92; });
		// Adopting would freeze its differences from this very edit as overrides; a write of its own file adopts it.
		CHECK_FALSE(Prefab::IsTracked(instance.GetComponent<PrefabComponent>()));
		CHECK(GunRadius(f.SquadTree) == 92);
		// With nothing around it listing it, its nested root is adopted on its own until then, as any untracked instance.
		CHECK(Prefab::IsTracked(Find(instance, "Soldier").GetComponent<PrefabComponent>()));
		f.Edit(f.Squad, [](Scene& edit) { edit.FindEntityByName("Squad").AddComponent<PointLightComponent>(); });
		CHECK(Prefab::IsTracked(instance.GetComponent<PrefabComponent>()));
		CHECK_FALSE(Prefab::ListsMembers(Find(instance, "Soldier").GetComponent<PrefabComponent>()));
		CHECK(Prefab::EnclosingInstance(Find(instance, "Soldier")) == instance);
		// Adopted: what differed stays, as overrides, and no entity reads as deleted (a deletion's path is the entity's).
		const json overrides = f.Overrides(instance);
		CHECK(overrides.size() == 2);
		for (const auto& [field, value] : overrides.items())
			CHECK(Contains(field, "/Components/"));
	}

	TEST_CASE("Variants inherit their base and store only their differences")
	{
		NestingFixture f;
		const json created = f.Variant(f.Soldier, f.Elite);
		CHECK(created["instances"] == 0);
		const json entities = f.FileEntities(f.Elite);
		REQUIRE(entities.size() == 1);
		CHECK(entities[0]["Parent"] == 0);
		CHECK(entities[0]["Components"].size() == 1);
		CHECK_FALSE(entities[0]["Components"]["Prefab"].contains("Overrides"));
		const json info = f.Info(f.Elite);
		CHECK(info["base"] == f.Soldier);
		CHECK(info["entities"] == 2);

		Entity instance = f.Instantiate(f.Elite);
		CHECK(instance.GetName() == "Soldier");
		CHECK(instance.GetComponent<PrefabComponent>().Prefab == f.Elite);
		CHECK(GunRadius(instance) == 7);
		CHECK(f.Overrides(instance).empty());

		// The variant changes one field, renames its root and adds an entity.
		f.Edit(f.Elite, [](Scene& scene) {
			Entity root = scene.FindEntityByName("Soldier");
			root.GetComponent<TagComponent>().Tag = "Elite";
			GunLight(scene).Radius = 17;
			Entity badge = scene.CreateEntity("Badge");
			scene.SetParent(badge, root, false);
		});
		const json edited = f.FileEntities(f.Elite);
		// The nested root and the variant's own entity; the Gun lives in the root's overrides.
		REQUIRE(edited.size() == 2);
		REQUIRE(edited[0]["Components"]["Prefab"]["Overrides"].size() == 2);
		CHECK(instance.GetName() == "Elite");
		CHECK(GunRadius(instance) == 17);
		CHECK(Find(instance, "Badge"));
		CHECK(f.Overrides(instance).empty());
		// The base did not change.
		CHECK(GunRadius(f.SoldierTree) == 7);
		CHECK_FALSE(Find(f.SoldierTree, "Badge"));

		// The base's edits reach the variant's instances, except what the variant overrides.
		f.Edit(f.Soldier, [](Scene& scene) {
			auto& light = GunLight(scene);
			light.Radius = 60;
			light.Intensity = 61;
			Entity late = scene.CreateEntity("Late");
			scene.SetParent(late, scene.FindEntityByName("Soldier"), false);
		});
		Entity gun = Find(instance, "Gun");
		const auto& light = gun.GetComponent<PointLightComponent>();
		CHECK(light.Radius == 17);
		CHECK(light.Intensity == 61);
		CHECK(Find(instance, "Late"));
		CHECK(f.Overrides(instance).empty());
		CHECK(Find(f.Instantiate(f.Elite), "Late"));
	}

	TEST_CASE("Variants of variants resolve base by base; applying to a variant's instance stays a variant")
	{
		NestingFixture f;
		const std::string captain = "Assets/Prefabs/Captain.bprefab";
		f.Variant(f.Soldier, f.Elite);
		f.Edit(f.Elite, [](Scene& scene) { GunLight(scene).Radius = 17; });
		f.Variant(f.Elite, captain);
		f.Edit(captain, [](Scene& scene) { GunLight(scene).Intensity = 27; });

		Entity instance = f.Instantiate(captain);
		Entity gun = Find(instance, "Gun");
		CHECK(Radius(gun) == 17);
		CHECK(gun.GetComponent<PointLightComponent>().Intensity == 27);
		CHECK(f.Info(captain)["dependencies"].size() == 2);

		// A base two levels up reaches it.
		f.Edit(f.Soldier, [](Scene& scene) { GunLight(scene).Color = { 0.5f, 0.5f, 0.5f }; });
		CHECK(gun.GetComponent<PointLightComponent>().Color == glm::vec3(0.5f));
		CHECK(f.Overrides(instance).empty());

		gun.GetComponent<PointLightComponent>().Radius = 47;
		f.Session.MarkDirty();
		const std::string baseFile = f.FileText(f.Elite);
		f.Run("prefab.apply", { { "entity", IdOf(instance) } });
		CHECK(f.FileEntities(captain).size() == 1);
		CHECK(f.FileText(f.Elite) == baseFile);
		CHECK(f.Info(captain)["base"] == f.Elite);
		CHECK(GunRadius(f.Instantiate(captain)) == 47);
		CHECK(GunRadius(f.Instantiate(f.Elite)) == 17);
	}

	TEST_CASE("prefab.create turns an instance into a variant that tracks it, and keeps a variant one when saved again")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Soldier);
		Entity gun = Find(instance, "Gun");
		gun.GetComponent<PointLightComponent>().Radius = 19;
		f.Session.MarkDirty();

		CHECK(Contains(f.RunError("prefab.create", { { "entity", IdOf(gun) }, { "path", f.Elite }, { "variant", true } }), "not a top-level instance root"));
		CHECK(Contains(f.RunError("prefab.create", { { "entity", IdOf(instance) }, { "path", f.Soldier }, { "variant", true } }), "variant of itself"));
		const json result = f.Run("prefab.create", { { "entity", IdOf(instance) }, { "path", f.Elite }, { "variant", true } });
		CHECK(result["linked"] == true);
		CHECK(result["warnings"].empty());
		CHECK(instance.GetComponent<PrefabComponent>().Prefab == f.Elite);
		CHECK(f.Overrides(instance).empty());
		CHECK(f.FileEntities(f.Elite).size() == 1);
		CHECK(f.Info(f.Elite)["base"] == f.Soldier);
		// Its place in the scene is the instance's, not something the variant inherits to others.
		CHECK_FALSE(Contains(f.FileText(f.Elite), "Translation"));
		CHECK(instance.GetTransform().Translation == glm::vec3(9, 0, 0));
		CHECK(f.Info(f.Elite)["entities"] == 2);
		// The instance is up to date with the file it now tracks, whatever its placement.
		json source;
		std::string error;
		REQUIRE(Prefab::ReadSource(Breccia::Project::ResolvePath(f.Elite), source, error));
		CHECK_FALSE(Prefab::IsOutdated(instance.GetComponent<PrefabComponent>(), source));

		f.Edit(f.Soldier, [](Scene& scene) { GunLight(scene).Intensity = 29; });
		CHECK(gun.GetComponent<PointLightComponent>().Intensity == 29);
		CHECK(Radius(gun) == 19);

		// Saving the instance over its own variant writes the variant again.
		gun.GetComponent<PointLightComponent>().Radius = 39;
		f.Run("prefab.create", { { "entity", IdOf(instance) }, { "path", f.Elite } });
		CHECK(f.FileEntities(f.Elite).size() == 1);
		CHECK(f.Info(f.Elite)["base"] == f.Soldier);
		CHECK(GunRadius(f.Instantiate(f.Elite)) == 39);
	}

	TEST_CASE("prefab.variant needs a readable base and a new file; variants of unreadable bases fail")
	{
		NestingFixture f;
		CHECK(Contains(f.RunError("prefab.variant", { { "base", "Assets/Prefabs/Missing.bprefab" }, { "path", "Assets/Prefabs/V.bprefab" } }), "Missing.bprefab"));
		CHECK_FALSE(FileSystem::Exists(Breccia::Project::ResolvePath("Assets/Prefabs/V.bprefab")));
		CHECK(Contains(f.RunError("prefab.variant", { { "base", f.Soldier }, { "path", f.Squad } }), "already exists"));
		CHECK(Contains(f.RunError("prefab.variant", { { "base", "Assets/Soldier.txt" }, { "path", "Assets/Prefabs/V.bprefab" } }), "expected a .bprefab"));
		f.Run("prefab.variant", { { "base", f.Soldier }, { "path", "Assets/Prefabs/V.bprefab" } });
		std::filesystem::remove(Breccia::Project::ResolvePath(f.Soldier));
		CHECK(Contains(f.RunError("prefab.instantiate", { { "path", "Assets/Prefabs/V.bprefab" } }), "Soldier.bprefab"));
		// Instances of it keep their state and say why, reached through the base they read.
		const auto report = f.Session.ReloadAssets({ { f.Soldier, FileChangeType::Removed } });
		REQUIRE(report.PrefabErrors.size() == 2);
		CHECK(Contains(report.PrefabErrors[0] + report.PrefabErrors[1], "'Squad'"));
		CHECK(Find(f.SquadTree, "Gun"));
	}

	TEST_CASE("Prefabs cannot hold themselves, directly or through others")
	{
		NestingFixture f;
		// Saving: an instance of Squad inside Soldier, which Squad holds.
		f.Run("prefab.open", { { "path", f.Soldier } });
		f.Run("prefab.instantiate", { { "path", f.Squad }, { "parent", "Soldier" } });
		f.Session.MarkDirty();
		const std::string file = f.FileText(f.Soldier);
		CHECK(Contains(f.RunError("prefab.save"), "holds that prefab itself"));
		CHECK(f.FileText(f.Soldier) == file);
		f.Run("prefab.close", { { "discard", true } });
		f.Run("prefab.open", { { "path", f.Soldier } });
		f.Run("prefab.instantiate", { { "path", f.Soldier }, { "parent", "Soldier" } });
		f.Session.MarkDirty();
		CHECK(Contains(f.RunError("prefab.save"), "holds that prefab itself"));
		f.Run("prefab.close", { { "discard", true } });

		// Creating over a prefab a nested instance holds.
		Entity holder = f.Session.GetScene()->CreateEntity("Holder");
		f.InstantiateUnder(f.Squad, holder);
		CHECK(Contains(f.RunError("prefab.create", { { "entity", IdOf(holder) }, { "path", f.Soldier } }), "holds that prefab itself"));
		CHECK(f.FileText(f.Soldier) == file);

		// Reading files written by hand: a cycle is an error, not a crash.
		f.WriteNesting("Assets/Prefabs/A.bprefab", "Assets/Prefabs/B.bprefab", 1);
		f.WriteNesting("Assets/Prefabs/B.bprefab", "Assets/Prefabs/A.bprefab", 1);
		CHECK(Contains(f.RunError("prefab.instantiate", { { "path", "Assets/Prefabs/A.bprefab" } }), "prefab cycle"));
		CHECK(Contains(f.Run("project.validate").dump(), "prefab cycle"));
	}

	TEST_CASE("Reading stops at the nesting and expansion limits, and at links outside the project")
	{
		NestingFixture f;
		std::string error;
		json entities;
		const auto read = [&](const std::string& path) {
			error.clear();
			return Prefab::ReadFile(Breccia::Project::ResolvePath(path), entities, error);
		};
		// A chain one level past the limit; shorter ones read.
		for (size_t i = 0; i <= Prefab::MaxNestingDepth; ++i)
			f.WriteNesting("Assets/Prefabs/Chain" + std::to_string(i) + ".bprefab", "Assets/Prefabs/Chain" + std::to_string(i + 1) + ".bprefab", 1);
		f.WriteNesting("Assets/Prefabs/Chain" + std::to_string(Prefab::MaxNestingDepth + 1) + ".bprefab", f.Soldier, 0);
		CHECK_FALSE(read("Assets/Prefabs/Chain0.bprefab"));
		CHECK(Contains(error, "nested deeper than"));
		CHECK(read("Assets/Prefabs/Chain3.bprefab"));

		// Three small files that would multiply to 256^3 entities.
		f.WriteNesting("Assets/Prefabs/Bomb3.bprefab", f.Soldier, 256);
		f.WriteNesting("Assets/Prefabs/Bomb2.bprefab", "Assets/Prefabs/Bomb3.bprefab", 256);
		f.WriteNesting("Assets/Prefabs/Bomb1.bprefab", "Assets/Prefabs/Bomb2.bprefab", 256);
		CHECK_FALSE(read("Assets/Prefabs/Bomb1.bprefab"));
		CHECK(Contains(error, "expands past"));

		f.WriteNesting("Assets/Prefabs/Bad.bprefab", "../Outside.bprefab", 1);
		CHECK_FALSE(read("Assets/Prefabs/Bad.bprefab"));
		CHECK(Contains(error, "inside the project"));
		f.WriteNesting("Assets/Prefabs/Bad.bprefab", "Assets/Scenes/Main.bscene", 1);
		CHECK_FALSE(read("Assets/Prefabs/Bad.bprefab"));
		CHECK(Contains(error, "must name a .bprefab"));
	}

	TEST_CASE("Malformed nested records are errors")
	{
		NestingFixture f;
		const std::string path = "Assets/Prefabs/Hand.bprefab";
		const auto write = [&](const json& overrides) {
			f.Directory.WriteFile(path, json{ { "Version", 4 }, { "Entities", json::array({ json{ { "ID", 1 }, { "Parent", 0 }, { "Components", { { "Prefab", { { "Prefab", f.Soldier }, { "Overrides", overrides } } } } } } }) } }.dump());
		};
		write(5);
		CHECK(Contains(f.RunError("prefab.instantiate", { { "path", path } }), "'Overrides' must be an object"));
		write(json{ { "1", nullptr } });
		CHECK(Contains(f.RunError("prefab.instantiate", { { "path", path } }), "other than the root"));
		write(json{ { "1", 7 } });
		CHECK(Contains(f.RunError("prefab.instantiate", { { "path", path } }), "must be an object"));
		write(json{ { "1", { { "Components", 5 } } } });
		CHECK(Contains(f.RunError("prefab.instantiate", { { "path", path } }), "'Components' that is not an object"));
		write(json{ { "1", { { "Parent", "up" } } } });
		// The root's parent is the record's, whatever a patch says, and its ID stays.
		CHECK(f.Info(path)["entities"] == 2);
		write(json{ { "1", { { "ID", 99 } } } });
		json expanded;
		std::string error;
		REQUIRE(Prefab::ReadFile(Breccia::Project::ResolvePath(path), expanded, error));
		CHECK(expanded[0]["ID"] == 1);
		// A file that means to nest and does not manage to is an error, never a missing subtree.
		for (const json& link : { json(f.Soldier), json::object(), json{ { "Prefab", "" } }, json{ { "Prefab", nullptr } }, json{ { "Prefab", f.Soldier + ".txt" } } })
		{
			INFO(link.dump());
			f.Directory.WriteFile(path, json{ { "Version", 4 }, { "Entities", json::array({ json{ { "ID", 1 }, { "Parent", 0 }, { "Components", { { "Prefab", link } } } } }) } }.dump());
			f.RunError("prefab.instantiate", { { "path", path } });
		}
		// Files before version 4, or without one, could not nest: what they hold is left over, and only read says so.
		for (const json& version : { json(3), json(nullptr) })
		{
			json older = { { "Entities", json::array({ json{ { "ID", 1 }, { "Name", "Plain" }, { "Parent", 0 }, { "Components", { { "Prefab", { { "Prefab", f.Soldier } } } } } } }) } };
			if (!version.is_null())
				older["Version"] = version;
			f.Directory.WriteFile(path, older.dump());
			CHECK(f.EntityFrom(f.Run("prefab.instantiate", { { "path", path } })).GetChildren().empty());
			REQUIRE(f.Info(path)["warnings"].size() == 1);
			CHECK(Contains(f.Info(path)["warnings"][0].get<std::string>(), "needs 'Version' 4"));
		}
	}

	TEST_CASE("Overrides of entities the nested prefab dropped do nothing, and validation says so")
	{
		NestingFixture f;
		f.Variant(f.Soldier, f.Elite);
		f.Edit(f.Elite, [](Scene& scene) {
			Entity gun = scene.FindEntityByName("Gun");
			gun.GetComponent<PointLightComponent>().Radius = 17;
			Entity sight = scene.CreateEntity("Sight");
			scene.SetParent(sight, gun, false);
		});
		Entity instance = f.Instantiate(f.Elite);
		CHECK(Find(instance, "Sight"));

		// The write that drops it says what it left behind.
		const json dropped = f.Edit(f.Soldier, [](Scene& scene) { scene.DestroyEntity(scene.FindEntityByName("Gun")); });
		CHECK(Contains(dropped["warnings"].dump(), "do nothing"));
		const json info = f.Info(f.Elite);
		// The override, and the parent of the variant's entity.
		REQUIRE(info["warnings"].size() == 2);
		CHECK(Contains(info["warnings"][0].get<std::string>(), "do nothing"));
		CHECK(Contains(info["warnings"][1].get<std::string>(), "the parent of 'Sight' is gone"));
		CHECK(Contains(f.Run("project.validate").dump(), "do nothing"));
		CHECK_FALSE(Find(instance, "Gun"));
		Entity sight = Find(instance, "Sight");
		REQUIRE(sight);
		CHECK(sight.GetParent() == instance);
		// A fresh instance agrees, and saving forgets the dead override.
		Entity fresh = f.Instantiate(f.Elite);
		CHECK(Find(fresh, "Sight").GetParent() == fresh);
		// Everyone involved hears about it before the save forgets it.
		CHECK_FALSE(f.Run("prefab.instantiate", { { "path", f.Elite } })["warnings"].empty());
		const json opened = f.Run("prefab.open", { { "path", f.Elite } });
		CHECK_FALSE(opened["warnings"].empty());
		f.Session.MarkDirty();
		CHECK(Contains(f.Run("prefab.save").dump(), "do nothing"));
		f.Run("prefab.close");
		CHECK(f.Info(f.Elite)["warnings"].empty());
	}

	TEST_CASE("Saved scenes keep nested markers, and outdated outer instances merge when opened")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Squad);
		const uint64_t id = IdOf(instance);
		f.Run("scene.save", { { "path", "Assets/Main.bscene" } });
		const std::string text = f.FileText("Assets/Main.bscene");
		CHECK(ParseJson(text)["Version"] == SceneSerializer::FormatVersion);

		// An edit of the inner prefab while the scene is not open.
		f.Run("scene.new");
		json soldier = ParseJson(f.FileText(f.Soldier));
		for (json& record : soldier["Entities"])
			if (record["Name"] == "Gun")
				record["Components"]["PointLight"]["Radius"] = 80;
		f.Directory.WriteFile(f.Soldier, soldier.dump());
		CHECK(Contains(f.Run("project.validate").dump(), "predate its last save"));

		const json opened = f.Run("scene.open", { { "path", "Assets/Main.bscene" } });
		// The Soldier's tree, Squad's and the instance, each once.
		CHECK(opened["prefabInstances"] == 3);
		Entity loaded = f.Session.GetScene()->GetEntityByUUID(UUID(id));
		REQUIRE(loaded);
		CHECK(GunRadius(loaded) == 80);
		CHECK(Find(loaded, "Soldier").GetComponent<PrefabComponent>().Prefab == f.Soldier);
		CHECK(f.Overrides(loaded).empty());
		f.Run("scene.save");
		CHECK_FALSE(Contains(f.Run("project.validate").dump(), "predate its last save"));
	}

	TEST_CASE("asset.move follows nested and override references through every file")
	{
		NestingFixture f;
		f.Directory.WriteFile("Assets/Script.lua", "return {}");
		f.Edit(f.Squad, [](Scene& scene) { scene.FindEntityByName("Gun").AddComponent<ScriptComponent>().Script = "Assets/Script.lua"; });
		Entity instance = f.Instantiate(f.Squad);
		f.Run("scene.save", { { "path", "Assets/Main.bscene" } });

		// An asset named only inside a nested record's overrides: a UUID, so the file needs no change to follow it.
		CHECK(Contains(f.FileText(f.Squad), "\"UUID\""));
		f.Run("asset.move", { { "from", "Assets/Script.lua" }, { "to", "Assets/Moved.lua" } });
		CHECK(Find(instance, "Gun").GetComponent<ScriptComponent>().Script == "Assets/Moved.lua");
		CHECK(f.Overrides(instance).empty());
		CHECK(Find(f.Instantiate(f.Squad), "Gun").GetComponent<ScriptComponent>().Script == "Assets/Moved.lua");

		// The nested prefab itself: the reference and every baseline's marker move with it.
		const std::string moved = "Assets/Prefabs/Moved/Soldier.bprefab";
		f.Run("asset.move", { { "from", f.Soldier }, { "to", moved } });
		CHECK(f.Info(f.Squad)["dependencies"] == json::array({ moved }));
		CHECK(Find(instance, "Soldier").GetComponent<PrefabComponent>().Prefab == moved);
		CHECK(f.Overrides(instance).empty());
		const json opened = f.Run("scene.open", { { "path", "Assets/Main.bscene" } });
		CHECK_FALSE(opened.contains("prefabInstances"));
		CHECK_FALSE(Contains(f.Run("project.validate").dump(), "Prefabs/"));
	}

	TEST_CASE("asset.move migrates path references that a nested record holds")
	{
		NestingFixture f;
		f.Directory.WriteFile("Assets/Script.lua", "return {}");
		f.Edit(f.Squad, [](Scene& scene) { scene.FindEntityByName("Gun").AddComponent<ScriptComponent>().Script = "Assets/Script.lua"; });
		// As written by hand: plain paths, in the link and in an override.
		json file = ParseJson(f.FileText(f.Squad));
		json& link = file["Entities"][1]["Components"]["Prefab"];
		link["Prefab"] = f.Soldier;
		for (auto& [key, patch] : link["Overrides"].items())
			if (patch.contains("Components") && patch["Components"].contains("Script"))
				patch["Components"]["Script"]["Script"] = "Assets/Script.lua";
		f.Directory.WriteFile(f.Squad, file.dump());

		f.Run("asset.move", { { "from", "Assets/Script.lua" }, { "to", "Assets/Moved.lua" } });
		// A path becomes an identity, which follows the file from then on.
		CHECK(Contains(f.FileText(f.Squad), "\"UUID\""));
		CHECK(Find(f.Instantiate(f.Squad), "Gun").GetComponent<ScriptComponent>().Script == "Assets/Moved.lua");
		const std::string moved = "Assets/Prefabs/Moved/Soldier.bprefab";
		CHECK(f.FileEntities(f.Squad)[1]["Components"]["Prefab"]["Prefab"].is_string());
		f.Run("asset.move", { { "from", f.Soldier }, { "to", moved } });
		CHECK(f.FileEntities(f.Squad)[1]["Components"]["Prefab"]["Prefab"].is_object());
		CHECK(f.Info(f.Squad)["dependencies"] == json::array({ moved }));
	}

	TEST_CASE("Copies of an outer instance track their own nested entities; play instances are flat too")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Squad);
		const json copy = f.Run("entity.duplicate", { { "entity", IdOf(instance) } });
		Entity duplicate = f.EntityFrom(copy);
		CHECK(f.Overrides(duplicate).empty());
		f.Edit(f.Soldier, [](Scene& scene) { GunLight(scene).Radius = 31; });
		CHECK(GunRadius(duplicate) == 31);

		// A copy of a nested root is the scene's own instance now, which the next prefab write adopts.
		const json nested = f.Run("entity.duplicate", { { "entity", IdOf(Find(instance, "Soldier")) } });
		Entity lone = f.EntityFrom(nested);
		CHECK_FALSE(Prefab::EnclosingInstance(lone));
		f.Edit(f.Soldier, [](Scene& scene) { GunLight(scene).Radius = 32; });
		CHECK(Prefab::IsTracked(lone.GetComponent<PrefabComponent>()));
		CHECK(GunRadius(instance) == 32);

		f.Run("play.start");
		const json played = f.Run("prefab.instantiate", { { "path", f.Squad } });
		Entity spawned = f.EntityFrom(played);
		CHECK(GunRadius(spawned) == 32);
		CHECK(spawned.GetComponent<PrefabComponent>().Members.size() == 3);
		CHECK(spawned.GetComponent<PrefabComponent>().Baseline.empty());
		CHECK(Find(spawned, "Soldier").GetComponent<PrefabComponent>().Prefab == f.Soldier);
		f.Run("play.stop");
	}

	TEST_CASE("Baselines accept a nested root's path and nothing more")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Squad);
		json data = ComponentRegistry::Find("Prefab")->Serialize(instance);
		std::string nestedKey;
		for (const auto& [key, record] : data["Baseline"].items())
			if (record["Components"].contains("Prefab"))
				nestedKey = key;
		REQUIRE_FALSE(nestedKey.empty());
		json extended = data;
		extended["Baseline"][nestedKey]["Components"]["Prefab"]["Members"] = json::object();
		std::string error;
		CHECK_FALSE(ComponentRegistry::AddOrPatch(instance, "Prefab", extended, error));
		CHECK(Contains(error, "with the path alone"));
		CHECK(ComponentRegistry::AddOrPatch(instance, "Prefab", data, error));
	}

	TEST_CASE("Saving merges what nested instances missed instead of pinning old values as overrides")
	{
		NestingFixture f;
		f.Run("prefab.open", { { "path", f.Squad } });
		const auto setRadius = [](float radius) {
			return [=](json& record) {
				if (record["Name"] == "Gun")
					record["Components"]["PointLight"]["Radius"] = radius;
			};
		};
		// No refresh and no watcher: the session has not heard of this edit.
		f.WriteRecords(f.Soldier, setRadius(42));
		f.Session.MarkDirty();
		f.Run("prefab.save");
		f.Run("prefab.close");
		CHECK_FALSE(Contains(f.FileText(f.Squad), "Radius"));
		CHECK(GunRadius(f.Instantiate(f.Squad)) == 42);

		// The same for a tree in a scene, and for a tree over its own nesting prefab.
		Entity holder = f.Session.GetScene()->CreateEntity("Holder");
		Entity nested = f.InstantiateUnder(f.Soldier, holder);
		f.WriteRecords(f.Soldier, setRadius(43));
		f.Run("prefab.create", { { "entity", IdOf(holder) }, { "path", "Assets/Prefabs/Holder.bprefab" } });
		CHECK_FALSE(Contains(f.FileText("Assets/Prefabs/Holder.bprefab"), "Radius"));
		CHECK(GunRadius(nested) == 43);
		f.WriteRecords(f.Soldier, setRadius(44));
		f.Run("prefab.create", { { "entity", IdOf(holder) }, { "path", "Assets/Prefabs/Holder.bprefab" } });
		CHECK_FALSE(Contains(f.FileText("Assets/Prefabs/Holder.bprefab"), "Radius"));
		CHECK(GunRadius(holder) == 44);
	}

	TEST_CASE("Saving an instance over its variant refuses when the file cannot be read")
	{
		NestingFixture f;
		f.Variant(f.Soldier, f.Elite);
		Entity instance = f.Instantiate(f.Elite);
		const std::string variant = f.FileText(f.Elite);
		f.Directory.WriteFile(f.Soldier, "not a prefab");
		CHECK(Contains(f.RunError("prefab.create", { { "entity", IdOf(instance) }, { "path", f.Elite } }), "not a valid prefab"));
		CHECK(f.FileText(f.Elite) == variant);

		// An instance without tracking takes the same care: it cannot tell what the file was either.
		Entity untracked = f.Session.GetScene()->CreateEntity("Untracked");
		untracked.AddComponent<PrefabComponent>().Prefab = f.Elite;
		CHECK(Contains(f.RunError("prefab.create", { { "entity", IdOf(untracked) }, { "path", f.Elite } }), "cannot be read"));
		CHECK(f.FileText(f.Elite) == variant);
	}

	TEST_CASE("A record the prefab kept wins over the nested prefab's when that entity comes back")
	{
		NestingFixture f;
		const std::string soldierFile = f.FileText(f.Soldier);
		f.Run("prefab.open", { { "path", f.Squad } });
		GunLight(*f.Session.GetEditScene()).Radius = 21;
		f.Session.MarkDirty();
		// The nested prefab drops the Gun; the open prefab's edit keeps it, as an instance's edit does.
		json dropped = ParseJson(soldierFile);
		dropped["Entities"].erase(1);
		f.Run("asset.write", { { "path", f.Soldier }, { "content", dropped.dump() } });
		f.Run("prefab.save");
		f.Run("prefab.close");
		CHECK(f.FileEntities(f.Squad).size() == 3);

		f.Directory.WriteFile(f.Soldier, soldierFile);
		const json info = f.Info(f.Squad);
		CHECK(info["entities"] == 3);
		REQUIRE(info["warnings"].size() == 1);
		CHECK(Contains(info["warnings"][0].get<std::string>(), "replaces the one in"));
		CHECK(GunRadius(f.Instantiate(f.Squad)) == 21);
		f.Edit(f.Squad, [](Scene&) {});
		CHECK(f.FileEntities(f.Squad).size() == 2);
		CHECK(f.Info(f.Squad)["warnings"].empty());
	}

	TEST_CASE("Unpacking tracks nested instances by what the instance knows, not by their names")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Squad);
		Entity gun = Find(instance, "Gun");
		gun.GetComponent<TagComponent>().Tag = "Rifle";
		f.Session.MarkDirty();
		const std::string path = "Assets/Prefabs/Copy.bprefab";
		// Saved elsewhere from the instance, the renamed entity stays part of its nested instance.
		f.Run("prefab.create", { { "entity", IdOf(instance) }, { "path", path } });
		CHECK(f.FileEntities(path).size() == 2);
		f.Run("prefab.unpack", { { "entity", IdOf(instance) } });
		Entity soldier = Find(instance, "Soldier");
		const json overrides = f.Overrides(soldier);
		REQUIRE(overrides.size() == 1);
		CHECK(Contains(overrides.begin().key(), "/Name"));
		f.Edit(f.Soldier, [](Scene& edit) { GunLight(edit).Radius = 93; });
		CHECK(Radius(gun) == 93);
		CHECK(Radius(Find(f.Instantiate(path), "Rifle")) == 93);
	}

	TEST_CASE("Instances added under a nested entity are found: tracked in an open prefab, and on unpacking")
	{
		NestingFixture f;
		Entity holder = f.Session.GetScene()->CreateEntity("Holder");
		Entity nested = f.InstantiateUnder(f.Squad, holder);
		Entity other = f.EntityFrom(f.Run("prefab.instantiate", { { "path", f.Soldier }, { "parent", IdOf(Find(nested, "Gun")) } }));
		other.GetComponent<TagComponent>().Tag = "Other";
		const std::string path = "Assets/Prefabs/Holder.bprefab";
		f.Run("prefab.create", { { "entity", IdOf(holder) }, { "path", path } });

		f.Run("prefab.open", { { "path", path } });
		Entity opened = f.Session.GetEditScene()->FindEntityByName("Other");
		REQUIRE(opened);
		CHECK(Prefab::IsTracked(opened.GetComponent<PrefabComponent>()));
		f.Run("prefab.close");

		Entity instance = f.Instantiate(path);
		Entity added = Find(instance, "Other");
		f.Run("prefab.unpack", { { "entity", IdOf(instance) } });
		CHECK(Prefab::IsTracked(added.GetComponent<PrefabComponent>()));
		CHECK(Contains(f.Overrides(added).dump(), "/Name"));
	}

	TEST_CASE("Unpacking a nested root keeps the IDs of everything below it, wherever that is done")
	{
		NestingFixture f;
		Entity army = f.Session.GetScene()->CreateEntity("Army");
		f.InstantiateUnder(f.Squad, army);
		const std::string path = "Assets/Prefabs/Army.bprefab";
		f.Run("prefab.create", { { "entity", IdOf(army) }, { "path", path } });

		// In an instance, and applied: the Soldier inside becomes a nested instance of Army, and still follows its prefab.
		Entity instance = f.Instantiate(path);
		Entity gun = Find(instance, "Gun");
		const uint64_t gunId = IdOf(gun);
		gun.GetComponent<PointLightComponent>().Intensity = 66;
		f.Run("prefab.unpack", { { "entity", IdOf(Find(instance, "Squad")) } });
		f.Run("prefab.apply", { { "entity", IdOf(instance) } });
		json info = f.Info(path);
		REQUIRE(info["nested"].size() == 1);
		CHECK(info["nested"][0]["prefab"] == f.Soldier);
		CHECK(f.Overrides(instance).empty());
		CHECK(IdOf(Find(instance, "Gun")) == gunId);

		// In the open prefab: an instance with an override of its own keeps one Gun, the same one, and the override.
		Entity holder = f.Session.GetScene()->CreateEntity("Holder");
		f.InstantiateUnder(f.Squad, holder);
		const std::string holderPath = "Assets/Prefabs/Holder.bprefab";
		f.Run("prefab.create", { { "entity", IdOf(holder) }, { "path", holderPath } });
		Entity other = f.Instantiate(holderPath);
		Entity otherGun = Find(other, "Gun");
		const uint64_t otherId = IdOf(otherGun);
		otherGun.GetComponent<PointLightComponent>().Intensity = 77;
		f.Session.MarkDirty();
		f.Run("prefab.open", { { "path", holderPath } });
		f.Run("prefab.unpack", { { "entity", "Squad" } });
		const json saved = f.Run("prefab.save");
		f.Run("prefab.close");
		// The instance and the authoring tree.
		CHECK(saved["instances"] == 2);
		CHECK(IdOf(Find(other, "Gun")) == otherId);
		CHECK(Find(other, "Gun").GetComponent<PointLightComponent>().Intensity == 77);
		CHECK(f.Overrides(other).size() == 1);
		size_t guns = 0;
		for (Entity child : Find(other, "Soldier").GetChildren())
			guns += child.GetName() == "Gun" ? 1 : 0;
		CHECK(guns == 1);
		f.Edit(f.Soldier, [](Scene& edit) { GunLight(edit).Radius = 94; });
		CHECK(GunRadius(other) == 94);
	}

	TEST_CASE("A chain too deep to read is refused before it is written")
	{
		NestingFixture f;
		std::string previous = f.Soldier;
		// Soldier is 1 deep, so each new prefab stays within the limit up to here.
		for (size_t level = 1; level + 1 < Prefab::MaxNestingDepth; ++level)
		{
			const std::string path = "Assets/Prefabs/Chain" + std::to_string(level) + ".bprefab";
			f.WriteNesting(path, previous, 1);
			previous = path;
		}
		Entity holder = f.Session.GetScene()->CreateEntity("Holder");
		f.InstantiateUnder(previous, holder);
		const std::string path = "Assets/Prefabs/TooDeep.bprefab";
		f.Run("prefab.create", { { "entity", IdOf(holder) }, { "path", path } });
		Entity outer = f.Session.GetScene()->CreateEntity("Outer");
		f.InstantiateUnder(path, outer);
		CHECK(Contains(f.RunError("prefab.create", { { "entity", IdOf(outer) }, { "path", "Assets/Prefabs/Deeper.bprefab" } }), "deeper than"));
		CHECK_FALSE(FileSystem::Exists(Breccia::Project::ResolvePath("Assets/Prefabs/Deeper.bprefab")));
	}

	TEST_CASE("asset.move keeps leftovers in old prefabs dead when it moves them to the current version")
	{
		NestingFixture f;
		f.Directory.WriteFile("Assets/Script.lua", "return {}");
		const std::string path = "Assets/Prefabs/Old.bprefab";
		const json root = { { "ID", 1 }, { "Name", "Old" }, { "Parent", 0 }, { "Components", { { "Script", { { "Script", "Assets/Script.lua" } } }, { "Prefab", { { "Prefab", f.Soldier } } } } } };
		f.Directory.WriteFile(path, json{ { "Format", "BrecciaPrefab" }, { "Version", 3 }, { "Entities", json::array({ root }) } }.dump());
		const json moved = f.Run("asset.move", { { "from", "Assets/Script.lua" }, { "to", "Assets/Moved.lua" } });
		CHECK(Contains(moved["warnings"].dump(), "removed a Prefab component from 'Old'"));
		CHECK(ParseJson(f.FileText(path))["Version"] == SceneSerializer::FormatVersion);
		CHECK(f.Info(path)["entities"] == 1);
		CHECK(f.Info(path)["base"].is_null());
	}

	TEST_CASE("A differently spelled path is the same prefab on a case-insensitive disk")
	{
		NestingFixture f;
		const std::string shouted = "Assets/Prefabs/SOLDIER.bprefab";
		CHECK(Prefab::FileKeyOf("Assets/Prefabs/./Soldier.bprefab") == Prefab::FileKeyOf(f.Soldier));
		if (!FileSystem::Exists(Breccia::Project::ResolvePath(shouted)))
			return;
		const std::string file = f.FileText(f.Soldier);
		f.Run("prefab.open", { { "path", f.Soldier } });
		f.Run("prefab.instantiate", { { "path", shouted }, { "parent", "Soldier" } });
		f.Session.MarkDirty();
		CHECK(Contains(f.RunError("prefab.save"), "holds that prefab itself"));
		CHECK(f.FileText(f.Soldier) == file);
		f.Run("prefab.close", { { "discard", true } });

		// A link spelled differently still hears of the file it names.
		json squad = ParseJson(f.FileText(f.Squad));
		squad["Entities"][1]["Components"]["Prefab"]["Prefab"] = shouted;
		f.Run("asset.write", { { "path", f.Squad }, { "content", squad.dump() } });
		Entity instance = f.Instantiate(f.Squad);
		f.Edit(f.Soldier, [](Scene& edit) { GunLight(edit).Radius = 95; });
		CHECK(GunRadius(instance) == 95);
	}

	TEST_CASE("Copies of nested roots and nested roots moved out come back to the instance around them")
	{
		NestingFixture f;
		Entity army = f.Session.GetScene()->CreateEntity("Army");
		f.InstantiateUnder(f.Squad, army);
		const std::string path = "Assets/Prefabs/Army.bprefab";
		f.Run("prefab.create", { { "entity", IdOf(army) }, { "path", path } });
		Entity instance = f.Instantiate(path);
		Entity bystander = f.Instantiate(f.Squad);
		Entity copy = f.EntityFrom(f.Run("entity.duplicate", { { "entity", IdOf(Find(instance, "Squad")) } }));
		// The copy's inner Soldier is adopted first, on its own, and then the copy around it.
		f.Edit(f.Soldier, [](Scene& edit) { GunLight(edit).Radius = 31; });
		f.Edit(f.Squad, [](Scene& edit) { GunLight(edit).Intensity = 44; });
		const json overrides = f.Overrides(copy);
		for (const auto& [key, value] : overrides.items())
			CHECK_FALSE(value.is_null());
		f.Run("prefab.apply", { { "entity", IdOf(copy) } });
		CHECK(Find(bystander, "Soldier"));
		CHECK(Find(bystander, "Gun"));

		// Out of its instance a nested root takes its prefab's edits itself; back in, it is a member again.
		Entity soldier = Find(instance, "Soldier");
		Entity parent = soldier.GetParent();
		auto scene = f.Session.GetScene();
		scene->SetParent(soldier, Entity{}, false);
		f.Edit(f.Soldier, [](Scene& edit) { GunLight(edit).Radius = 32; });
		CHECK(Prefab::IsTracked(soldier.GetComponent<PrefabComponent>()));
		scene->SetParent(soldier, parent, false);
		f.Session.MarkDirty();
		CHECK(Prefab::EnclosingInstance(soldier) == instance);
		f.Run("prefab.apply", { { "entity", IdOf(instance) } });
		CHECK_FALSE(Prefab::ListsMembers(soldier.GetComponent<PrefabComponent>()));
		CHECK(Find(f.Instantiate(path), "Soldier"));
	}

	TEST_CASE("A nested record's own fields do nothing, and reading says so")
	{
		NestingFixture f;
		const std::string path = "Assets/Prefabs/Hand.bprefab";
		f.Directory.WriteFile(path, json{ { "Version", 4 }, { "Entities", json::array({ json{ { "ID", 1 }, { "Name", "Mine" }, { "Parent", 0 }, { "Components", { { "PointLight", { { "Radius", 99 } } }, { "Prefab", { { "Prefab", f.Soldier }, { "Members", json::object() } } } } } } }) } }.dump());
		const json warnings = f.Info(path)["warnings"];
		REQUIRE(warnings.size() == 1);
		for (const char* field : { "Name", "PointLight", "Prefab.Members" })
			CHECK(Contains(warnings[0].get<std::string>(), field));
		CHECK(f.Instantiate(path).GetName() == "Soldier");
	}

	TEST_CASE("A merge moves an instance's additions to the nearest entity left, and says so")
	{
		NestingFixture f;
		f.Edit(f.Soldier, [](Scene& edit) {
			Entity sling = edit.CreateEntity("Sling");
			edit.SetParent(sling, edit.FindEntityByName("Gun"), false);
		});
		Entity instance = f.Instantiate(f.Squad);
		auto scene = f.Session.GetScene();
		Entity local = scene->CreateEntity("Local");
		scene->SetParent(local, Find(instance, "Sling"), false);
		json soldier = ParseJson(f.FileText(f.Soldier));
		soldier["Entities"].erase(2);
		const json written = f.Run("asset.write", { { "path", f.Soldier }, { "content", soldier.dump() } });
		CHECK(local.GetParent() == Find(instance, "Gun"));
		CHECK(Contains(written.dump(), "'Local' moved to 'Gun'"));
	}

	TEST_CASE("prefab.create with a variant stays unlinked and says so while a prefab is open")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Soldier);
		f.Run("play.start");
		const json result = f.Run("prefab.create", { { "entity", IdOf(instance) }, { "path", f.Elite }, { "variant", true } });
		f.Run("play.stop");
		CHECK(result["linked"] == false);
		CHECK(Contains(result["warnings"].dump(), "stays an instance of"));
		CHECK(instance.GetComponent<PrefabComponent>().Prefab == f.Soldier);
	}

	TEST_CASE("prefab.variant picks a free name beside its base")
	{
		NestingFixture f;
		CHECK(f.Run("prefab.variant", { { "base", f.Soldier } })["path"] == "Assets/Prefabs/Soldier Variant.bprefab");
		CHECK(f.Run("prefab.variant", { { "base", f.Soldier } })["path"] == "Assets/Prefabs/Soldier Variant 2.bprefab");
	}

	TEST_CASE("Variants and nesting combine: a variant of a nesting prefab, and a variant nested in a holder")
	{
		NestingFixture f;
		f.Variant(f.Squad, f.Elite);
		f.Edit(f.Elite, [](Scene& edit) { GunLight(edit).Radius = 15; });
		CHECK(f.FileEntities(f.Elite).size() == 1);
		Entity instance = f.Instantiate(f.Elite);
		Entity soldier = Find(instance, "Soldier");
		CHECK(soldier.GetComponent<PrefabComponent>().Prefab == f.Soldier);
		CHECK(Prefab::EnclosingInstance(soldier) == instance);
		f.Edit(f.Soldier, [](Scene& edit) { GunLight(edit).Intensity = 16; });
		f.Edit(f.Squad, [](Scene& edit) { edit.FindEntityByName("Squad").AddComponent<PointLightComponent>().Radius = 17; });
		CHECK(Find(instance, "Gun").GetComponent<PointLightComponent>().Intensity == 16);
		CHECK(GunRadius(instance) == 15);
		CHECK(Radius(instance) == 17);
		CHECK(f.Overrides(instance).empty());

		Entity holder = f.Session.GetScene()->CreateEntity("Holder");
		Entity nested = f.InstantiateUnder(f.Elite, holder);
		const std::string path = "Assets/Prefabs/Holder.bprefab";
		f.Run("prefab.create", { { "entity", IdOf(holder) }, { "path", path } });
		CHECK(nested.GetComponent<PrefabComponent>().Prefab == f.Elite);
		CHECK(f.Info(path)["dependencies"].size() == 3);
		Entity outer = f.Instantiate(path);
		Find(outer, "Gun").GetComponent<PointLightComponent>().Radius = 18;
		f.Session.MarkDirty();
		const std::string variant = f.FileText(f.Elite);
		f.Run("prefab.apply", { { "entity", IdOf(outer) } });
		CHECK(f.FileText(f.Elite) == variant);
		CHECK(GunRadius(f.Instantiate(path)) == 18);
		CHECK(GunRadius(instance) == 15);
	}

	TEST_CASE("Three levels override one field: the outermost wins, and each file keeps its own value")
	{
		NestingFixture f;
		f.Edit(f.Squad, [](Scene& edit) { GunLight(edit).Radius = 12; });
		Entity army = f.Session.GetScene()->CreateEntity("Army");
		Entity squad = f.InstantiateUnder(f.Squad, army);
		CHECK(GunRadius(squad) == 12);
		Find(squad, "Gun").GetComponent<PointLightComponent>().Radius = 13;
		const std::string path = "Assets/Prefabs/Army.bprefab";
		f.Run("prefab.create", { { "entity", IdOf(army) }, { "path", path } });
		Entity instance = f.Instantiate(path);
		CHECK(GunRadius(instance) == 13);
		Find(instance, "Gun").GetComponent<PointLightComponent>().Radius = 14;
		f.Session.MarkDirty();
		const std::string squadFile = f.FileText(f.Squad);
		f.Run("prefab.apply", { { "entity", IdOf(instance) } });
		CHECK(f.FileText(f.Squad) == squadFile);
		CHECK(GunRadius(f.Instantiate(path)) == 14);
		const json entities = f.FileEntities(path);
		f.Edit(path, [](Scene&) {});
		CHECK(f.FileEntities(path) == entities);
	}

	TEST_CASE("References cross nesting levels, and survive edits and a level more")
	{
		NestingFixture f;
		f.Edit(f.Soldier, [](Scene& edit) {
			Entity self = edit.CreateEntity("Self");
			edit.SetParent(self, edit.FindEntityByName("Gun"), false);
			self.AddComponent<BoneAttachmentComponent>().Target = edit.FindEntityByName("Soldier").GetUUID();
		});
		auto scene = f.Session.GetScene();
		Entity pair = scene->CreateEntity("Pair");
		Entity left = f.InstantiateUnder(f.Soldier, pair);
		Entity right = f.InstantiateUnder(f.Soldier, pair);
		left.GetComponent<TagComponent>().Tag = "L";
		right.GetComponent<TagComponent>().Tag = "R";
		// One nested instance's entity aims at the other's, and the other's at the holder's own.
		Find(left, "Self").GetComponent<BoneAttachmentComponent>().Target = Find(right, "Gun").GetUUID();
		Find(right, "Self").GetComponent<BoneAttachmentComponent>().Target = pair.GetUUID();
		const std::string path = "Assets/Prefabs/Pair.bprefab";
		f.Run("prefab.create", { { "entity", IdOf(pair) }, { "path", path } });
		const auto check = [&](Entity instance) {
			CHECK(Find(Find(instance, "L"), "Self").GetComponent<BoneAttachmentComponent>().Target == Find(Find(instance, "R"), "Gun").GetUUID());
			CHECK(Find(Find(instance, "R"), "Self").GetComponent<BoneAttachmentComponent>().Target == instance.GetUUID());
		};
		Entity instance = f.Instantiate(path);
		check(instance);
		CHECK(f.Overrides(instance).empty());
		f.Edit(f.Soldier, [](Scene& edit) { GunLight(edit).Radius = 33; });
		check(instance);
		// A variant of it: the same references, renumbered.
		f.Variant(path, f.Elite);
		check(f.Instantiate(f.Elite));
		// An unedited nested root aims at its own root.
		Entity spare = f.Instantiate(f.Squad);
		f.Edit(f.Squad, [](Scene&) {});
		CHECK(Find(spare, "Self").GetComponent<BoneAttachmentComponent>().Target == Find(spare, "Soldier").GetUUID());
	}

	TEST_CASE("An open prefab can hold a new instance of a variant of a nesting prefab, across saves")
	{
		NestingFixture f;
		f.Variant(f.Squad, f.Elite);
		const std::string path = "Assets/Prefabs/Holder.bprefab";
		Entity holder = f.Session.GetScene()->CreateEntity("Holder");
		f.Run("prefab.create", { { "entity", IdOf(holder) }, { "path", path } });
		f.Run("prefab.open", { { "path", path } });
		Scene& edit = *f.Session.GetEditScene();
		Entity added = f.EntityFrom(f.Run("prefab.instantiate", { { "path", f.Elite }, { "parent", "Holder" } }));
		Entity pointer = edit.CreateEntity("Pointer");
		edit.SetParent(pointer, edit.FindEntityByName("Holder"), false);
		pointer.AddComponent<BoneAttachmentComponent>().Target = Find(added, "Gun").GetUUID();
		f.Session.MarkDirty();
		f.Run("prefab.save");
		const json first = f.FileEntities(path);
		f.Session.MarkDirty();
		f.Run("prefab.save");
		CHECK(f.FileEntities(path) == first);
		f.Run("prefab.close");
		Entity instance = f.Instantiate(path);
		CHECK(Find(instance, "Pointer").GetComponent<BoneAttachmentComponent>().Target == Find(instance, "Gun").GetUUID());
		CHECK(f.Overrides(instance).empty());
	}

	TEST_CASE("Reverting one nested override, or a deleted nested root, restores it with its IDs")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Squad);
		Entity gun = Find(instance, "Gun");
		gun.GetComponent<PointLightComponent>().Radius = 19;
		gun.GetComponent<PointLightComponent>().Intensity = 20;
		const json overrides = f.Overrides(instance);
		REQUIRE(overrides.size() == 2);
		for (const auto& [field, value] : overrides.items())
			if (Contains(field, "Radius"))
				f.Run("prefab.revert", { { "entity", IdOf(instance) }, { "field", field } });
		CHECK(Radius(gun) == 7);
		CHECK(gun.GetComponent<PointLightComponent>().Intensity == 20);

		Entity soldier = Find(instance, "Soldier");
		const uint64_t soldierId = IdOf(soldier);
		const uint64_t gunId = IdOf(gun);
		f.Session.GetScene()->DestroyEntity(soldier);
		f.Session.MarkDirty();
		CHECK_FALSE(Find(instance, "Gun"));
		f.Run("prefab.revert", { { "entity", IdOf(instance) } });
		Entity restored = Find(instance, "Soldier");
		REQUIRE(restored);
		CHECK(IdOf(restored) == soldierId);
		CHECK(IdOf(Find(instance, "Gun")) == gunId);
		CHECK(restored.GetComponent<PrefabComponent>().Prefab == f.Soldier);
		CHECK(Prefab::EnclosingInstance(restored) == instance);
	}

	TEST_CASE("A nested entity moved out of its nested tree stays moved and takes its prefab's edits")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Squad);
		f.Session.GetScene()->SetParent(Find(instance, "Gun"), instance, false);
		f.Session.MarkDirty();
		f.Run("prefab.apply", { { "entity", IdOf(instance) } });
		CHECK(f.Overrides(instance).empty());
		Entity fresh = f.Instantiate(f.Squad);
		CHECK(Find(fresh, "Gun").GetParent() == fresh);
		f.Edit(f.Soldier, [](Scene& edit) { GunLight(edit).Radius = 34; });
		CHECK(GunRadius(fresh) == 34);
		CHECK(Find(fresh, "Gun").GetParent() == fresh);
	}

	TEST_CASE("An existing instance gains a nested instance as a marker that takes inner edits")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Squad);
		f.Edit(f.Squad, [&](Scene& edit) {
			Entity second = f.EntityFrom(f.Run("prefab.instantiate", { { "path", f.Soldier }, { "parent", "Squad" } }));
			second.GetComponent<TagComponent>().Tag = "Second";
			(void)edit;
		});
		Entity second = Find(instance, "Second");
		REQUIRE(second);
		CHECK(second.GetComponent<PrefabComponent>().Prefab == f.Soldier);
		CHECK_FALSE(Prefab::ListsMembers(second.GetComponent<PrefabComponent>()));
		CHECK(Prefab::EnclosingInstance(second) == instance);
		f.Edit(f.Soldier, [](Scene& edit) { GunLight(edit).Radius = 35; });
		CHECK(GunRadius(second) == 35);
	}

	TEST_CASE("prefab.create keeps a nested instance's deletions and removed components as overrides")
	{
		NestingFixture f;
		Entity holder = f.Session.GetScene()->CreateEntity("Holder");
		Entity nested = f.InstantiateUnder(f.Soldier, holder);
		nested.RemoveComponent<PointLightComponent>();
		f.Session.GetScene()->DestroyEntity(Find(nested, "Gun"));
		const std::string path = "Assets/Prefabs/Holder.bprefab";
		f.Run("prefab.create", { { "entity", IdOf(holder) }, { "path", path } });
		CHECK(f.FileEntities(path).size() == 2);
		f.Edit(f.Soldier, [](Scene& edit) {
			edit.FindEntityByName("Soldier").GetComponent<PointLightComponent>().Radius = 36;
			GunLight(edit).Radius = 37;
		});
		Entity instance = f.Instantiate(path);
		CHECK_FALSE(Find(instance, "Gun"));
		CHECK_FALSE(Find(instance, "Soldier").HasComponent<PointLightComponent>());
		f.Run("prefab.open", { { "path", path } });
		f.Run("prefab.revert", { { "entity", "Soldier" } });
		f.Run("prefab.save");
		f.Run("prefab.close");
		CHECK(GunRadius(f.Instantiate(path)) == 37);
	}

	TEST_CASE("The scene waiting behind an open prefab takes inner edits, keeping its overrides")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Squad);
		Find(instance, "Gun").GetComponent<PointLightComponent>().Intensity = 38;
		f.Session.ClearDirty();
		f.Run("prefab.open", { { "path", f.Squad } });
		f.WriteRecords(f.Soldier, [](json& record) {
			if (record["Name"] == "Gun")
				record["Components"]["PointLight"]["Radius"] = 39;
		});
		f.Session.ReloadAssets({ { f.Soldier, FileChangeType::Modified } });
		CHECK(GunRadius(instance) == 39);
		CHECK(Find(instance, "Gun").GetComponent<PointLightComponent>().Intensity == 38);
		CHECK(f.Session.IsWaitingSceneDirty());
		f.Run("prefab.close", { { "discard", true } });
	}

	TEST_CASE("Failures around nested prefabs are reported, and write nothing")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Squad);
		std::filesystem::remove(Breccia::Project::ResolvePath(f.Soldier));
		const json unpacked = f.Run("prefab.unpack", { { "entity", IdOf(instance) } });
		CHECK(Contains(unpacked["warnings"].dump(), "stays untracked"));

		const std::string path = "Assets/Prefabs/New.bprefab";
		Entity holder = f.Session.GetScene()->CreateEntity("Holder");
		f.Session.GetScene()->SetParent(instance, holder, false);
		CHECK(Contains(f.RunError("prefab.create", { { "entity", IdOf(holder) }, { "path", path } }), "Soldier"));
		CHECK_FALSE(FileSystem::Exists(Breccia::Project::ResolvePath(path)));
		CHECK_FALSE(holder.HasComponent<PrefabComponent>());
	}

	TEST_CASE("Known limits: sibling order inside a nested tree, and additions under a deleted nested entity")
	{
		NestingFixture f;
		// A nested tree keeps its prefab's sibling order: the file holds no order for it.
		f.Edit(f.Soldier, [](Scene& edit) {
			Entity knife = edit.CreateEntity("Knife");
			edit.SetParent(knife, edit.FindEntityByName("Soldier"), false);
		});
		f.Edit(f.Squad, [](Scene& edit) {
			Entity soldier = edit.FindEntityByName("Soldier");
			edit.SetParent(edit.FindEntityByName("Gun"), soldier, false);
		});
		Entity instance = f.Instantiate(f.Squad);
		CHECK(Find(instance, "Soldier").GetChildren().front().GetName() == "Gun");

		// The prefab deleted the Gun; what its prefab adds under it has nowhere to be but the root.
		f.Edit(f.Squad, [](Scene& edit) { edit.DestroyEntity(edit.FindEntityByName("Gun")); });
		f.Edit(f.Soldier, [](Scene& edit) {
			Entity scope = edit.CreateEntity("Scope");
			edit.SetParent(scope, edit.FindEntityByName("Gun"), false);
		});
		CHECK(Contains(f.Info(f.Squad)["warnings"].dump(), "the parent of 'Scope' is gone"));
		CHECK(Find(f.Instantiate(f.Squad), "Scope").GetParent().GetName() == "Squad");
	}

	TEST_CASE("Links stay inside the project however they are spelled")
	{
		NestingFixture f;
		const std::string path = "Assets/Prefabs/Bad.bprefab";
		for (const char* link : { "Assets/../../Outside.bprefab", "/etc/Outside.bprefab", "../Outside.bprefab" })
		{
			INFO(link);
			f.WriteNesting(path, link, 1);
			CHECK(Contains(f.RunError("prefab.info", { { "path", path } }), "inside the project"));
		}
	}

	TEST_CASE("Entities a stale nested instance gains or loses when saved stay linked to the right records")
	{
		NestingFixture f;
		Entity holder = f.Session.GetScene()->CreateEntity("Holder");
		Entity nested = f.InstantiateUnder(f.Soldier, holder);
		json soldier = ParseJson(f.FileText(f.Soldier));
		json added = soldier["Entities"][1];
		added["ID"] = 777;
		added["Name"] = "Added";
		soldier["Entities"].push_back(added);
		f.Directory.WriteFile(f.Soldier, soldier.dump());
		const std::string path = "Assets/Prefabs/Holder.bprefab";
		const json first = f.Run("prefab.create", { { "entity", IdOf(holder) }, { "path", path } });
		CHECK(first["linked"] == true);
		CHECK(Find(nested, "Added"));
		CHECK(holder.GetComponent<PrefabComponent>().Members.size() == 4);
		CHECK(f.Overrides(holder).empty());

		soldier["Entities"].erase(1);
		soldier["Entities"].erase(1);
		f.Directory.WriteFile(f.Soldier, soldier.dump());
		f.Run("prefab.create", { { "entity", IdOf(holder) }, { "path", path } });
		CHECK_FALSE(Find(nested, "Gun"));
		CHECK(holder.GetComponent<PrefabComponent>().Members.size() >= 2);
		CHECK(f.Overrides(holder).empty());
	}

	TEST_CASE("Saving elsewhere catches up what an instance missed, also through the instance around a subtree")
	{
		NestingFixture f;
		const auto setRadius = [](float radius) {
			return [=](json& record) {
				if (record["Name"] == "Gun")
					record["Components"]["PointLight"]["Radius"] = radius;
			};
		};
		Entity instance = f.Instantiate(f.Squad);
		f.WriteRecords(f.Soldier, setRadius(42));
		f.Run("prefab.create", { { "entity", IdOf(instance) }, { "path", "Assets/Prefabs/Copy.bprefab" } });
		CHECK_FALSE(Contains(f.FileText("Assets/Prefabs/Copy.bprefab"), "Radius"));

		f.WriteRecords(f.Soldier, setRadius(43));
		f.Run("prefab.create", { { "entity", IdOf(Find(instance, "Soldier")) }, { "path", "Assets/Prefabs/Sub.bprefab" } });
		// A plain copy of the subtree, so it holds its values: the current ones.
		CHECK(GunRadius(f.Instantiate("Assets/Prefabs/Sub.bprefab")) == 43);
		CHECK(GunRadius(instance) == 43);

		// A running game cannot merge, and says what that means.
		Entity holder = f.Session.GetScene()->CreateEntity("Holder");
		f.InstantiateUnder(f.Soldier, holder);
		f.Run("play.start");
		f.WriteRecords(f.Soldier, setRadius(44));
		const json played = f.Run("prefab.create", { { "entity", "Holder" }, { "path", "Assets/Prefabs/Played.bprefab" } });
		f.Run("play.stop");
		CHECK(Contains(played["warnings"].dump(), "while the game runs"));
	}

	TEST_CASE("A save that cannot read one nested prefab merges nothing; one that cannot merge says so")
	{
		NestingFixture f;
		f.Run("prefab.create", { { "entity", IdOf(f.SoldierTree) }, { "path", "Assets/Prefabs/Other.bprefab" } });
		Entity holder = f.Session.GetScene()->CreateEntity("Holder");
		Entity first = f.InstantiateUnder(f.Soldier, holder);
		f.InstantiateUnder("Assets/Prefabs/Other.bprefab", holder);
		const std::string other = f.FileText("Assets/Prefabs/Other.bprefab");
		f.WriteRecords(f.Soldier, [](json& record) {
			if (record["Name"] == "Gun")
				record["Components"]["PointLight"]["Radius"] = 42;
		});
		f.Directory.WriteFile("Assets/Prefabs/Other.bprefab", "not a prefab");
		const std::string path = "Assets/Prefabs/Holder.bprefab";
		CHECK(Contains(f.RunError("prefab.create", { { "entity", IdOf(holder) }, { "path", path } }), "not a valid prefab"));
		CHECK(GunRadius(first) == 7);
		CHECK_FALSE(FileSystem::Exists(Breccia::Project::ResolvePath(path)));
		f.Directory.WriteFile("Assets/Prefabs/Other.bprefab", other);

		// When one nested instance merges and the next cannot, what merged is an edit of the open prefab like any other.
		f.Session.GetScene()->DestroyEntity(holder);
		f.WriteRecords(f.Soldier, [](json&) {});
		Entity pair = f.Session.GetScene()->CreateEntity("Pair");
		f.InstantiateUnder(f.Soldier, pair);
		f.InstantiateUnder("Assets/Prefabs/Other.bprefab", pair);
		f.Run("prefab.create", { { "entity", IdOf(pair) }, { "path", path } });
		const std::string file = f.FileText(path);
		f.Run("prefab.open", { { "path", path } });
		f.Session.ClearDirty();
		f.WriteRecords(f.Soldier, [](json& record) {
			if (record["Name"] == "Gun")
				record["Components"]["PointLight"]["Radius"] = 43;
		});
		f.WriteRecords("Assets/Prefabs/Other.bprefab", [](json& record) {
			if (record["Parent"] == 0)
				record["ID"] = 555;
			else
				record["Parent"] = 555;
		});
		CHECK(Contains(f.RunError("prefab.save"), "could not take its prefab's changes"));
		CHECK(f.FileText(path) == file);
		CHECK(f.Session.IsDirty());
		f.Run("prefab.close", { { "discard", true } });
	}

	TEST_CASE("A nested root kept by its holder survives coming back, and so does its holder")
	{
		NestingFixture f;
		Entity army = f.Session.GetScene()->CreateEntity("Army");
		f.InstantiateUnder(f.Squad, army);
		const std::string path = "Assets/Prefabs/Army.bprefab";
		f.Run("prefab.create", { { "entity", IdOf(army) }, { "path", path } });
		const std::string squadFile = f.FileText(f.Squad);

		f.Run("prefab.open", { { "path", path } });
		f.Session.GetEditScene()->FindEntityByName("Soldier").GetComponent<PointLightComponent>().Radius = 21;
		GunLight(*f.Session.GetEditScene()).Radius = 22;
		f.Session.MarkDirty();
		json dropped = ParseJson(squadFile);
		dropped["Entities"].erase(1);
		f.Run("asset.write", { { "path", f.Squad }, { "content", dropped.dump() } });
		f.Run("prefab.save");
		f.Run("prefab.close");

		f.Directory.WriteFile(f.Squad, squadFile);
		const json info = f.Info(path);
		CHECK(Contains(info["warnings"].dump(), "replaces the one in"));
		Entity instance = f.Instantiate(path);
		Entity soldier = Find(instance, "Soldier");
		REQUIRE(soldier);
		CHECK(Radius(soldier) == 21);
		size_t soldiers = 0;
		for (Entity child : Find(instance, "Squad").GetChildren())
			soldiers += child.GetName() == "Soldier" ? 1 : 0;
		CHECK(soldiers == 1);
		// What it holds comes once too, with the kept override.
		CHECK(soldier.GetChildren().size() == 1);
		CHECK(GunRadius(soldier) == 22);
	}

	TEST_CASE("Saving a nested root over its own variant writes overrides, not a copy")
	{
		NestingFixture f;
		f.Variant(f.Soldier, f.Elite);
		Entity holder = f.Session.GetScene()->CreateEntity("Holder");
		Entity nested = f.InstantiateUnder(f.Elite, holder);
		f.Run("prefab.create", { { "entity", IdOf(holder) }, { "path", "Assets/Prefabs/Holder.bprefab" } });
		Find(nested, "Gun").GetComponent<PointLightComponent>().Intensity = 55;
		Find(nested, "Gun").GetComponent<TagComponent>().Tag = "Rifle";
		f.Session.MarkDirty();
		f.Run("prefab.create", { { "entity", IdOf(nested) }, { "path", f.Elite } });
		CHECK(f.FileEntities(f.Elite).size() == 1);
		CHECK_FALSE(Contains(f.FileText(f.Elite), "null"));
		CHECK(f.Info(f.Elite)["base"] == f.Soldier);
		f.Edit(f.Soldier, [](Scene& edit) { GunLight(edit).Radius = 70; });
		Entity fresh = f.Instantiate(f.Elite);
		Entity rifle = Find(fresh, "Rifle");
		REQUIRE(rifle);
		CHECK(Radius(rifle) == 70);
		CHECK(rifle.GetComponent<PointLightComponent>().Intensity == 55);
	}

	TEST_CASE("Unpacking an outdated instance catches up first: nothing new reads as deleted")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Squad);
		json soldier = ParseJson(f.FileText(f.Soldier));
		json added = soldier["Entities"][1];
		added["ID"] = 777;
		added["Name"] = "Added";
		soldier["Entities"].push_back(added);
		f.Directory.WriteFile(f.Soldier, soldier.dump());
		f.Run("prefab.unpack", { { "entity", IdOf(instance) } });
		Entity nested = Find(instance, "Soldier");
		CHECK(f.Overrides(nested).empty());
		CHECK(Find(nested, "Added"));
		f.Edit(f.Soldier, [](Scene& edit) { GunLight(edit).Radius = 71; });
		CHECK(GunRadius(nested) == 71);
	}

	TEST_CASE("A nested root moved out and back belongs to its instance straight away")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Squad);
		Entity soldier = Find(instance, "Soldier");
		Entity parent = soldier.GetParent();
		auto scene = f.Session.GetScene();
		scene->SetParent(soldier, Entity{}, false);
		f.Edit(f.Soldier, [](Scene& edit) { GunLight(edit).Radius = 32; });
		REQUIRE(Prefab::IsTracked(soldier.GetComponent<PrefabComponent>()));
		scene->SetParent(soldier, parent, false);
		const json overrides = f.Overrides(instance);
		for (const auto& [field, value] : overrides.items())
			CHECK_FALSE(value.is_null());
		CHECK(Prefab::EnclosingInstance(soldier) == instance);
		// The next edit merges through the instance, and leaves a marker behind.
		const json result = f.Edit(f.Soldier, [](Scene& edit) { GunLight(edit).Intensity = 33; });
		CHECK(result["warnings"].empty());
		CHECK(Find(soldier, "Gun").GetComponent<PointLightComponent>().Intensity == 33);
		CHECK_FALSE(Prefab::ListsMembers(soldier.GetComponent<PrefabComponent>()));
	}

	TEST_CASE("Path spelling never decides what a file is")
	{
		NestingFixture f;
		const std::string shouted = "Assets/Prefabs/ELITE.bprefab";
		f.Variant(f.Soldier, f.Elite);
		if (!FileSystem::Exists(Breccia::Project::ResolvePath(shouted)))
			return;
		Entity instance = f.Instantiate(f.Elite);
		Entity gun = Find(instance, "Gun");
		const uint64_t gunId = IdOf(gun);
		gun.GetComponent<TagComponent>().Tag = "Rifle";
		gun.GetComponent<PointLightComponent>().Radius = 45;
		f.Session.MarkDirty();
		f.Run("prefab.create", { { "entity", IdOf(instance) }, { "path", shouted } });
		CHECK(f.Info(f.Elite)["base"] == f.Soldier);
		CHECK(IdOf(Find(instance, "Rifle")) == gunId);
		size_t children = 0;
		for (Entity child : instance.GetChildren())
			children += child.GetName() == "Rifle" ? 1 : 0;
		CHECK(children == 1);
	}

	TEST_CASE("Depth counts through a prefab read earlier, in any sibling order")
	{
		NestingFixture f;
		// Over > Top > the chain is exactly the limit. Top also reaches the chain through Longer, which is one too many, and
		// must say so even when the chain was read first, the short way.
		std::string previous = f.Soldier;
		for (size_t level = Prefab::MaxNestingDepth - 3; level > 0; --level)
		{
			const std::string path = "Assets/Prefabs/Chain" + std::to_string(level) + ".bprefab";
			f.WriteNesting(path, previous, 1);
			previous = path;
		}
		const std::string deep = previous;
		const auto write = [&](const std::string& path, const std::vector<std::string>& nested) {
			json entities = json::array({ json{ { "ID", 1 }, { "Name", "Root" }, { "Parent", 0 } } });
			uint64_t id = 2;
			for (const std::string& prefab : nested)
				entities.push_back({ { "ID", id++ }, { "Parent", 1 }, { "Components", { { "Prefab", { { "Prefab", prefab } } } } } });
			f.Directory.WriteFile(path, json{ { "Version", SceneSerializer::FormatVersion }, { "Entities", entities } }.dump());
		};
		write("Assets/Prefabs/Longer.bprefab", { deep });
		std::string error;
		json entities;
		write("Assets/Prefabs/Top.bprefab", { deep });
		write("Assets/Prefabs/Over.bprefab", { "Assets/Prefabs/Top.bprefab" });
		CHECK(Prefab::ReadFile(Breccia::Project::ResolvePath("Assets/Prefabs/Over.bprefab"), entities, error));
		for (const std::vector<std::string>& order : { std::vector<std::string>{ deep, "Assets/Prefabs/Longer.bprefab" }, std::vector<std::string>{ "Assets/Prefabs/Longer.bprefab", deep } })
		{
			write("Assets/Prefabs/Top.bprefab", order);
			write("Assets/Prefabs/Over.bprefab", { "Assets/Prefabs/Top.bprefab" });
			CHECK_FALSE(Prefab::ReadFile(Breccia::Project::ResolvePath("Assets/Prefabs/Over.bprefab"), entities, error));
			CHECK(Contains(error, "deeper than"));
		}
	}

	TEST_CASE("Warnings reach prefab.variant, prefab.refresh and asset.write once each")
	{
		NestingFixture f;
		f.Variant(f.Soldier, f.Elite);
		f.Edit(f.Elite, [](Scene& edit) { GunLight(edit).Radius = 17; });
		Entity instance = f.Instantiate(f.Elite);
		f.Edit(f.Soldier, [](Scene& edit) { edit.DestroyEntity(edit.FindEntityByName("Gun")); });
		const json variant = f.Run("prefab.variant", { { "base", f.Elite } });
		CHECK(Contains(variant["warnings"].dump(), "do nothing"));
		CHECK(Contains(f.Run("prefab.refresh", { { "entity", IdOf(instance) } })["warnings"].dump(), "do nothing"));
		CHECK(Contains(f.Run("prefab.revert", { { "entity", IdOf(instance) } })["warnings"].dump(), "do nothing"));

		// Two nested copies of a prefab with a dead override report it once.
		Entity holder = f.Session.GetScene()->CreateEntity("Holder");
		f.InstantiateUnder(f.Elite, holder);
		f.InstantiateUnder(f.Elite, holder);
		const json created = f.Run("prefab.create", { { "entity", IdOf(holder) }, { "path", "Assets/Prefabs/Holder.bprefab" } });
		size_t mentions = 0;
		for (const json& warning : created["warnings"])
			mentions += Contains(warning.get<std::string>(), "do nothing") ? 1 : 0;
		CHECK(mentions == 1);

		// No instance needs to exist for a written prefab to be read.
		f.WriteNesting("Assets/Prefabs/A.bprefab", "Assets/Prefabs/B.bprefab", 1);
		json cycle = ParseJson(f.FileText("Assets/Prefabs/A.bprefab"));
		cycle["Entities"][1]["Components"]["Prefab"]["Prefab"] = "Assets/Prefabs/B.bprefab";
		f.WriteNesting("Assets/Prefabs/B.bprefab", "Assets/Prefabs/A.bprefab", 1);
		const json written = f.Run("asset.write", { { "path", "Assets/Prefabs/A.bprefab" }, { "content", cycle.dump() } });
		CHECK(Contains(written["warnings"].dump(), "prefab cycle"));
	}

	TEST_CASE("A deleted nested prefab is reported through what holds it, and restoring it brings everything back")
	{
		NestingFixture f;
		f.Variant(f.Soldier, f.Elite);
		Entity squad = f.Instantiate(f.Squad);
		Entity elite = f.Instantiate(f.Elite);
		const std::string text = f.FileText(f.Soldier);
		const std::string meta = f.FileText(f.Soldier + ".meta");
		f.Run("asset.delete", { { "path", f.Soldier } });
		const json refreshed = f.Run("project.validate");
		CHECK(Contains(refreshed.dump(), "unresolved asset UUID"));
		f.Directory.WriteFile(f.Soldier + ".meta", meta);
		f.Directory.WriteFile(f.Soldier, text);
		f.Session.ReloadAssets({ { f.Soldier, FileChangeType::Added } });
		CHECK(f.Overrides(squad).empty());
		f.Edit(f.Soldier, [](Scene& edit) { GunLight(edit).Radius = 72; });
		CHECK(GunRadius(squad) == 72);
		CHECK(GunRadius(elite) == 72);
	}

	TEST_CASE("An empty Prefab component is no link: it does not stop a save or make a variant")
	{
		NestingFixture f;
		f.Run("prefab.open", { { "path", f.Soldier } });
		f.Run("component.set", { { "entity", "Gun" }, { "component", "Prefab" }, { "data", json::object() } });
		f.Run("component.set", { { "entity", "Soldier" }, { "component", "Prefab" }, { "data", json::object() } });
		f.Run("prefab.save");
		f.Run("prefab.close");
		CHECK_FALSE(Contains(f.FileText(f.Soldier), "\"Prefab\""));
		CHECK(f.Info(f.Soldier)["base"].is_null());
	}

	TEST_CASE("Hand-written small IDs do not collide across nested copies")
	{
		NestingFixture f;
		const auto write = [&](const std::string& path, const json& entities) {
			f.Directory.WriteFile(path, json{ { "Version", SceneSerializer::FormatVersion }, { "Entities", entities } }.dump());
		};
		const std::string inner = "Assets/Prefabs/Small.bprefab";
		write(inner, json::array({ json{ { "ID", 1 }, { "Name", "Root" }, { "Parent", 0 } }, json{ { "ID", 2 }, { "Name", "B" }, { "Parent", 1 } }, json{ { "ID", 3 }, { "Name", "C" }, { "Parent", 1 } } }));
		const json link = { { "Prefab", { { "Prefab", inner } } } };
		const std::string outer = "Assets/Prefabs/Pair.bprefab";
		write(outer, json::array({ json{ { "ID", 1 }, { "Name", "Pair" }, { "Parent", 0 } }, json{ { "ID", 2 }, { "Parent", 1 }, { "Components", link } }, json{ { "ID", 3 }, { "Parent", 1 }, { "Components", link } } }));
		CHECK(f.Info(outer)["entities"] == 7);
	}

	TEST_CASE("A prefab's preview depends on what its nested prefabs use, and a nesting loop ends")
	{
		TempDirectory project("ThumbnailNesting");
		ThumbnailCache cache(project.GetPath());
		const std::filesystem::path& root = project.GetPath();
		REQUIRE(FileSystem::WriteTextFile(root / "Assets/Prefabs/Outer.bprefab", R"({"Entities": [{"ID": 1, "Components": {
			"Prefab": {"Prefab": "Assets/Prefabs/Inner.bprefab", "Overrides": {"1": {"Components": {"Material": {"AlbedoMap": "Assets/Textures/Paint.png"}}}}}}}]})"));
		REQUIRE(FileSystem::WriteTextFile(root / "Assets/Prefabs/Inner.bprefab", R"({"Entities": [{"ID": 1, "Components": {
			"Mesh": {"Mesh": "Assets/Models/Ship.gltf"}}}, {"ID": 2, "Parent": 1, "Components": {"Prefab": {"Prefab": "Assets/Prefabs/Outer.bprefab"}}}]})"));
		const std::vector<std::string> expected = { "Assets/Models/Ship.gltf", "Assets/Prefabs/Inner.bprefab", "Assets/Prefabs/Outer.bprefab", "Assets/Textures/Paint.png" };
		CHECK(cache.GetDependencies("Assets/Prefabs/Outer.bprefab") == expected);

		// As a project stores them: a nested prefab by identity, which is followed through to what it uses.
		NestingFixture f;
		ThumbnailCache projectCache(f.Directory.GetDirectory());
		const auto dependencies = projectCache.GetDependencies(f.Squad);
		CHECK(std::find(dependencies.begin(), dependencies.end(), f.Soldier) != dependencies.end());
	}
}
