#include "AnimationTestUtils.h"
#include "JointAssetDraft.h"
#include "TestUtils.h"

#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Core/FileSystem.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>

#include <doctest/doctest.h>

using namespace Basalt;
using namespace BasaltTest;

using AnimationTest::FixturePath;
using AnimationTest::LoadModel;

TEST_SUITE("Editor")
{
	TEST_CASE("Mask drafts track edits against their file")
	{
		TempProject project("MaskDraft");
		const std::string path = project.WriteFile("Assets/Upper.banimmask", R"({ "Joints": [ { "Joint": "Spine", "Weight": 0.5 } ] })");
		AnimationMaskDraft draft;
		std::string error;
		CHECK_FALSE(draft.IsOpen());
		CHECK_FALSE(draft.Load("Assets/Missing.banimmask", error));
		CHECK(error == "cannot read 'Assets/Missing.banimmask'");
		CHECK_FALSE(draft.IsOpen());
		REQUIRE_MESSAGE(draft.Load(path, error), error);
		CHECK(draft.GetPath() == path);
		CHECK_FALSE(draft.IsDirty());

		// A failed load changes nothing.
		const std::string broken = project.WriteFile("Assets/Broken.banimmask", R"({ "Joints": 3 })");
		CHECK_FALSE(draft.Load(broken, error));
		CHECK(draft.GetPath() == path);
		REQUIRE(draft.GetAsset().Joints.size() == 1);

		JointAssetEdit::SetMaskWeight(draft.GetAsset(), "Spine", 0.25f);
		CHECK(draft.IsDirty());
		// Dragging a value back is no edit.
		JointAssetEdit::SetMaskWeight(draft.GetAsset(), "Spine", 0.5f);
		CHECK_FALSE(draft.IsDirty());
		JointAssetEdit::SetMaskWeight(draft.GetAsset(), "Chest", 1.0f);
		CHECK(draft.GetAsset().Joints.size() == 2);
		CHECK(draft.IsDirty());
		REQUIRE(draft.Validate(error));

		// The text a save writes reads back as the same mask, in the same form a layer loads, and ends its last line.
		CHECK(draft.MakeFileText().ends_with('\n'));
		const auto written = AnimationMask::FromText(draft.MakeFileText(), error);
		REQUIRE_MESSAGE(written, error);
		CHECK(written->Joints == draft.GetAsset().Joints);
		REQUIRE(FileSystem::WriteTextFile(project.GetDirectory() / path, draft.MakeFileText()));
		draft.MarkSaved();
		CHECK_FALSE(draft.IsDirty());

		// Edits the parser would refuse keep the draft but cannot be saved.
		draft.GetAsset().Joints.push_back({ .Joint = "Spine" });
		CHECK_FALSE(draft.Validate(error));
		CHECK(error == "Joints[2].Joint 'Spine' is listed twice");
		draft.GetAsset().Joints.back().Joint = "";
		CHECK_FALSE(draft.Validate(error));
		CHECK(error == "Joints[2].Joint must be a nonempty string");
		REQUIRE(draft.Revert(error));
		CHECK(draft.GetAsset().Joints.size() == 2);
		CHECK(draft.Validate(error));

		draft.OnAssetMoved("Assets/Other.banimmask", "Assets/Elsewhere.banimmask");
		CHECK(draft.GetPath() == path);
		draft.OnAssetMoved(path, "Assets/Moved.banimmask");
		CHECK(draft.GetPath() == "Assets/Moved.banimmask");
		draft.Close();
		CHECK_FALSE(draft.IsOpen());
		CHECK(draft.GetAsset().Joints.empty());
	}

	TEST_CASE("Joint asset drafts follow their file while clean and report a conflict while dirty")
	{
		TempProject project("DraftChanges");
		const std::string path = project.WriteFile("Assets/Rig.bjointmap", R"({ "Joints": [ { "Source": "a", "Target": "b" } ] })");
		AnimationJointMapDraft draft;
		std::string error;
		REQUIRE(draft.Load(path, error));
		// Clean: it follows the new version, and that undoes nothing.
		project.WriteFile(path, R"({ "Joints": [ { "Source": "c", "Target": "d", "Offset": [ 0, 90, 0 ] } ] })");
		REQUIRE(draft.OnFileChanged(error));
		REQUIRE(draft.GetAsset().Joints.size() == 1);
		CHECK(draft.GetAsset().Joints[0].Offset == glm::vec3(0, 90, 0));
		CHECK_FALSE(draft.IsDirty());
		CHECK_FALSE(draft.CanUndo());

		// Dirty: the edits stay, and the conflict shows until a save or a revert.
		REQUIRE_FALSE(draft.GetAsset().Joints.empty());
		draft.GetAsset().Joints[0].Source = "edited";
		project.WriteFile(path, R"({ "Joints": [] })");
		CHECK_FALSE(draft.OnFileChanged(error));
		CHECK(draft.HasExternalChange());
		CHECK(draft.GetAsset().Joints[0].Source == "edited");
		REQUIRE(draft.Revert(error));
		CHECK_FALSE(draft.HasExternalChange());
		CHECK(draft.GetAsset().Joints.empty());

		// A new version that cannot be read is a conflict too, whatever the edits, and a fixed file clears it.
		project.WriteFile(path, "not json");
		error.clear();
		CHECK_FALSE(draft.OnFileChanged(error));
		CHECK_FALSE(error.empty());
		CHECK(draft.HasExternalChange());
		project.WriteFile(path, R"({ "Joints": [ { "Source": "e", "Target": "f" } ] })");
		REQUIRE(draft.OnFileChanged(error));
		CHECK_FALSE(draft.HasExternalChange());
		CHECK(draft.GetAsset().Joints.size() == 1);

		// A dirty save clears the conflict as well.
		draft.GetAsset().Joints[0].Source = "g";
		project.WriteFile(path, R"({ "Joints": [] })");
		CHECK_FALSE(draft.OnFileChanged(error));
		draft.MarkSaved();
		CHECK_FALSE(draft.HasExternalChange());
		CHECK_FALSE(draft.IsDirty());
	}

	TEST_CASE("Joint asset drafts undo finished edits and forget them with a new file")
	{
		TempProject project("DraftUndo");
		const std::string path = project.WriteFile("Assets/Upper.banimmask", R"({ "Joints": [] })");
		AnimationMaskDraft draft;
		std::string error;
		REQUIRE(draft.Load(path, error));
		CHECK_FALSE(draft.CanUndo());
		CHECK_FALSE(draft.Undo());
		CHECK_FALSE(draft.CanRedo());
		const auto names = [&] {
			std::string text;
			for (const AnimationMaskJoint& joint : draft.GetAsset().Joints)
				text += joint.Joint + ";";
			return text;
		};
		JointAssetEdit::SetMaskWeight(draft.GetAsset(), "A", 1);
		draft.Record();
		// Recording the same edits twice is one step.
		draft.Record();
		JointAssetEdit::SetMaskWeight(draft.GetAsset(), "B", 1);
		draft.Record();
		REQUIRE(draft.Undo());
		CHECK(names() == "A;");
		REQUIRE(draft.Undo());
		CHECK(names().empty());
		CHECK_FALSE(draft.Undo());
		REQUIRE(draft.Redo());
		REQUIRE(draft.Redo());
		CHECK(names() == "A;B;");
		CHECK_FALSE(draft.Redo());

		// An edit not yet recorded (a slider still held) is undone too, as its own step.
		JointAssetEdit::RemoveMaskJoint(draft.GetAsset(), "A");
		CHECK(draft.CanUndo());
		REQUIRE(draft.Undo());
		CHECK(names() == "A;B;");

		// A new edit drops the redo steps.
		REQUIRE(draft.Undo());
		CHECK(draft.CanRedo());
		JointAssetEdit::SetMaskWeight(draft.GetAsset(), "C", 1);
		draft.Record();
		CHECK_FALSE(draft.CanRedo());
		// Undoing back to the file is clean again; a reload forgets the history.
		REQUIRE(draft.Revert(error));
		JointAssetEdit::SetMaskWeight(draft.GetAsset(), "D", 1);
		draft.Record();
		REQUIRE(draft.Undo());
		CHECK_FALSE(draft.IsDirty());
		REQUIRE(draft.Revert(error));
		CHECK_FALSE(draft.CanRedo());
		CHECK_FALSE(draft.CanUndo());
	}

	TEST_CASE("Mask edits stop at the mask's limit")
	{
		AnimationMask mask;
		for (uint32_t i = 0; i < AnimationLayer::MaxMaskJoints; i++)
			REQUIRE(JointAssetEdit::SetMaskWeight(mask, "J" + std::to_string(i), 1));
		CHECK_FALSE(JointAssetEdit::SetMaskWeight(mask, "Extra", 1));
		CHECK(mask.Joints.size() == AnimationLayer::MaxMaskJoints);
		// A listed joint still takes a new weight, and removing one that is not listed does nothing.
		CHECK(JointAssetEdit::SetMaskWeight(mask, "J0", 0.5f));
		CHECK(mask.Joints[0].Weight == 0.5f);
		JointAssetEdit::RemoveMaskJoint(mask, "Extra");
		CHECK(mask.Joints.size() == AnimationLayer::MaxMaskJoints);
		JointAssetEdit::RemoveMaskJoint(mask, "J0");
		CHECK(mask.Joints.size() == AnimationLayer::MaxMaskJoints - 1);
		CHECK(JointAssetEdit::SetMaskWeight(mask, "Extra", 1));
	}

	TEST_CASE("Automatic matches become joint map pairs that reproduce them")
	{
		const auto target = LoadModel(FixturePath("UnrealRig.gltf"));
		const auto source = LoadModel(FixturePath("MixamoRig.gltf"));
		AnimationJointMap map;
		map.Joints = { { .Source = "mixamorig:LeftArm", .Target = "upperarm_l", .Offset = { 0, 0, 40 } } };
		std::vector<std::string> warnings;
		const RetargetMap before = Retargeting::BuildMap(*target, *source, map.Joints, warnings);
		REQUIRE(warnings.empty());
		const auto matched = JointAssetEdit::AddMatchedPairs(map, before, *target, *source);
		CHECK(matched.Added == before.MappedCount() - 1);
		CHECK(matched.Skipped() == 0);
		CHECK(map.Joints.size() == before.MappedCount());
		// The pair already there keeps its offset, and the file still parses.
		CHECK(map.Joints[0].Offset == glm::vec3(0, 0, 40));
		std::string error;
		REQUIRE_MESSAGE(AnimationJointMap::FromJson(map.ToJson(), error), error);
		// Nothing was left to chance: the same skeleton mapping, every joint by the map now.
		const RetargetMap after = Retargeting::BuildMap(*target, *source, map.Joints, warnings);
		CHECK(warnings.empty());
		CHECK(after.SourceOf == before.SourceOf);
		CHECK(std::ranges::none_of(after.Matches, [](RetargetMatch match) { return match == RetargetMatch::Name || match == RetargetMatch::Humanoid; }));
		// Running it again finds nothing more.
		CHECK(JointAssetEdit::AddMatchedPairs(map, after, *target, *source).Added == 0);

		// A pair that did not apply leaves its joint to the automatic matching, but listing that match would name the joint
		// twice: the draft stays valid, and says what it left out.
		AnimationJointMap stale;
		stale.Joints = { { .Source = "mixamorig:Hips", .Target = "renamed_away" }, { .Source = "gone", .Target = "pelvis_2" } };
		const RetargetMap partial = Retargeting::BuildMap(*target, *source, stale.Joints, warnings);
		const auto kept = JointAssetEdit::AddMatchedPairs(stale, partial, *target, *source);
		CHECK(kept.PairedElsewhere == 1);
		REQUIRE_MESSAGE(AnimationJointMap::FromJson(stale.ToJson(), error), error);

		// A full map stops adding.
		AnimationJointMap full;
		for (uint32_t i = 0; i + 1 < AnimationClipLibrary::MaxJoints; i++)
			full.Joints.push_back({ .Source = "s" + std::to_string(i), .Target = "t" + std::to_string(i) });
		const RetargetMap automatic = Retargeting::BuildMap(*target, *source, {}, warnings);
		const auto limited = JointAssetEdit::AddMatchedPairs(full, automatic, *target, *source);
		CHECK(limited.Added == 1);
		CHECK(limited.OverLimit == automatic.MappedCount() - 1);
		CHECK(full.Joints.size() == AnimationClipLibrary::MaxJoints);

		// Nodes without a name have nothing in common: they match nothing, so no pair the file would refuse is listed.
		auto nameless = CreateRef<MeshSource>(*target);
		auto namelessLibrary = CreateRef<MeshSource>(*source);
		nameless->Nodes[0].Name.clear();
		namelessLibrary->Nodes[0].Name.clear();
		AnimationJointMap empty;
		const RetargetMap unnamed = Retargeting::BuildMap(*nameless, *namelessLibrary, {}, warnings);
		CHECK(unnamed.SourceOf[0] < 0);
		JointAssetEdit::AddMatchedPairs(empty, unnamed, *nameless, *namelessLibrary);
		REQUIRE_MESSAGE(AnimationJointMap::FromJson(empty.ToJson(), error), error);

		// A second node of a name cannot be a pair: its name stands for the first.
		auto twin = CreateRef<MeshSource>(*target);
		const uint32_t second = AnimationTest::Node(*twin, "lowerarm_l");
		twin->Nodes[second].Name = "upperarm_l";
		AnimationJointMap named;
		RetargetMap forced = Retargeting::BuildMap(*twin, *source, {}, warnings);
		forced.SourceOf[second] = 0;
		forced.Matches[second] = RetargetMatch::Name;
		CHECK(JointAssetEdit::AddMatchedPairs(named, forced, *twin, *source).SharedNames == 1);
		CHECK(std::ranges::count_if(named.Joints, [](const AnimationRetargetJoint& pair) { return pair.Target == "upperarm_l"; }) == 1);
	}

	TEST_CASE("New joint assets take free, safe names, so creating one never replaces a file")
	{
		TempProject project("NewJointAsset");
		const std::filesystem::path directory = Project::ResolvePath("Assets");
		std::string error;
		CHECK(JointAssetEdit::MakeNewAssetPath(directory, "Upper", ".banimmask", error) == "Assets/Upper.banimmask");
		project.WriteFile("Assets/Upper.banimmask", R"({ "Joints": [ { "Joint": "Spine" } ] })");
		CHECK_FALSE(JointAssetEdit::MakeNewAssetPath(directory, "Upper", ".banimmask", error));
		CHECK(error == "'Upper.banimmask' already exists");
		// Another kind of asset may share the name.
		CHECK(JointAssetEdit::MakeNewAssetPath(directory, "Upper", ".bjointmap", error) == "Assets/Upper.bjointmap");
		for (const char* bad : { "", "a/b", "a\\b", "a.b", "a:b", "NUL", "con", "a*b" })
		{
			CAPTURE(bad);
			CHECK_FALSE(JointAssetEdit::MakeNewAssetPath(directory, bad, ".bjointmap", error));
			CHECK(error.find("is not a safe file name") != std::string::npos);
		}
	}

	TEST_CASE("Joint asset windows find their models from the Animators that use the file, else the selection")
	{
		Scene scene;
		auto plain = scene.CreateEntity("Plain");
		auto selected = scene.CreateEntity("Selected");
		auto& picked = selected.AddComponent<AnimatorComponent>();
		picked.Mesh = "Assets/Selected.gltf";
		picked.ClipLibraries = { { .Mesh = "Assets/SelectedLibrary.gltf" } };
		auto other = scene.CreateEntity("Other");
		auto& animator = other.AddComponent<AnimatorComponent>();
		animator.Mesh = "Assets/Hero.gltf";
		animator.Layers = { { .Name = "First" }, { .Name = "Upper", .MaskAsset = "Assets/Upper.banimmask" } };
		animator.ClipLibraries = { { .Mesh = "Assets/First.gltf", .JointMap = "Assets/First.bjointmap" }, { .Mesh = "Assets/Mixamo.gltf", .JointMap = "Assets/Rig.bjointmap" } };

		// What uses the file comes first, whatever is selected.
		CHECK(JointAssetEdit::FindMaskModel(scene, "Assets/Upper.banimmask", selected.GetUUID()) == "Assets/Hero.gltf");
		const auto used = JointAssetEdit::FindJointMapModels(scene, "Assets/Rig.bjointmap", selected.GetUUID());
		CHECK(used.Source == "Assets/Mixamo.gltf");
		CHECK(used.Target == "Assets/Hero.gltf");
		// Else the selection, which may have no library or no Animator.
		CHECK(JointAssetEdit::FindMaskModel(scene, "Assets/New.banimmask", selected.GetUUID()) == "Assets/Selected.gltf");
		const auto fallback = JointAssetEdit::FindJointMapModels(scene, "Assets/New.bjointmap", selected.GetUUID());
		CHECK(fallback.Source == "Assets/SelectedLibrary.gltf");
		CHECK(fallback.Target == "Assets/Selected.gltf");
		CHECK(JointAssetEdit::FindMaskModel(scene, "Assets/New.banimmask", plain.GetUUID()).empty());
		CHECK(JointAssetEdit::FindJointMapModels(scene, "Assets/New.bjointmap", plain.GetUUID()).Target.empty());
		CHECK(JointAssetEdit::FindMaskModel(scene, "Assets/New.banimmask", 0).empty());
		picked.ClipLibraries.clear();
		CHECK(JointAssetEdit::FindJointMapModels(scene, "Assets/New.bjointmap", selected.GetUUID()).Source.empty());
	}
}

TEST_SUITE("Editor")
{
	TEST_CASE("An unrecorded edit is a new edit, which Redo must not overwrite")
	{
		TempProject project("DraftRedo");
		const std::string path = project.WriteFile("Assets/Upper.banimmask", R"({ "Joints": [] })");
		AnimationMaskDraft draft;
		std::string error;
		REQUIRE(draft.Load(path, error));
		JointAssetEdit::SetMaskWeight(draft.GetAsset(), "A", 1);
		draft.Record();
		REQUIRE(draft.Undo());
		JointAssetEdit::SetMaskWeight(draft.GetAsset(), "B", 1);
		CHECK_FALSE(draft.Redo());
		REQUIRE(draft.GetAsset().Joints.size() == 1);
		CHECK(draft.GetAsset().Joints[0].Joint == "B");
	}

	TEST_CASE("The undo steps drop the oldest edits, and the newest are all there")
	{
		TempProject project("DraftLimit");
		const std::string path = project.WriteFile("Assets/Upper.banimmask", R"({ "Joints": [] })");
		AnimationMaskDraft draft;
		std::string error;
		REQUIRE(draft.Load(path, error));
		for (int i = 1; i <= 100; i++)
		{
			JointAssetEdit::SetMaskWeight(draft.GetAsset(), "A", static_cast<float>(i));
			draft.Record();
		}
		for (int i = 99; i > 37; i--)
		{
			REQUIRE(draft.Undo());
			CHECK(draft.GetAsset().Joints[0].Weight == static_cast<float>(i));
		}
		CHECK(draft.Undo());
		CHECK(draft.GetAsset().Joints[0].Weight == 37.0f);
		CHECK_FALSE(draft.Undo());
	}

	TEST_CASE("A save ends the undo step, so the next edit undoes back to what was saved")
	{
		TempProject project("DraftSave");
		const std::string path = project.WriteFile("Assets/Upper.banimmask", R"({ "Joints": [] })");
		AnimationMaskDraft draft;
		std::string error;
		REQUIRE(draft.Load(path, error));
		JointAssetEdit::SetMaskWeight(draft.GetAsset(), "A", 1);
		draft.MarkSaved();
		JointAssetEdit::SetMaskWeight(draft.GetAsset(), "B", 1);
		REQUIRE(draft.Undo());
		CHECK_FALSE(draft.IsDirty());
		CHECK(draft.GetAsset().Joints.size() == 1);
	}

	TEST_CASE("A held slider's first edit can be undone")
	{
		TempProject project("DraftCanUndo");
		const std::string path = project.WriteFile("Assets/Upper.banimmask", R"({ "Joints": [] })");
		AnimationMaskDraft draft;
		std::string error;
		REQUIRE(draft.Load(path, error));
		JointAssetEdit::SetMaskWeight(draft.GetAsset(), "A", 1);
		CHECK(draft.CanUndo());
	}

	TEST_CASE("A second source node of a name cannot be a pair either")
	{
		const auto target = LoadModel(FixturePath("UnrealRig.gltf"));
		const auto source = LoadModel(FixturePath("MixamoRig.gltf"));
		auto twin = CreateRef<MeshSource>(*source);
		const uint32_t second = AnimationTest::Node(*twin, "mixamorig:LeftForeArm");
		twin->Nodes[second].Name = "mixamorig:LeftArm";
		std::vector<std::string> warnings;
		const RetargetMap forced = Retargeting::BuildMap(*target, *twin, {}, warnings);
		RetargetMap map = forced;
		const uint32_t node = AnimationTest::Node(*target, "lowerarm_l");
		map.SourceOf[node] = static_cast<int32_t>(second);
		map.Matches[node] = RetargetMatch::Name;
		AnimationJointMap named;
		JointAssetEdit::AddMatchedPairs(named, map, *target, *twin);
		CHECK(std::ranges::none_of(named.Joints, [](const AnimationRetargetJoint& pair) { return pair.Target == "lowerarm_l"; }));
	}
}
