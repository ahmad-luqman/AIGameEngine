#include "AutomationTestUtils.h"
#include "TestUtils.h"

#include <doctest/doctest.h>

#include <Basalt/Asset/Thumbnails.h>
#include <Basalt/Core/FileSystem.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Prefab.h>
#include <Basalt/Scene/SceneSerializer.h>

#include <functional>

using namespace Basalt;
using namespace BasaltTest;
using json = nlohmann::json;

namespace {
	uint64_t IdOf(Entity entity)
	{
		return static_cast<uint64_t>(entity.GetUUID());
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
			EntityFrom(Run("prefab.instantiate", { { "path", Soldier }, { "parent", IdOf(SquadTree) }, { "position", { 1, 0, 0 } } }));
			Run("prefab.create", { { "entity", IdOf(SquadTree) }, { "path", Squad } });
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

		Entity Instantiate(const std::string& path, const json& position = { 9, 0, 0 })
		{
			return EntityFrom(Run("prefab.instantiate", { { "path", path }, { "position", position } }));
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
			const auto text = FileSystem::ReadTextFile(Basalt::Project::ResolvePath(path));
			REQUIRE(text);
			return *text;
		}

		json FileEntities(const std::string& path) const
		{
			return ParseJson(FileText(path))["Entities"];
		}

		// The first descendant of root named name, depth first.
		static Entity Find(Entity root, const std::string& name)
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

		static float Radius(Entity entity)
		{
			return entity.GetComponent<PointLightComponent>().Radius;
		}

		// A minimal file: a root that nests prefab count times.
		void WriteNesting(const std::string& path, const std::string& prefab, int count)
		{
			json entities = json::array({ json{ { "ID", 1 }, { "Name", "Root" }, { "Parent", 0 } } });
			for (int i = 0; i < count; ++i)
				entities.push_back({ { "ID", 2 + i }, { "Parent", 1 }, { "Components", { { "Prefab", { { "Prefab", prefab } } } } } });
			Directory.WriteFile(path, json{ { "Format", "BasaltPrefab" }, { "Entities", entities } }.dump());
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
		const json& nested = entities[1]["Components"]["Prefab"];
		CHECK(entities[1]["Components"].size() == 1);
		CHECK(entities[1]["Parent"] == entities[0]["ID"]);
		CHECK(nested["Prefab"]["Path"] == f.Soldier);
		// The nested root's place in Squad is Squad's, whatever the Soldier's root does; nothing else differs.
		REQUIRE(nested["Overrides"].size() == 1);
		const json& placement = nested["Overrides"].begin().value();
		CHECK(placement.size() == 1);
		CHECK(placement["Components"]["Transform"].size() == 2);
		CHECK(placement["Components"]["Transform"]["Translation"] == json{ 1.0, 0.0, 0.0 });

		// The tree is an instance of Squad, tracking everything: its nested root is only a marker.
		const auto& link = f.SquadTree.GetComponent<PrefabComponent>();
		CHECK(link.Prefab == f.Squad);
		CHECK(link.Members.size() == 3);
		Entity soldier = NestingFixture::Find(f.SquadTree, "Soldier");
		const auto& marker = soldier.GetComponent<PrefabComponent>();
		CHECK(marker.Prefab == f.Soldier);
		CHECK(marker.Members.empty());
		CHECK(marker.Baseline.empty());
		CHECK(f.Overrides(f.SquadTree).empty());
		CHECK(Prefab::EnclosingInstance(soldier) == f.SquadTree);

		const json info = f.Run("prefab.info", { { "path", f.Squad } });
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

		const json result = f.Edit(f.Soldier, [](Scene& scene) { scene.FindEntityByName("Gun").GetComponent<PointLightComponent>().Radius = 30; });
		// Soldier's authoring tree, Squad's and the two instances; none of them nested on their own.
		CHECK(result["instances"] == 4);
		CHECK(f.FileText(f.Squad) == squadFile);
		for (Entity instance : { first, second, f.SquadTree })
		{
			CHECK(NestingFixture::Radius(NestingFixture::Find(instance, "Gun")) == 30);
			CHECK(f.Overrides(instance).empty());
		}
		// Instantiating reads the same flat source.
		CHECK(NestingFixture::Radius(NestingFixture::Find(f.Instantiate(f.Squad), "Gun")) == 30);
	}

	TEST_CASE("An outer prefab's overrides and an instance's overrides both survive inner edits, each at its level")
	{
		NestingFixture f;
		f.Edit(f.Squad, [](Scene& scene) { scene.FindEntityByName("Gun").GetComponent<PointLightComponent>().Radius = 11; });
		Entity instance = f.Instantiate(f.Squad);
		Entity gun = NestingFixture::Find(instance, "Gun");
		CHECK(NestingFixture::Radius(gun) == 11);
		CHECK(f.Overrides(instance).empty());
		gun.GetComponent<PointLightComponent>().Intensity = 22;
		f.Session.MarkDirty();

		f.Edit(f.Soldier, [](Scene& scene) {
			auto& light = scene.FindEntityByName("Gun").GetComponent<PointLightComponent>();
			light.Radius = 33;
			light.Intensity = 44;
			light.Color = { 0.25f, 0.5f, 0.75f };
		});
		const auto& light = gun.GetComponent<PointLightComponent>();
		CHECK(light.Radius == 11);
		CHECK(light.Intensity == 22);
		CHECK(light.Color == glm::vec3(0.25f, 0.5f, 0.75f));
		// The bare Soldier takes everything; Squad's file still only differs in what it overrode.
		CHECK(NestingFixture::Radius(NestingFixture::Find(f.SoldierTree, "Gun")) == 33);
		const json overrides = f.Overrides(instance);
		REQUIRE(overrides.size() == 1);
		CHECK(Contains(overrides.begin().key(), "/Components/PointLight/Intensity"));
	}

	TEST_CASE("A nested root's place belongs to the prefab holding it; a variant's follows its base until changed")
	{
		NestingFixture f;
		const std::string elite = "Assets/Prefabs/Elite.bprefab";
		f.Run("prefab.variant", { { "base", f.Soldier }, { "path", elite } });
		f.Edit(f.Soldier, [](Scene& scene) { scene.FindEntityByName("Soldier").GetTransform().Translation = { 0, 5, 0 }; });
		CHECK(NestingFixture::Find(f.SquadTree, "Soldier").GetTransform().Translation == glm::vec3(1, 0, 0));
		CHECK(f.Overrides(f.SquadTree).empty());
		json entities;
		std::string error;
		REQUIRE(Prefab::ReadFile(Basalt::Project::ResolvePath(elite), entities, error));
		CHECK(entities[0]["Components"]["Transform"]["Translation"] == json{ 0.0, 5.0, 0.0 });

		// A placed instance keeps its place whatever the file's root does, nested or not.
		Entity instance = f.Instantiate(elite);
		CHECK(instance.GetTransform().Translation == glm::vec3(9, 0, 0));
		f.Edit(f.Soldier, [](Scene& scene) { scene.FindEntityByName("Soldier").GetTransform().Translation = { 0, 6, 0 }; });
		CHECK(instance.GetTransform().Translation == glm::vec3(9, 0, 0));
		CHECK(f.Overrides(instance).empty());
	}

	TEST_CASE("Entities an inner prefab adds or removes appear and go in every instance, once each")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Squad);
		// The instance's own entity under a nested entity stays when that entity's prefab changes.
		auto scene = f.Session.GetScene();
		Entity local = scene->CreateEntity("Local");
		scene->SetParent(local, NestingFixture::Find(instance, "Soldier"), false);
		const auto before = scene->GetEntityCount();
		const auto instances = 3; // The instance, Squad's tree and Soldier's tree each gain the entity.

		f.Edit(f.Soldier, [](Scene& edit) {
			Entity scope = edit.CreateEntity("Scope");
			edit.SetParent(scope, edit.FindEntityByName("Gun"), false);
		});
		CHECK(scene->GetEntityCount() == before + instances);
		Entity scope = NestingFixture::Find(instance, "Scope");
		REQUIRE(scope);
		CHECK(scope.GetParent() == NestingFixture::Find(instance, "Gun"));
		CHECK(f.Overrides(instance).empty());
		CHECK(local.GetParent() == NestingFixture::Find(instance, "Soldier"));

		f.Edit(f.Soldier, [](Scene& edit) { edit.DestroyEntity(edit.FindEntityByName("Scope")); });
		CHECK(scene->GetEntityCount() == before);
		CHECK_FALSE(NestingFixture::Find(instance, "Scope"));
		CHECK(f.Overrides(instance).empty());
	}

	TEST_CASE("Two nested copies of a prefab are separate entities with separate overrides")
	{
		NestingFixture f;
		Entity pair = f.Instantiate(f.Soldier);
		f.Session.GetScene()->DestroyEntity(pair);
		Entity holder = f.Session.GetScene()->CreateEntity("Pair");
		Entity left = f.EntityFrom(f.Run("prefab.instantiate", { { "path", f.Soldier }, { "parent", IdOf(holder) } }));
		Entity right = f.EntityFrom(f.Run("prefab.instantiate", { { "path", f.Soldier }, { "parent", IdOf(holder) } }));
		left.GetComponent<TagComponent>().Tag = "Left";
		right.GetComponent<TagComponent>().Tag = "Right";
		NestingFixture::Find(right, "Gun").GetComponent<PointLightComponent>().Radius = 2;
		const std::string path = "Assets/Prefabs/Pair.bprefab";
		f.Run("prefab.create", { { "entity", IdOf(holder) }, { "path", path } });
		CHECK(f.FileEntities(path).size() == 3);

		Entity instance = f.Instantiate(path);
		CHECK(f.Overrides(instance).empty());
		CHECK(instance.GetComponent<PrefabComponent>().Members.size() == 5);
		CHECK(NestingFixture::Radius(NestingFixture::Find(NestingFixture::Find(instance, "Left"), "Gun")) == 7);
		CHECK(NestingFixture::Radius(NestingFixture::Find(NestingFixture::Find(instance, "Right"), "Gun")) == 2);
		CHECK(NestingFixture::Find(instance, "Left").GetName() == "Left");

		f.Edit(f.Soldier, [](Scene& edit) { edit.FindEntityByName("Gun").GetComponent<PointLightComponent>().Radius = 40; });
		CHECK(NestingFixture::Radius(NestingFixture::Find(NestingFixture::Find(instance, "Left"), "Gun")) == 40);
		CHECK(NestingFixture::Radius(NestingFixture::Find(NestingFixture::Find(instance, "Right"), "Gun")) == 2);
	}

	TEST_CASE("Prefabs nest through several levels, and references follow into nested entities")
	{
		NestingFixture f;
		auto scene = f.Session.GetScene();
		Entity army = scene->CreateEntity("Army");
		Entity squad = f.EntityFrom(f.Run("prefab.instantiate", { { "path", f.Squad }, { "parent", IdOf(army) } }));
		Entity pointer = scene->CreateEntity("Pointer");
		scene->SetParent(pointer, army, false);
		pointer.AddComponent<BoneAttachmentComponent>().Target = NestingFixture::Find(squad, "Gun").GetUUID();
		const std::string path = "Assets/Prefabs/Army.bprefab";
		const json result = f.Run("prefab.create", { { "entity", IdOf(army) }, { "path", path } });
		CHECK(result["linked"] == true);
		CHECK(result["warnings"].empty());
		// Army, its one nested record and Pointer: Squad's tree is Squad's business.
		CHECK(f.FileEntities(path).size() == 3);
		CHECK(f.Run("prefab.info", { { "path", path } })["dependencies"].size() == 2);
		CHECK(f.Overrides(army).empty());

		Entity instance = f.Instantiate(path);
		CHECK(f.Overrides(instance).empty());
		CHECK(instance.GetComponent<PrefabComponent>().Members.size() == 5);
		Entity gun = NestingFixture::Find(instance, "Gun");
		REQUIRE(gun);
		CHECK(NestingFixture::Find(instance, "Pointer").GetComponent<BoneAttachmentComponent>().Target == gun.GetUUID());
		// Both inner levels are markers inside it.
		CHECK(NestingFixture::Find(instance, "Squad").GetComponent<PrefabComponent>().Prefab == f.Squad);
		CHECK(NestingFixture::Find(instance, "Soldier").GetComponent<PrefabComponent>().Prefab == f.Soldier);

		f.Edit(f.Soldier, [](Scene& edit) { edit.FindEntityByName("Gun").GetComponent<PointLightComponent>().Radius = 50; });
		CHECK(NestingFixture::Radius(gun) == 50);
		CHECK(NestingFixture::Find(instance, "Pointer").GetComponent<BoneAttachmentComponent>().Target == gun.GetUUID());
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
		Entity second = f.EntityFrom(f.Run("prefab.instantiate", { { "path", f.Soldier }, { "parent", IdOf(pointer) } }));
		Entity secondGun = NestingFixture::Find(second, "Gun");
		pointer.GetComponent<BoneAttachmentComponent>().Target = secondGun.GetUUID();
		second.GetComponent<TagComponent>().Tag = "Second";
		f.Session.MarkDirty();
		f.Run("prefab.save");
		f.Run("prefab.close");

		// Squad, the nested Soldier, Added, Pointer and the second nested Soldier.
		CHECK(f.FileEntities(f.Squad).size() == 5);
		Entity instance = f.Instantiate(f.Squad);
		CHECK(f.Overrides(instance).empty());
		Entity savedSecond = NestingFixture::Find(instance, "Second");
		REQUIRE(savedSecond);
		CHECK(NestingFixture::Find(instance, "Pointer").GetComponent<BoneAttachmentComponent>().Target == NestingFixture::Find(savedSecond, "Gun").GetUUID());
		CHECK(NestingFixture::Find(NestingFixture::Find(instance, "Gun"), "Added"));
		CHECK(NestingFixture::Radius(NestingFixture::Find(instance, "Gun")) == 13);

		// Opening and saving without edits changes no ID.
		const json entities = f.FileEntities(f.Squad);
		f.Edit(f.Squad, [](Scene&) {});
		CHECK(f.FileEntities(f.Squad) == entities);
	}

	TEST_CASE("Applying an instance writes nested overrides into the prefab holding them, as differences")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Squad);
		NestingFixture::Find(instance, "Gun").GetComponent<PointLightComponent>().Radius = 99;
		f.Session.MarkDirty();
		f.Run("prefab.apply", { { "entity", IdOf(instance) } });
		CHECK(f.Overrides(instance).empty());
		CHECK(f.FileEntities(f.Squad).size() == 2);
		CHECK(Contains(f.FileText(f.Squad), "Radius"));
		// Squad's instances take it, Soldier's do not.
		CHECK(NestingFixture::Radius(NestingFixture::Find(f.SquadTree, "Gun")) == 99);
		CHECK(NestingFixture::Radius(NestingFixture::Find(f.SoldierTree, "Gun")) == 7);
		CHECK(NestingFixture::Find(instance, "Soldier").HasComponent<PrefabComponent>());

		// Deleting a nested entity in an instance and applying it stores the deletion alone.
		f.Session.GetScene()->DestroyEntity(NestingFixture::Find(instance, "Gun"));
		f.Session.MarkDirty();
		f.Run("prefab.apply", { { "entity", IdOf(instance) } });
		CHECK_FALSE(NestingFixture::Find(f.SquadTree, "Gun"));
		const json entities = f.FileEntities(f.Squad);
		REQUIRE(entities.size() == 2);
		bool deleted = false;
		for (const auto& [key, patch] : entities[1]["Components"]["Prefab"]["Overrides"].items())
			deleted |= patch.is_null();
		CHECK(deleted);
		CHECK(f.Run("prefab.info", { { "path", f.Squad } })["entities"] == 2);
	}

	TEST_CASE("A nested root is part of the instance holding it: its commands say so, and unpacking one is an override")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Squad);
		Entity soldier = NestingFixture::Find(instance, "Soldier");
		for (const char* command : { "prefab.apply", "prefab.refresh", "prefab.revert" })
			CHECK(Contains(f.RunError(command, { { "entity", IdOf(soldier) } }), "nested in the instance 'Squad'"));
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
		f.Edit(f.Squad, [](Scene& scene) { scene.FindEntityByName("Gun").GetComponent<PointLightComponent>().Radius = 11; });
		Entity instance = f.Instantiate(f.Squad);
		f.Run("prefab.unpack", { { "entity", IdOf(instance) } });
		CHECK_FALSE(instance.HasComponent<PrefabComponent>());
		Entity soldier = NestingFixture::Find(instance, "Soldier");
		REQUIRE(Prefab::IsTracked(soldier.GetComponent<PrefabComponent>()));
		CHECK_FALSE(Prefab::EnclosingInstance(soldier));
		const json overrides = f.Overrides(soldier);
		REQUIRE(overrides.size() == 1);
		CHECK(overrides.begin().value() == 11);

		f.Edit(f.Soldier, [](Scene& edit) { edit.FindEntityByName("Gun").GetComponent<PointLightComponent>().Intensity = 8; });
		Entity gun = NestingFixture::Find(soldier, "Gun");
		CHECK(gun.GetComponent<PointLightComponent>().Intensity == 8);
		CHECK(NestingFixture::Radius(gun) == 11);
	}

	TEST_CASE("Unpacking keeps deeper nesting inside the instance it leaves tracked")
	{
		NestingFixture f;
		auto scene = f.Session.GetScene();
		Entity army = scene->CreateEntity("Army");
		f.Run("prefab.instantiate", { { "path", f.Squad }, { "parent", IdOf(army) } });
		const std::string path = "Assets/Prefabs/Army.bprefab";
		f.Run("prefab.create", { { "entity", IdOf(army) }, { "path", path } });
		Entity instance = f.Instantiate(path);
		const auto before = scene->GetEntityCount();
		f.Run("prefab.unpack", { { "entity", IdOf(instance) } });
		// The Squad adopts what it finds: its nested Soldier and Gun are members, not additions or deletions.
		Entity squad = NestingFixture::Find(instance, "Squad");
		REQUIRE(Prefab::IsTracked(squad.GetComponent<PrefabComponent>()));
		CHECK(f.Overrides(squad).empty());
		Entity soldier = NestingFixture::Find(squad, "Soldier");
		CHECK(Prefab::EnclosingInstance(soldier) == squad);
		f.Edit(f.Soldier, [](Scene& edit) { edit.FindEntityByName("Gun").GetComponent<PointLightComponent>().Radius = 90; });
		CHECK(NestingFixture::Radius(NestingFixture::Find(soldier, "Gun")) == 90);
		CHECK(scene->GetEntityCount() == before);
	}

	TEST_CASE("A prefab that stops nesting turns its instances' nested roots into plain members")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Squad);
		Entity soldier = NestingFixture::Find(instance, "Soldier");
		Entity gun = NestingFixture::Find(instance, "Gun");
		f.Run("prefab.open", { { "path", f.Squad } });
		f.Run("prefab.unpack", { { "entity", "Soldier" } });
		f.Run("prefab.save");
		f.Run("prefab.close");
		CHECK(f.FileEntities(f.Squad).size() == 3);
		CHECK_FALSE(Contains(f.FileText(f.Squad), "\"Prefab\""));
		// The same entities, no longer a link: an inner edit stops reaching them.
		CHECK_FALSE(soldier.HasComponent<PrefabComponent>());
		CHECK(NestingFixture::Find(instance, "Gun") == gun);
		CHECK(f.Overrides(instance).empty());
		f.Edit(f.Soldier, [](Scene& edit) { edit.FindEntityByName("Gun").GetComponent<PointLightComponent>().Radius = 91; });
		CHECK(NestingFixture::Radius(gun) == 7);
	}

	TEST_CASE("Ownership stops at an instance that tracks itself, even one a hand-edited Members value lists")
	{
		NestingFixture f;
		auto scene = f.Session.GetScene();
		Entity outer = f.Instantiate(f.Squad);
		Entity local = f.Instantiate(f.Soldier);
		scene->SetParent(local, NestingFixture::Find(outer, "Gun"), false);
		CHECK_FALSE(Prefab::EnclosingInstance(local));
		const std::string gunKey = [&] {
			for (const auto& [key, value] : outer.GetComponent<PrefabComponent>().Members.items())
				if (value == IdOf(NestingFixture::Find(outer, "Gun")))
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
		f.Edit(f.Soldier, [](Scene& edit) { edit.FindEntityByName("Gun").GetComponent<PointLightComponent>().Radius = 92; });
		// Adopting would freeze its differences from this very edit as overrides; a write of its own file adopts it.
		CHECK_FALSE(Prefab::IsTracked(instance.GetComponent<PrefabComponent>()));
		CHECK(NestingFixture::Radius(NestingFixture::Find(f.SquadTree, "Gun")) == 92);
	}

	TEST_CASE("Variants inherit their base and store only their differences")
	{
		NestingFixture f;
		const std::string elite = "Assets/Prefabs/Elite.bprefab";
		const json created = f.Run("prefab.variant", { { "base", f.Soldier }, { "path", elite } });
		CHECK(created["instances"] == 0);
		const json entities = f.FileEntities(elite);
		REQUIRE(entities.size() == 1);
		CHECK(entities[0]["Parent"] == 0);
		CHECK(entities[0]["Components"].size() == 1);
		CHECK_FALSE(entities[0]["Components"]["Prefab"].contains("Overrides"));
		const json info = f.Run("prefab.info", { { "path", elite } });
		CHECK(info["base"] == f.Soldier);
		CHECK(info["entities"] == 2);

		Entity instance = f.Instantiate(elite);
		CHECK(instance.GetName() == "Soldier");
		CHECK(instance.GetComponent<PrefabComponent>().Prefab == elite);
		CHECK(NestingFixture::Radius(NestingFixture::Find(instance, "Gun")) == 7);
		CHECK(f.Overrides(instance).empty());

		// The variant changes one field, renames its root, adds an entity, and removes nothing else.
		f.Edit(elite, [](Scene& scene) {
			Entity root = scene.FindEntityByName("Soldier");
			root.GetComponent<TagComponent>().Tag = "Elite";
			scene.FindEntityByName("Gun").GetComponent<PointLightComponent>().Radius = 17;
			Entity badge = scene.CreateEntity("Badge");
			scene.SetParent(badge, root, false);
		});
		const json edited = f.FileEntities(elite);
		// The nested root and the variant's own entity; the Gun lives in the root's overrides.
		REQUIRE(edited.size() == 2);
		REQUIRE(edited[0]["Components"]["Prefab"]["Overrides"].size() == 2);
		CHECK(instance.GetName() == "Elite");
		CHECK(NestingFixture::Radius(NestingFixture::Find(instance, "Gun")) == 17);
		CHECK(NestingFixture::Find(instance, "Badge"));
		CHECK(f.Overrides(instance).empty());
		// The base did not change.
		CHECK(NestingFixture::Radius(NestingFixture::Find(f.SoldierTree, "Gun")) == 7);
		CHECK_FALSE(NestingFixture::Find(f.SoldierTree, "Badge"));

		// The base's edits reach the variant's instances, except what the variant overrides.
		f.Edit(f.Soldier, [](Scene& scene) {
			auto& light = scene.FindEntityByName("Gun").GetComponent<PointLightComponent>();
			light.Radius = 60;
			light.Intensity = 61;
			Entity late = scene.CreateEntity("Late");
			scene.SetParent(late, scene.FindEntityByName("Soldier"), false);
		});
		Entity gun = NestingFixture::Find(instance, "Gun");
		const auto& light = gun.GetComponent<PointLightComponent>();
		CHECK(light.Radius == 17);
		CHECK(light.Intensity == 61);
		CHECK(NestingFixture::Find(instance, "Late"));
		CHECK(f.Overrides(instance).empty());
		CHECK(NestingFixture::Find(f.Instantiate(elite), "Late"));
	}

	TEST_CASE("Variants of variants resolve base by base; applying to a variant's instance stays a variant")
	{
		NestingFixture f;
		const std::string elite = "Assets/Prefabs/Elite.bprefab";
		const std::string captain = "Assets/Prefabs/Captain.bprefab";
		f.Run("prefab.variant", { { "base", f.Soldier }, { "path", elite } });
		f.Edit(elite, [](Scene& scene) { scene.FindEntityByName("Gun").GetComponent<PointLightComponent>().Radius = 17; });
		f.Run("prefab.variant", { { "base", elite }, { "path", captain } });
		f.Edit(captain, [](Scene& scene) { scene.FindEntityByName("Gun").GetComponent<PointLightComponent>().Intensity = 27; });

		Entity instance = f.Instantiate(captain);
		Entity gun = NestingFixture::Find(instance, "Gun");
		CHECK(NestingFixture::Radius(gun) == 17);
		CHECK(gun.GetComponent<PointLightComponent>().Intensity == 27);
		CHECK(f.Run("prefab.info", { { "path", captain } })["dependencies"].size() == 2);

		// A base two levels up reaches it.
		f.Edit(f.Soldier, [](Scene& scene) { scene.FindEntityByName("Gun").GetComponent<PointLightComponent>().Color = { 0.5f, 0.5f, 0.5f }; });
		CHECK(gun.GetComponent<PointLightComponent>().Color == glm::vec3(0.5f));
		CHECK(f.Overrides(instance).empty());

		gun.GetComponent<PointLightComponent>().Radius = 47;
		f.Session.MarkDirty();
		const std::string baseFile = f.FileText(elite);
		f.Run("prefab.apply", { { "entity", IdOf(instance) } });
		CHECK(f.FileEntities(captain).size() == 1);
		CHECK(f.FileText(elite) == baseFile);
		CHECK(f.Run("prefab.info", { { "path", captain } })["base"] == elite);
		CHECK(NestingFixture::Radius(NestingFixture::Find(f.Instantiate(captain), "Gun")) == 47);
		CHECK(NestingFixture::Radius(NestingFixture::Find(f.Instantiate(elite), "Gun")) == 17);
	}

	TEST_CASE("prefab.create turns an instance into a variant that tracks it, and keeps a variant one when saved again")
	{
		NestingFixture f;
		const std::string elite = "Assets/Prefabs/Elite.bprefab";
		Entity instance = f.Instantiate(f.Soldier);
		Entity gun = NestingFixture::Find(instance, "Gun");
		gun.GetComponent<PointLightComponent>().Radius = 19;
		f.Session.MarkDirty();

		CHECK(Contains(f.RunError("prefab.create", { { "entity", IdOf(gun) }, { "path", elite }, { "variant", true } }), "not an instance root"));
		CHECK(Contains(f.RunError("prefab.create", { { "entity", IdOf(instance) }, { "path", f.Soldier }, { "variant", true } }), "variant of itself"));
		const json result = f.Run("prefab.create", { { "entity", IdOf(instance) }, { "path", elite }, { "variant", true } });
		CHECK(result["linked"] == true);
		CHECK(result["warnings"].empty());
		CHECK(instance.GetComponent<PrefabComponent>().Prefab == elite);
		CHECK(f.Overrides(instance).empty());
		CHECK(f.FileEntities(elite).size() == 1);
		CHECK(f.Run("prefab.info", { { "path", elite } })["base"] == f.Soldier);

		f.Edit(f.Soldier, [](Scene& scene) { scene.FindEntityByName("Gun").GetComponent<PointLightComponent>().Intensity = 29; });
		CHECK(gun.GetComponent<PointLightComponent>().Intensity == 29);
		CHECK(NestingFixture::Radius(gun) == 19);

		// Saving the instance over its own variant writes the variant again.
		gun.GetComponent<PointLightComponent>().Radius = 39;
		f.Run("prefab.create", { { "entity", IdOf(instance) }, { "path", elite } });
		CHECK(f.FileEntities(elite).size() == 1);
		CHECK(f.Run("prefab.info", { { "path", elite } })["base"] == f.Soldier);
		CHECK(NestingFixture::Radius(NestingFixture::Find(f.Instantiate(elite), "Gun")) == 39);
	}

	TEST_CASE("prefab.variant needs a readable base and a new file; variants of unreadable bases fail")
	{
		NestingFixture f;
		CHECK(Contains(f.RunError("prefab.variant", { { "base", "Assets/Prefabs/Missing.bprefab" }, { "path", "Assets/Prefabs/V.bprefab" } }), "Missing.bprefab"));
		CHECK_FALSE(FileSystem::Exists(Basalt::Project::ResolvePath("Assets/Prefabs/V.bprefab")));
		CHECK(Contains(f.RunError("prefab.variant", { { "base", f.Soldier }, { "path", f.Squad } }), "already exists"));
		CHECK(Contains(f.RunError("prefab.variant", { { "base", "Assets/Soldier.txt" }, { "path", "Assets/Prefabs/V.bprefab" } }), "expected a .bprefab"));
		f.Run("prefab.variant", { { "base", f.Soldier }, { "path", "Assets/Prefabs/V.bprefab" } });
		std::filesystem::remove(Basalt::Project::ResolvePath(f.Soldier));
		CHECK(Contains(f.RunError("prefab.instantiate", { { "path", "Assets/Prefabs/V.bprefab" } }), "Soldier.bprefab"));
		// Instances of it keep their state and say why, reached through the base they read.
		Entity squad = f.SquadTree;
		const auto report = f.Session.ReloadAssets({ { f.Soldier, FileChangeType::Removed } });
		REQUIRE(report.PrefabErrors.size() == 2);
		CHECK(Contains(report.PrefabErrors[0] + report.PrefabErrors[1], "'Squad'"));
		CHECK(NestingFixture::Find(squad, "Gun"));
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
		f.Run("prefab.instantiate", { { "path", f.Squad }, { "parent", IdOf(holder) } });
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
			return Prefab::ReadFile(Basalt::Project::ResolvePath(path), entities, error);
		};
		// A chain just past the depth limit.
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
			f.Directory.WriteFile(path, json{ { "Entities", json::array({ json{ { "ID", 1 }, { "Parent", 0 }, { "Components", { { "Prefab", { { "Prefab", f.Soldier }, { "Overrides", overrides } } } } } } }) } }.dump());
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
		CHECK(f.Run("prefab.info", { { "path", path } })["entities"] == 2);
		write(json{ { "1", { { "ID", 99 } } } });
		json expanded;
		std::string error;
		REQUIRE(Prefab::ReadFile(Basalt::Project::ResolvePath(path), expanded, error));
		CHECK(expanded[0]["ID"] == 1);
		// A link without a path is no link; files before version 4 could not nest, so what they hold is not one either.
		const json plain = { { "ID", 1 }, { "Name", "Plain" }, { "Parent", 0 }, { "Components", { { "Prefab", { { "Members", json::object() } } } } } };
		f.Directory.WriteFile(path, json{ { "Entities", json::array({ plain }) } }.dump());
		CHECK(f.EntityFrom(f.Run("prefab.instantiate", { { "path", path } })).GetChildren().empty());
		json older = plain;
		older["Components"]["Prefab"]["Prefab"] = f.Soldier;
		f.Directory.WriteFile(path, json{ { "Version", 3 }, { "Entities", json::array({ older }) } }.dump());
		CHECK(f.EntityFrom(f.Run("prefab.instantiate", { { "path", path } })).GetChildren().empty());
	}

	TEST_CASE("Overrides of entities the nested prefab dropped do nothing, and validation says so")
	{
		NestingFixture f;
		const std::string elite = "Assets/Prefabs/Elite.bprefab";
		f.Run("prefab.variant", { { "base", f.Soldier }, { "path", elite } });
		f.Edit(elite, [](Scene& scene) {
			Entity gun = scene.FindEntityByName("Gun");
			gun.GetComponent<PointLightComponent>().Radius = 17;
			Entity sight = scene.CreateEntity("Sight");
			scene.SetParent(sight, gun, false);
		});
		Entity instance = f.Instantiate(elite);
		CHECK(NestingFixture::Find(instance, "Sight"));

		f.Edit(f.Soldier, [](Scene& scene) { scene.DestroyEntity(scene.FindEntityByName("Gun")); });
		const json info = f.Run("prefab.info", { { "path", elite } });
		// The override, and the parent of the variant's entity.
		REQUIRE(info["warnings"].size() == 2);
		CHECK(Contains(info["warnings"][0].get<std::string>(), "do nothing"));
		CHECK(Contains(info["warnings"][1].get<std::string>(), "moved to the root"));
		CHECK(Contains(f.Run("project.validate").dump(), "do nothing"));
		CHECK_FALSE(NestingFixture::Find(instance, "Gun"));
		Entity sight = NestingFixture::Find(instance, "Sight");
		REQUIRE(sight);
		CHECK(sight.GetParent() == instance);
		// A fresh instance agrees, and saving forgets the dead override.
		Entity fresh = f.Instantiate(elite);
		CHECK(NestingFixture::Find(fresh, "Sight").GetParent() == fresh);
		f.Edit(elite, [](Scene&) {});
		CHECK(f.Run("prefab.info", { { "path", elite } })["warnings"].empty());
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
		CHECK(NestingFixture::Radius(NestingFixture::Find(loaded, "Gun")) == 80);
		CHECK(NestingFixture::Find(loaded, "Soldier").GetComponent<PrefabComponent>().Prefab == f.Soldier);
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
		CHECK(NestingFixture::Find(instance, "Gun").GetComponent<ScriptComponent>().Script == "Assets/Moved.lua");
		CHECK(f.Overrides(instance).empty());
		CHECK(NestingFixture::Find(f.Instantiate(f.Squad), "Gun").GetComponent<ScriptComponent>().Script == "Assets/Moved.lua");

		// The nested prefab itself: the reference and every baseline's marker move with it.
		const std::string moved = "Assets/Prefabs/Moved/Soldier.bprefab";
		f.Run("asset.move", { { "from", f.Soldier }, { "to", moved } });
		CHECK(f.Run("prefab.info", { { "path", f.Squad } })["dependencies"] == json::array({ moved }));
		CHECK(NestingFixture::Find(instance, "Soldier").GetComponent<PrefabComponent>().Prefab == moved);
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
		CHECK(NestingFixture::Find(f.Instantiate(f.Squad), "Gun").GetComponent<ScriptComponent>().Script == "Assets/Moved.lua");
		const std::string moved = "Assets/Prefabs/Moved/Soldier.bprefab";
		CHECK(f.FileEntities(f.Squad)[1]["Components"]["Prefab"]["Prefab"].is_string());
		f.Run("asset.move", { { "from", f.Soldier }, { "to", moved } });
		CHECK(f.FileEntities(f.Squad)[1]["Components"]["Prefab"]["Prefab"].is_object());
		CHECK(f.Run("prefab.info", { { "path", f.Squad } })["dependencies"] == json::array({ moved }));
	}

	TEST_CASE("Copies of an outer instance track their own nested entities; play instances are flat too")
	{
		NestingFixture f;
		Entity instance = f.Instantiate(f.Squad);
		const json copy = f.Run("entity.duplicate", { { "entity", IdOf(instance) } });
		Entity duplicate = f.EntityFrom(copy);
		CHECK(f.Overrides(duplicate).empty());
		f.Edit(f.Soldier, [](Scene& scene) { scene.FindEntityByName("Gun").GetComponent<PointLightComponent>().Radius = 31; });
		CHECK(NestingFixture::Radius(NestingFixture::Find(duplicate, "Gun")) == 31);

		// A copy of a nested root is the scene's own instance now, which the next prefab write adopts.
		const json nested = f.Run("entity.duplicate", { { "entity", IdOf(NestingFixture::Find(instance, "Soldier")) } });
		Entity lone = f.EntityFrom(nested);
		CHECK_FALSE(Prefab::EnclosingInstance(lone));
		f.Edit(f.Soldier, [](Scene& scene) { scene.FindEntityByName("Gun").GetComponent<PointLightComponent>().Radius = 32; });
		CHECK(Prefab::IsTracked(lone.GetComponent<PrefabComponent>()));
		CHECK(NestingFixture::Radius(NestingFixture::Find(instance, "Gun")) == 32);

		f.Run("play.start");
		const json played = f.Run("prefab.instantiate", { { "path", f.Squad } });
		Entity spawned = f.EntityFrom(played);
		CHECK(NestingFixture::Radius(NestingFixture::Find(spawned, "Gun")) == 32);
		CHECK(spawned.GetComponent<PrefabComponent>().Members.size() == 3);
		CHECK(spawned.GetComponent<PrefabComponent>().Baseline.empty());
		CHECK(NestingFixture::Find(spawned, "Soldier").GetComponent<PrefabComponent>().Prefab == f.Soldier);
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
	}
}
