#include <doctest/doctest.h>

#include "AnimationTestUtils.h"
#include "AutomationTestUtils.h"
#include "TestUtils.h"

#include <Basalt/Animation/AnimatedModel.h>
#include <Basalt/Animation/AnimationMask.h>
#include <Basalt/Animation/AnimationPose.h>
#include <Basalt/Animation/AnimationSystem.h>
#include <Basalt/Animation/FadingWeight.h>
#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Asset/MeshImporter.h>
#include <Basalt/Core/JsonUtils.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Project/ProjectValidator.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/SceneSerializer.h>
#include <Basalt/Scripting/ScriptEngine.h>

using namespace Basalt;
using namespace BasaltTest;
using json = nlohmann::json;

using namespace AnimationTest;

namespace {
	AnimationLocalPose Sample(const MeshSource& source, const std::string& clip, float time)
	{
		return SampleAnimation(source, clip.empty() ? nullptr : &Clip(source, clip.c_str()), time);
	}
	std::vector<glm::mat4> Compose(const MeshSource& source, const AnimationLocalPose& pose)
	{
		std::vector<glm::mat4> globals;
		REQUIRE(ComposeAnimationPose(source, pose, globals));
		return globals;
	}
	constexpr glm::vec3 X(1, 0, 0), Z(0, 0, 1);

	Entity AddBiped(Scene& scene, const std::string& clip = "Walk")
	{
		auto entity = scene.CreateEntity("Biped");
		auto& animator = entity.AddComponent<AnimatorComponent>();
		animator.Mesh = BipedPath().string();
		animator.Clip = clip;
		return entity;
	}
	AnimationLayer Layer(const std::string& name, const std::string& clip, AnimationLayerBlend blend = AnimationLayerBlend::Override, std::vector<AnimationMaskJoint> mask = {})
	{
		AnimationLayer layer;
		layer.Name = name;
		layer.Clip = clip;
		layer.Blend = blend;
		layer.Mask = std::move(mask);
		return layer;
	}
	// An upper-body graph: Lean until Waving, then Wave (instant transitions unless `fade`), with a Nod trigger back.
	json UpperBodyGraph(float fade = 0)
	{
		return {
			{ "InitialState", "Lean" },
			{ "Parameters", json::array({ { { "Name", "Waving" }, { "Type", "Bool" } }, { { "Name", "Nod" }, { "Type", "Trigger" } } }) },
			{ "States", json::array({ { { "Name", "Lean" }, { "Clip", "Lean" } }, { { "Name", "Wave" }, { "Clip", "Wave" } } }) },
			{ "Transitions", json::array({ { { "From", "Lean" }, { "To", "Wave" }, { "Duration", fade }, { "Conditions", json::array({ { { "Parameter", "Waving" }, { "Value", true } } }) } },
										   { { "From", "Wave" }, { "To", "Lean" }, { "Duration", 0 }, { "Conditions", json::array({ { { "Parameter", "Nod" } } }) } } }) },
		};
	}
	AnimationLayer GraphLayer(const std::string& name, const std::string& graph, std::vector<AnimationMaskJoint> mask = {})
	{
		AnimationLayer layer = Layer(name, "", AnimationLayerBlend::Override, std::move(mask));
		layer.StateMachine = graph;
		return layer;
	}
	// The local rotation of `joint` in the Animator's current pose.
	glm::quat LocalRotation(AnimationSystem& system, Entity entity, const MeshSource& source, const std::string& joint)
	{
		const auto& globals = *system.GetPose(entity);
		const uint32_t node = Node(source, joint);
		const glm::mat4 parent = source.Nodes[node].Parent >= 0 ? globals[source.Nodes[node].Parent] : glm::mat4(1);
		return glm::normalize(glm::quat_cast(glm::mat3(glm::inverse(parent) * globals[node])));
	}
}

TEST_SUITE("Animation")
{
	TEST_CASE("Layer masks weight subtrees let deeper entries win and report unknown joints")
	{
		const auto source = LoadBiped();
		std::vector<std::string> missing;
		const auto all = AnimationMaskWeights(*source, {}, missing);
		CHECK(std::ranges::all_of(all, [](float weight) { return weight == 1.0f; }));
		CHECK(missing.empty());

		// An entry without a Joint (as the inspector adds one) is ignored.
		const auto weights = AnimationMaskWeights(*source, { { "Chest", 1.0f }, { "LeftUpperArm", 0.5f }, { "RightForeArm", 0.0f }, { "Missing", 1.0f }, { "", 1.0f } }, missing);
		CHECK(missing == std::vector<std::string>{ "Missing" });
		CHECK(weights[Node(*source, "Hips")] == 0.0f);
		CHECK(weights[Node(*source, "Spine")] == 0.0f);
		CHECK(weights[Node(*source, "LeftFoot")] == 0.0f);
		CHECK(weights[Node(*source, "Chest")] == 1.0f);
		CHECK(weights[Node(*source, "Head")] == 1.0f);
		CHECK(weights[Node(*source, "LeftUpperArm")] == 0.5f);
		CHECK(weights[Node(*source, "LeftHand")] == 0.5f);
		CHECK(weights[Node(*source, "RightUpperArm")] == 1.0f);
		CHECK(weights[Node(*source, "RightForeArm")] == 0.0f);
		CHECK(weights[Node(*source, "RightHand")] == 0.0f);
	}

	TEST_CASE("Override layers replace only the masked channels their clip animates")
	{
		ClearAssetCache clear;
		const auto source = LoadBiped();
		Scene scene;
		auto biped = AddBiped(scene);
		biped.GetComponent<AnimatorComponent>().Layers = { Layer("Wave", "Wave", AnimationLayerBlend::Override, { { "RightUpperArm", 1.0f }, { "RightForeArm", 0.0f } }) };
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();

		// The base keeps the legs and hips (Walk); the layer turns only the upper arm (Wave), not the forearm.
		AnimationLocalPose expected = Sample(*source, "Walk", 0);
		expected.Nodes[Node(*source, "RightUpperArm")].Rotation = AxisAngle(Z, -45);
		CHECK(NearPose(*system.GetPose(biped), Compose(*source, expected)));

		// Weight 0.5 goes half way from the base (the rest rotation here).
		biped.GetComponent<AnimatorComponent>().Layers[0].Weight = 0.5f;
		system.Update(0);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), AxisAngle(Z, -22.5f)));
		CHECK(system.GetLayerWeight(biped, "Wave") == doctest::Approx(0.5f));

		// Without a mask the forearm follows too; legs keep the base, since Wave does not animate them.
		biped.GetComponent<AnimatorComponent>().Layers[0] = Layer("Wave", "Wave");
		system.Update(0.5f);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightForeArm"), AxisAngle(Z, -30)));
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), AxisAngle(Z, -90)));
		CHECK(NearRotation(LocalRotation(system, biped, *source, "LeftUpperLeg"), AxisAngle(X, -20)));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Additive layers add their clip's difference from the reference pose")
	{
		ClearAssetCache clear;
		const auto source = LoadBiped();
		Scene scene;
		auto biped = AddBiped(scene, "Lean");
		auto& layers = biped.GetComponent<AnimatorComponent>().Layers;
		layers = { Layer("Breathe", "Breathe", AnimationLayerBlend::Additive) };
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();

		// At the reference frame the delta vanishes: the chest keeps the base (rest) rotation, not Breathe's 5 degrees.
		CHECK(NearRotation(LocalRotation(system, biped, *source, "Chest"), glm::quat(1, 0, 0, 0)));
		system.Update(0.5f);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "Chest"), AxisAngle(X, 10)));
		// The base's own motion stays.
		CHECK(NearRotation(LocalRotation(system, biped, *source, "Spine"), AxisAngle(Z, 20)));

		layers[0].Weight = 0.5f;
		system.Update(0);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "Chest"), AxisAngle(X, 5)));

		// A reference later in the clip: at 0.5 s (15 degrees) the frame at 0 s is 10 degrees below it.
		layers[0] = Layer("Breathe", "Breathe", AnimationLayerBlend::Additive);
		layers[0].ReferenceTime = 0.5f;
		system.Update(0);
		CHECK(system.PlayLayer(biped, "Breathe", std::nullopt));
		CHECK(NearRotation(LocalRotation(system, biped, *source, "Chest"), AxisAngle(X, -10)));
		// A reference clip that does not animate the chest compares against its rest rotation; a time past its end clamps.
		layers[0].ReferenceClip = "Lean";
		layers[0].ReferenceTime = 50;
		system.Update(0);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "Chest"), AxisAngle(X, 5)));

		// A missing reference clip disables the layer and is reported once.
		const auto since = Log::GetHistory().GetTotalCount();
		layers[0].ReferenceClip = "Missing";
		system.Update(Step);
		system.Update(Step);
		CHECK(CountLogMessages(since, "reference clip 'Missing' not found") == 1);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "Chest"), glm::quat(1, 0, 0, 0)));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Layer weights fade linearly and configured edits apply at once")
	{
		ClearAssetCache clear;
		Scene scene;
		auto biped = AddBiped(scene);
		auto& layers = biped.GetComponent<AnimatorComponent>().Layers;
		layers = { Layer("Wave", "Wave"), Layer("Lean", "Lean") };
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();

		CHECK(system.SetLayerWeight(biped, "Wave", 0, 1));
		CHECK(system.GetLayerWeight(biped, "Wave") == doctest::Approx(1.0f));
		system.Update(0.25f);
		CHECK(system.GetLayerWeight(biped, "Wave") == doctest::Approx(0.75f));
		system.Update(0.5f);
		CHECK(system.GetLayerWeight(biped, "Wave") == doctest::Approx(0.25f));
		system.Update(0.5f);
		CHECK(system.GetLayerWeight(biped, "Wave") == doctest::Approx(0.0f));
		// Zero seconds sets the weight at once.
		CHECK(system.SetLayerWeight(biped, "Wave", 0.6f, 0));
		CHECK(system.GetLayerWeight(biped, "Wave") == doctest::Approx(0.6f));

		// Editing the configured Weight replaces the runtime weight and ends a fade.
		CHECK(system.SetLayerWeight(biped, "Wave", 0, 10));
		layers[0].Weight = 0.25f;
		system.Update(1);
		CHECK(system.GetLayerWeight(biped, "Wave") == doctest::Approx(0.25f));

		const auto since = Log::GetHistory().GetTotalCount();
		// A failing call logs once however often a script repeats it, with whatever bad values, also between good calls
		// on the same and other layers.
		for (int i = 0; i < 3; i++)
		{
			CHECK_FALSE(system.SetLayerWeight(biped, "Wave", 1.5f + static_cast<float>(i), 0));
			CHECK(system.GetLayerWeight(biped, "Wave").has_value());
			CHECK(system.GetLayerWeight(biped, "Lean").has_value());
			// Another call on the same layer succeeding does not end this call's problem.
			CHECK(system.PlayLayer(biped, "Wave", std::nullopt));
		}
		CHECK_FALSE(system.SetLayerWeight(biped, "Wave", 1, -1));
		CHECK_FALSE(system.SetLayerWeight(biped, "Wave", std::numeric_limits<float>::quiet_NaN(), 0));
		CHECK(CountLogMessages(since, "layer 'Wave': the weight must be between 0 and 1") == 1);
		CHECK(CountLogMessages(since, "(got 1.5 and 0)") == 1);
		// A success of the same call ends the problem: it logs again when it comes back.
		CHECK(system.SetLayerWeight(biped, "Wave", 0.25f, 0));
		CHECK_FALSE(system.SetLayerWeight(biped, "Wave", 2, 0));
		CHECK(CountLogMessages(since, "layer 'Wave': the weight must be between 0 and 1") == 2);
		CHECK(system.GetLayerWeight(biped, "Wave") == doctest::Approx(0.25f));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Layers play on while the base holds and freeze while the Animator is paused")
	{
		ClearAssetCache clear;
		const auto source = LoadBiped();
		Scene scene;
		auto biped = AddBiped(scene);
		biped.GetComponent<AnimatorComponent>().Layers = { Layer("Wave", "Wave") };
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();

		// Stopped: the base shows the rest pose, the layer keeps its clock.
		system.Stop(biped);
		system.Update(0.5f);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), AxisAngle(Z, -90)));
		CHECK(NearRotation(LocalRotation(system, biped, *source, "LeftUpperLeg"), glm::quat(1, 0, 0, 0)));

		// Paused: everything holds.
		system.Pause(biped, true);
		system.Update(0.25f);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), AxisAngle(Z, -90)));
		system.Pause(biped, false);
		system.Update(0.25f);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), AxisAngle(Z, -67.5f)));

		// A non-looping layer holds its last frame.
		auto& layer = biped.GetComponent<AnimatorComponent>().Layers[0];
		layer.Looping = false;
		layer.Speed = 2;
		CHECK(system.PlayLayer(biped, "Wave", std::nullopt));
		system.Update(0.75f);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), AxisAngle(Z, -45)));
		scene.OnRuntimeStop();
	}

	TEST_CASE("PlayAnimationLayer restarts on a script clip until the configured clip changes")
	{
		ClearAssetCache clear;
		const auto source = LoadBiped();
		Scene scene;
		auto biped = AddBiped(scene);
		auto& layers = biped.GetComponent<AnimatorComponent>().Layers;
		layers = { Layer("Upper", "") };
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();

		// An empty Clip shows nothing.
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), glm::quat(1, 0, 0, 0)));
		CHECK(system.PlayLayer(biped, "Upper", std::string("Wave")));
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), AxisAngle(Z, -45)));
		system.Update(0.5f);
		CHECK(system.PlayLayer(biped, "Upper", std::string("Wave")));
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), AxisAngle(Z, -45)));

		const auto since = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(system.PlayLayer(biped, "Upper", std::string("Missing")));
		CHECK_FALSE(system.PlayLayer(biped, "Upper", std::string("Missing")));
		CHECK_FALSE(system.PlayLayer(biped, "Upper", std::string()));
		CHECK_FALSE(system.PlayLayer(biped, "Lower", std::nullopt));
		CHECK_FALSE(system.GetLayerWeight(biped, "Lower").has_value());
		CHECK(CountLogMessages(since, "clip 'Missing' not found") == 1);
		// Once per operation: PlayAnimationLayer and GetAnimationLayerWeight.
		CHECK_FALSE(system.PlayLayer(biped, "Lower", std::nullopt));
		CHECK(CountLogMessages(since, "no layer named 'Lower'") == 2);
		// The failed calls kept the script clip.
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), AxisAngle(Z, -45)));

		// The configured Clip changing takes over again (from the start).
		layers[0].Clip = "Lean";
		system.Update(0);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), glm::quat(1, 0, 0, 0)));
		CHECK(NearRotation(LocalRotation(system, biped, *source, "Spine"), AxisAngle(Z, 20)));
		// Removing the layer removes its effect.
		layers.clear();
		system.Update(0);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "Spine"), glm::quat(1, 0, 0, 0)));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Root motion keeps its extraction node out of layers")
	{
		ClearAssetCache clear;
		const auto source = LoadBiped();
		for (bool rootMotion : { false, true })
		{
			CAPTURE(rootMotion);
			Scene scene;
			auto biped = AddBiped(scene, "Lean");
			auto& animator = biped.GetComponent<AnimatorComponent>();
			animator.RootMotion = rootMotion;
			animator.RootJoint = "Hips";
			animator.Layers = { Layer("Walk", "Walk") };
			scene.OnRuntimeStart();
			auto& system = *scene.GetAnimationSystem();
			system.Update(0.125f);
			// Walk bobs the hips to 1.005 m at 0.125 s; root motion leaves the base's hips in place, and the layer's legs
			// (10 degrees, not the base's rest) either way.
			const auto& globals = *system.GetPose(biped);
			CHECK(globals[Node(*source, "Hips")][3].y == doctest::Approx(rootMotion ? 1.0f : 1.005f));
			CHECK(NearRotation(LocalRotation(system, biped, *source, "LeftUpperLeg"), AxisAngle(X, 10)));
			scene.OnRuntimeStop();
		}
	}

	TEST_CASE("Layer and mask validation names the field")
	{
		Scene scene;
		auto entity = scene.CreateEntity("Rig");
		std::string error;
		const std::pair<json, const char*> cases[] = {
			{ { { "Layers", { { { "Name", "" } } } } }, "Layers[0].Name must not be empty" },
			{ { { "Layers", { { { "Name", "A" } }, { { "Name", "A" } } } } }, "Layers[1].Name 'A' is already used by Layers[0]" },
			{ { { "Layers", { { { "Weight", 1.5 } } } } }, "Layers[0].Weight 1.5 must be between 0 and 1" },
			{ { { "Layers", { { { "Speed", -1 } } } } }, "Layers[0].Speed -1 must be finite and between 0 and 100" },
			{ { { "Layers", { { { "ReferenceTime", -1 } } } } }, "Layers[0].ReferenceTime -1 must be finite and nonnegative" },
			{ { { "Layers", { { { "Mask", { { { "Joint", "A" }, { "Weight", 2 } } } } } } } }, "Layers[0].Mask[0].Weight 2 must be between 0 and 1" },
			{ { { "Layers", { { { "Blend", "Multiply" } } } } }, "Blend" },
			{ { { "Layers", { { { "Unknown", 1 } } } } }, "Unknown" },
		};
		for (const auto& [data, expected] : cases)
		{
			CAPTURE(data.dump());
			CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Animator", data, error));
			CHECK_MESSAGE(error.find(expected) != std::string::npos, error);
		}
		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Animator", { { "Layers", { { { "Mask", { { { "Joint", "A" } }, { { "Joint", "A" } } } } } } } }, error));
		CHECK(error.find("Layers[0].Mask[1].Joint 'A' is already used by Mask[0]") != std::string::npos);
		// Unnamed entries (the inspector's additions) may repeat.
		CHECK(ComponentRegistry::AddOrPatch(entity, "Animator", { { "Layers", { { { "Mask", { { { "Joint", "" } }, { { "Joint", "" } } } } } } } }, error));
		json many = json::array();
		for (uint32_t i = 0; i <= AnimatorComponent::MaxLayers; i++)
			many.push_back({ { "Name", "L" + std::to_string(i) } });
		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Animator", { { "Layers", many } }, error));

		json mask = json::array();
		for (uint32_t i = 0; i <= AnimationLayer::MaxMaskJoints; i++)
			mask.push_back({ { "Joint", "J" + std::to_string(i) } });
		CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, "Animator", { { "Layers", { { { "Mask", mask } } } } }, error));

		REQUIRE(ComponentRegistry::AddOrPatch(entity, "Animator", { { "Layers", { { { "Name", "Breath" }, { "Clip", "Breathe" }, { "Blend", "Additive" }, { "Mask", { { { "Joint", "Chest" }, { "Weight", 0.5 } } } } } } } }, error));
		const auto& layer = entity.GetComponent<AnimatorComponent>().Layers.at(0);
		CHECK(layer.Blend == AnimationLayerBlend::Additive);
		CHECK(layer.Mask.at(0).Weight == 0.5f);
		CHECK(ComponentRegistry::Find("Animator")->Serialize(entity)["Layers"][0]["Blend"] == "Additive");
	}

	TEST_CASE("Lua plays fades and reads animation layers")
	{
		ClearAssetCache clear;
		Scene scene;
		auto biped = AddBiped(scene);
		biped.GetComponent<AnimatorComponent>().Layers = { Layer("Wave", "Wave") };
		scene.CreateEntity("Plain");
		scene.OnRuntimeStart();
		const auto run = [&](const std::string& code) { return Eval(scene, "local e = Scene.FindEntityByName('Biped'); " + code); };
		CHECK(run("return tostring(e:PlayAnimationLayer('Wave', 'Lean'))") == "true");
		CHECK(run("return tostring(e:PlayAnimationLayer('Wave'))") == "true");
		CHECK(run("return tostring(e:SetAnimationLayerWeight('Wave', 0.5))") == "true");
		CHECK(run("return tostring(e:GetAnimationLayerWeight('Wave'))") == "0.5");
		CHECK(run("return tostring(e:SetAnimationLayerWeight('Wave', 1, 2))") == "true");
		CHECK(run("return tostring(e:GetAnimationLayerWeight('Missing'))") == "nil");
		CHECK(run("return tostring(e:PlayAnimationLayer('Missing'))") == "false");
		const std::pair<const char*, const char*> errors[] = {
			{ "Scene.FindEntityByName('Biped'):PlayAnimationLayer('Wave', 5)", "clip must be a string or nil" },
			{ "Scene.FindEntityByName('Biped'):SetAnimationLayerWeight('Wave', 1, 'slow')", "seconds must be a number or nil" },
			{ "Scene.FindEntityByName('Plain'):PlayAnimationLayer('Wave')", "needs the Animator component" },
			{ "Scene.FindEntityByName('Plain'):GetAnimationLayerWeight('Wave')", "needs the Animator component" },
		};
		CheckLuaErrors(*scene.GetScriptEngine(), errors);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Layer blends move only masked animated channels in parent space")
	{
		AnimationLocalPose target, layer, reference;
		target.Nodes.resize(2);
		target.Nodes[0] = { { 1, 0, 0 }, AxisAngle({ 0, 1, 0 }, 90), { 2, 2, 2 } };
		target.Nodes[1] = target.Nodes[0];
		layer = target;
		reference = target;
		layer.Nodes[0] = { { 3, 0, 0 }, AxisAngle({ 1, 0, 0 }, 40) * AxisAngle({ 0, 1, 0 }, 30), { 3, 1, 0 } };
		reference.Nodes[0] = { { 2, 0, 0 }, AxisAngle({ 0, 1, 0 }, 30), { 1, 1, 0 } };
		const AnimationLayerMask mask{ .Weights = { 1, 1 }, .Channels = { AnimatedChannel::Translation | AnimatedChannel::Rotation | AnimatedChannel::Scale, 0 } };

		AnimationLocalPose added = target;
		AddAnimationPose(added, layer, reference, mask, 0.5f);
		// Half the 1 m offset; half the 3x scale ratio on X; a zero reference scale keeps the target's Z.
		CHECK(added.Nodes[0].Translation == glm::vec3(1.5f, 0, 0));
		CHECK(added.Nodes[0].Scale == glm::vec3(4, 2, 2));
		// The 40 degree X delta (layer * inverse(reference)) turns in the parent's frame, on the left of the target.
		CHECK(NearRotation(added.Nodes[0].Rotation, AxisAngle({ 1, 0, 0 }, 20) * AxisAngle({ 0, 1, 0 }, 90)));
		// A node whose channels the clip does not animate keeps the target.
		CHECK(added.Nodes[1].Translation == target.Nodes[1].Translation);

		AnimationLocalPose overridden = target;
		OverrideAnimationPose(overridden, layer, { .Weights = { 0.5f, 1 }, .Channels = { AnimatedChannel::Scale, AnimatedChannel::Rotation } }, 1);
		CHECK(overridden.Nodes[0].Scale == glm::vec3(2.5f, 1.5f, 1));
		CHECK(overridden.Nodes[0].Translation == target.Nodes[0].Translation);
		CHECK(NearRotation(overridden.Nodes[0].Rotation, target.Nodes[0].Rotation));
	}

	TEST_CASE("Layers stack in list order")
	{
		ClearAssetCache clear;
		const auto source = LoadBiped();
		Scene scene;
		auto biped = AddBiped(scene, "Lean");
		auto& layers = biped.GetComponent<AnimatorComponent>().Layers;
		// Two overrides of the same arm: the later one wins.
		AnimationLayer first = Layer("First", "Wave");
		AnimationLayer second = Layer("Second", "Lean");
		second.Clip = "Wave";
		layers = { first, second };
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.PlayLayer(biped, "Second", std::string("Wave")));
		system.Update(0.25f);
		REQUIRE(system.PlayLayer(biped, "Second", std::string("Wave")));
		// First is at 0.25 s (-67.5 degrees), Second restarted at 0 (-45): the later layer shows.
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), AxisAngle(Z, -45)));
		// An additive layer after an override adds to it: Lean adds its 20 degrees to the spine on top.
		layers = { Layer("Wave", "Wave"), Layer("Lean", "Lean", AnimationLayerBlend::Additive) };
		layers[1].ReferenceClip = "Walk";
		system.Update(0);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "Spine"), AxisAngle(Z, 40)));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Layer fades rise too, renamed layers start fresh, and Animator Speed does not drive layers")
	{
		ClearAssetCache clear;
		const auto source = LoadBiped();
		Scene scene;
		auto biped = AddBiped(scene);
		auto& animator = biped.GetComponent<AnimatorComponent>();
		animator.Layers = { Layer("Wave", "Wave") };
		animator.Layers[0].Weight = 0;
		animator.Speed = 0;
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.SetLayerWeight(biped, "Wave", 1, 0.5f));
		system.Update(0.25f);
		CHECK(system.GetLayerWeight(biped, "Wave") == doctest::Approx(0.5f));
		// The base is frozen (Speed 0) but the layer clock ran: Wave at 0.25 s.
		system.SetLayerWeight(biped, "Wave", 1, 0);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), AxisAngle(Z, -67.5f)));
		CHECK(NearRotation(LocalRotation(system, biped, *source, "LeftUpperLeg"), AxisAngle(X, 20)));
		// A renamed layer is a new one: configured weight, time 0.
		animator.Layers[0].Name = "Greeting";
		system.Update(0);
		CHECK(system.GetLayerWeight(biped, "Greeting") == doctest::Approx(0.0f));
		CHECK_FALSE(system.GetLayerWeight(biped, "Wave").has_value());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Layer problems are reported once and layers recover when fixed")
	{
		ClearAssetCache clear;
		const auto source = LoadBiped();
		Scene scene;
		auto biped = AddBiped(scene);
		auto& layers = biped.GetComponent<AnimatorComponent>().Layers;
		layers = { Layer("Wave", "Waving", AnimationLayerBlend::Override, { { "RightArm", 1.0f } }) };
		const auto since = Log::GetHistory().GetTotalCount();
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		for (int i = 0; i < 3; i++)
			system.Update(Step);
		// Both problems in one message, once.
		CHECK(CountLogMessages(since, "layer 'Wave': clip 'Waving' not found") == 1);
		CHECK(CountLogMessages(since, "mask joint RightArm not found") == 1);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), glm::quat(1, 0, 0, 0)));
		// No clip shows nothing, so PlayAnimationLayer says so.
		CHECK_FALSE(system.PlayLayer(biped, "Wave", std::nullopt));
		CHECK(CountLogMessages(since, "layer 'Wave' shows nothing with this clip") == 1);
		layers[0].Clip = "Wave";
		layers[0].Mask[0].Joint = "RightUpperArm";
		system.Update(0);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), AxisAngle(Z, -45)));
		// A layer without a Clip cannot play without a script clip either.
		layers[0].Clip.clear();
		system.Update(0);
		CHECK_FALSE(system.PlayLayer(biped, "Wave", std::nullopt));
		CHECK(CountLogMessages(since, "layer 'Wave' has no Clip; pass one to PlayAnimationLayer") == 1);
		// C++ can store out-of-range values: clamped, with one warning.
		layers[0].Clip = "Wave";
		layers[0].Weight = 3;
		system.Update(Step);
		system.Update(Step);
		CHECK(system.GetLayerWeight(biped, "Wave") == doctest::Approx(1.0f));
		CHECK(CountLogMessages(since, "layer 'Wave': Weight must be between 0 and 1; clamping") == 1);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Root motion keeps the nodes above its extraction node out of layers too")
	{
		ClearAssetCache clear;
		const auto source = LoadBiped();
		Scene scene;
		auto biped = AddBiped(scene, "Lean");
		auto& animator = biped.GetComponent<AnimatorComponent>();
		animator.RootMotion = true;
		animator.RootJoint = "Spine";
		animator.Layers = { Layer("Walk", "Walk") };
		const auto since = Log::GetHistory().GetTotalCount();
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		system.Update(0.125f);
		// Walk bobs the hips, above the root joint Spine: the layer leaves them alone, and says why; the legs, beside
		// Spine, still follow the layer.
		CHECK((*system.GetPose(biped))[Node(*source, "Hips")][3].y == doctest::Approx(1.0f));
		CHECK(NearRotation(LocalRotation(system, biped, *source, "LeftUpperLeg"), AxisAngle(X, 10)));
		CHECK(CountLogMessages(since, "animates the root-motion joint 'Spine' or a node above it") == 1);
		// Turning root motion off gives the hips back to the layer (it bobs them); back on, the warning is not repeated.
		animator.RootMotion = false;
		system.Update(0);
		CHECK((*system.GetPose(biped))[Node(*source, "Hips")][3].y == doctest::Approx(1.005f));
		animator.RootMotion = true;
		system.Update(0);
		CHECK((*system.GetPose(biped))[Node(*source, "Hips")][3].y == doctest::Approx(1.0f));
		CHECK(CountLogMessages(since, "animates the root-motion joint 'Spine' or a node above it") == 1);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Layer state resets with the playback")
	{
		ClearAssetCache clear;
		Scene scene;
		auto biped = AddBiped(scene);
		biped.GetComponent<AnimatorComponent>().Layers = { Layer("Wave", "Wave") };
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.SetLayerWeight(biped, "Wave", 0.3f, 0));
		biped.GetComponent<AnimatorComponent>().Clip = "Lean";
		system.Update(0);
		CHECK(system.GetLayerWeight(biped, "Wave") == doctest::Approx(1.0f));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Fading weights clamp their inputs, fade from where they are and end at the target")
	{
		FadingWeight weight(2);
		CHECK(weight.Get() == 1);
		weight.FadeTo(0, 2);
		weight.Advance(0.5f);
		CHECK(weight.Get() == doctest::Approx(0.75f));
		// A new fade starts from the current value.
		weight.FadeTo(1, 1);
		weight.Advance(0.5f);
		CHECK(weight.Get() == doctest::Approx(0.875f));
		weight.Advance(10);
		CHECK(weight.Get() == 1);
		CHECK_FALSE(weight.IsFading());
		// No time, a denormal time or a non-finite target sets the value at once.
		weight.FadeTo(0.25f, 0);
		CHECK(weight.Get() == 0.25f);
		weight.FadeTo(0.5f, std::numeric_limits<float>::denorm_min());
		CHECK(weight.Get() == 0.5f);
		weight.FadeTo(NAN, 1);
		weight.Advance(1);
		CHECK(weight.Get() == 0);
		// A changed configuration applies at once and ends a fade; an unchanged one does nothing.
		weight.FadeTo(1, 1);
		weight.Configure(2);
		CHECK(weight.Get() == 0);
		weight.Configure(0.5f);
		CHECK(weight.Get() == 0.5f);
		CHECK_FALSE(weight.IsFading());
		CHECK(IsValidFade(1, 0));
		CHECK_FALSE(IsValidFade(1.5f, 0));
		CHECK_FALSE(IsValidFade(0.5f, -1));
		CHECK_FALSE(IsValidFade(NAN, 0));
	}

	TEST_CASE("Layers react to Blend changes and failed PlayAnimationLayer calls change nothing")
	{
		ClearAssetCache clear;
		const auto source = LoadBiped();
		Scene scene;
		auto biped = AddBiped(scene, "Lean");
		auto& layers = biped.GetComponent<AnimatorComponent>().Layers;
		// Lean on Lean: an override shows Lean's 20 degrees, an additive one adds them again (its reference is Walk's
		// rest spine).
		layers = { Layer("Lean", "Lean") };
		layers[0].ReferenceClip = "Walk";
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		CHECK(NearRotation(LocalRotation(system, biped, *source, "Spine"), AxisAngle(Z, 20)));
		layers[0].Blend = AnimationLayerBlend::Additive;
		system.Update(0);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "Spine"), AxisAngle(Z, 40)));

		// A reference clip that is missing makes the layer show nothing; asking it to play another clip then fails and
		// keeps the layer as it was.
		layers[0].ReferenceClip = "Missing";
		system.Update(0.25f);
		CHECK_FALSE(system.PlayLayer(biped, "Lean", std::string("Wave")));
		layers[0].ReferenceClip = "Walk";
		system.Update(0);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "Spine"), AxisAngle(Z, 40)));
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), glm::quat(1, 0, 0, 0)));
		// A mask of only unnamed entries (the inspector's fresh additions) is still the whole model.
		layers[0].Mask = { { "", 1.0f } };
		system.Update(0);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "Spine"), AxisAngle(Z, 40)));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Layers stay out of root motion while it fades out of a state")
	{
		ClearAssetCache clear;
		TempProject project("LayerRootFade");
		const json graph = {
			{ "InitialState", "Walk" },
			{ "Parameters", json::array({ { { "Name", "Stop" }, { "Type", "Trigger" } } }) },
			{ "States", json::array({ { { "Name", "Walk" }, { "Clip", "Walk" }, { "RootMotion", true } }, { { "Name", "Idle" }, { "Clip", "Lean" } } }) },
			{ "Transitions", json::array({ { { "From", "Walk" }, { "To", "Idle" }, { "Duration", 1 }, { "Conditions", json::array({ { { "Parameter", "Stop" } } }) } } }) },
		};
		const std::string path = project.WriteFile("Assets/Animations/Fade.banimgraph", graph.dump());
		const auto source = LoadBiped();
		// The same rig with and without a layer that bobs the hips at another speed: while the outgoing Walk still
		// extracts root motion the hips must not differ.
		std::array<float, 2> hips{};
		std::array<float, 2> settled{};
		for (int withLayer = 0; withLayer < 2; withLayer++)
		{
			Scene scene;
			auto biped = AddBiped(scene);
			auto& animator = biped.GetComponent<AnimatorComponent>();
			animator.StateMachine = path;
			animator.RootMotion = true;
			animator.RootJoint = "Hips";
			if (withLayer)
			{
				animator.Layers = { Layer("Bob", "Walk") };
				animator.Layers[0].Speed = 3;
			}
			scene.OnRuntimeStart();
			auto& system = *scene.GetAnimationSystem();
			system.Update(0.1f);
			REQUIRE(system.SetTrigger(biped, "Stop"));
			system.Update(0.05f);
			system.Update(0.1f);
			hips[withLayer] = (*system.GetPose(biped))[Node(*source, "Hips")][3].y;
			for (int i = 0; i < 20; i++)
				system.Update(0.1f);
			settled[withLayer] = (*system.GetPose(biped))[Node(*source, "Hips")][3].y;
			scene.OnRuntimeStop();
		}
		CHECK(hips[1] == doctest::Approx(hips[0]));
		// After the fade nothing extracts root motion and the layer moves the hips.
		CHECK(settled[1] != doctest::Approx(settled[0]));
	}

	TEST_CASE("Animation mask files parse strictly and round-trip")
	{
		std::string error;
		const auto mask = AnimationMask::FromJson(json::parse(R"({ "Joints": [ { "Joint": "Spine" }, { "Joint": "Head", "Weight": 0.25 } ] })"), error);
		REQUIRE_MESSAGE(mask, error);
		CHECK(mask->Joints == std::vector<AnimationMaskJoint>{ { "Spine", 1.0f }, { "Head", 0.25f } });
		const auto again = AnimationMask::FromJson(mask->ToJson(), error);
		REQUIRE(again);
		CHECK(again->Joints == mask->Joints);
		CHECK(AnimationMask::IsMaskFile("Assets/Masks/Upper.BANIMMASK"));
		CHECK_FALSE(AnimationMask::IsMaskFile("Assets/Masks/Upper.banimgraph"));

		json tooMany = { { "Joints", json::array() } };
		for (uint32_t i = 0; i <= AnimationLayer::MaxMaskJoints; i++)
			tooMany["Joints"].push_back({ { "Joint", "J" + std::to_string(i) } });
		const std::pair<json, const char*> invalid[] = {
			{ json::array(), "an animation mask must be an object" },
			{ { { "Joints", json::array() }, { "Extra", 1 } }, "animation mask: unknown key 'Extra'" },
			{ json::object(), "Joints must be an array with at most 64 entries" },
			{ tooMany, "Joints must be an array with at most 64 entries" },
			{ { { "Joints", { { { "Weight", 1 } } } } }, "Joints[0].Joint must be a nonempty string" },
			{ { { "Joints", { { { "Joint", "" } } } } }, "Joints[0].Joint must be a nonempty string" },
			{ { { "Joints", { { { "Joint", "A" } }, { { "Joint", "A" } } } } }, "Joints[1].Joint 'A' is listed twice" },
			{ { { "Joints", { { { "Joint", "A" }, { "Weight", 1.5 } } } } }, "Joints[0].Weight must be a number between 0 and 1" },
			{ { { "Joints", { { { "Joint", "A" }, { "Weight", "1" } } } } }, "Joints[0].Weight must be a number between 0 and 1" },
			{ { { "Joints", { { { "Joint", "A" }, { "Side", "L" } } } } }, "Joints[0]: unknown key 'Side'" },
			{ { { "Joints", { 3 } } }, "Joints[0] must be an object" },
		};
		for (const auto& [data, message] : invalid)
		{
			INFO(data.dump());
			CHECK_FALSE(AnimationMask::FromJson(data, error));
			// Whole messages: the parser shares its helpers with joint maps.
			CHECK(error == message);
		}
	}

	TEST_CASE("Mask assets replace the inline mask, reload with their file and hide layers they cannot load")
	{
		ClearAssetCache clear;
		TempProject project("MaskAssets");
		const auto source = LoadBiped();
		const std::string maskPath = project.WriteFile("Assets/Masks/Forearm.banimmask", R"({ "Joints": [ { "Joint": "RightForeArm" } ] })");
		Scene scene;
		auto biped = AddBiped(scene);
		// The inline mask names the upper arm; the asset (only the forearm) replaces it.
		AnimationLayer wave = Layer("Wave", "Wave", AnimationLayerBlend::Override, { { "RightUpperArm", 1.0f } });
		wave.MaskAsset = maskPath;
		biped.GetComponent<AnimatorComponent>().Layers = { wave };
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		system.Update(0.5f);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightForeArm"), AxisAngle(Z, -30)));
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), glm::quat(1, 0, 0, 0)));

		// Another Animator shares the asset.
		auto second = AddBiped(scene);
		second.GetComponent<AnimatorComponent>().Layers = { wave };
		system.Update(0);
		CHECK(NearRotation(LocalRotation(system, second, *source, "RightForeArm"), glm::quat(1, 0, 0, 0)));
		system.Update(0.5f);
		CHECK(NearRotation(LocalRotation(system, second, *source, "RightForeArm"), AxisAngle(Z, -30)));

		// A changed file applies once reloaded, without touching the components.
		project.WriteFile(maskPath, R"({ "Joints": [ { "Joint": "RightUpperArm" } ] })");
		AssetManager::Reload(maskPath);
		system.Update(0);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), AxisAngle(Z, -45)));
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightForeArm"), glm::quat(1, 0, 0, 0)));

		// One that cannot load hides the layer (rather than showing it on the whole body), said once.
		const auto since = Log::GetHistory().GetTotalCount();
		project.WriteFile(maskPath, R"({ "Joints": [ { "Joint": "A", "Weight": 2 } ] })");
		AssetManager::Reload(maskPath);
		system.Update(0);
		system.Update(0);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), glm::quat(1, 0, 0, 0)));
		CHECK(CountLogMessages(since, "layer 'Wave': cannot load mask 'Assets/Masks/Forearm.banimmask'") == 2);
		CHECK_FALSE(system.PlayLayer(biped, "Wave", std::nullopt));
		// Clearing MaskAsset returns to the inline mask.
		biped.GetComponent<AnimatorComponent>().Layers[0].MaskAsset.clear();
		system.Update(0);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), AxisAngle(Z, -45)));
		scene.OnRuntimeStop();
	}

	TEST_CASE("StateMachine layers run their own graph on the masked joints over the base")
	{
		ClearAssetCache clear;
		TempProject project("LayerGraphs");
		const auto source = LoadBiped();
		const std::string graphPath = project.WriteFile("Assets/Animations/Upper.banimgraph", UpperBodyGraph().dump());
		Scene scene;
		auto biped = AddBiped(scene);
		biped.GetComponent<AnimatorComponent>().Layers = { GraphLayer("Upper", graphPath, { { "Spine", 1.0f } }) };
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();

		// The initial state (Lean) shows over the base Walk, which keeps the legs.
		CHECK(system.GetLayerState(biped, "Upper") == "Lean");
		CHECK(NearRotation(LocalRotation(system, biped, *source, "Spine"), AxisAngle(Z, 20)));
		CHECK(NearRotation(LocalRotation(system, biped, *source, "LeftUpperLeg"), AxisAngle(X, 20)));
		// The Animator has no StateMachine of its own: parameters reach the layer's graph.
		REQUIRE(system.SetParameter(biped, "Waving", true));
		system.Update(0.5f);
		CHECK(system.GetLayerState(biped, "Upper") == "Wave");
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), AxisAngle(Z, -90)));
		// Wave does not animate the spine, but Lean (a state of the same graph) does: the layer holds it at the model's rest
		// pose.
		CHECK(NearRotation(LocalRotation(system, biped, *source, "Spine"), glm::quat(1, 0, 0, 0)));
		CHECK(NearRotation(LocalRotation(system, biped, *source, "LeftUpperLeg"), AxisAngle(X, -20)));
		// Triggers are consumed by the transition that takes them.
		REQUIRE(system.SetParameter(biped, "Waving", false));
		REQUIRE(system.SetTrigger(biped, "Nod"));
		system.Update(Step);
		CHECK(system.GetLayerState(biped, "Upper") == "Lean");
		REQUIRE(system.SetParameter(biped, "Waving", true));
		system.Update(Step);
		CHECK(system.GetLayerState(biped, "Upper") == "Wave");

		// Unknown names and wrong types fail and are logged once.
		const auto since = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(system.SetParameter(biped, "Missing", 1.0f));
		CHECK_FALSE(system.SetParameter(biped, "Missing", 1.0f));
		CHECK_FALSE(system.SetParameter(biped, "Waving", 1.0f));
		CHECK(CountLogMessages(since, "unknown name; declared parameters: Waving Nod") == 1);
		CHECK(CountLogMessages(since, "Animator 'Biped' layer 'Upper': parameter 'Waving': type mismatch: expected Bool, got Float") == 1);

		// A script clip plays instead of the graph; PlayAnimationLayer without one restarts the graph.
		REQUIRE(system.PlayLayer(biped, "Upper", std::string("Breathe")));
		CHECK(system.GetLayerState(biped, "Upper") == "");
		CHECK(NearRotation(LocalRotation(system, biped, *source, "Chest"), AxisAngle(X, 5)));
		REQUIRE(system.PlayLayer(biped, "Upper", std::nullopt));
		CHECK(system.GetLayerState(biped, "Upper") == "Lean");
		CHECK(NearRotation(LocalRotation(system, biped, *source, "Spine"), AxisAngle(Z, 20)));
		scene.OnRuntimeStop();
	}

	TEST_CASE("StateMachine layers fade between states share parameters with the base graph and report bad graphs")
	{
		ClearAssetCache clear;
		TempProject project("LayerGraphFades");
		const auto source = LoadBiped();
		const std::string upper = project.WriteFile("Assets/Animations/Upper.banimgraph", UpperBodyGraph(1.0f).dump());
		// The base graph walks while Waving (a name both graphs declare).
		const json base = {
			{ "InitialState", "Idle" },
			{ "Parameters", json::array({ { { "Name", "Waving" }, { "Type", "Bool" } } }) },
			{ "States", json::array({ { { "Name", "Idle" }, { "Clip", "Breathe" } }, { { "Name", "Walk" }, { "Clip", "Walk" } } }) },
			{ "Transitions", json::array({ { { "From", "Idle" }, { "To", "Walk" }, { "Duration", 0 }, { "Conditions", json::array({ { { "Parameter", "Waving" }, { "Value", true } } }) } } }) },
		};
		Scene scene;
		auto biped = AddBiped(scene, "");
		auto& animator = biped.GetComponent<AnimatorComponent>();
		animator.StateMachine = project.WriteFile("Assets/Animations/Base.banimgraph", base.dump());
		animator.Layers = { GraphLayer("Upper", upper, { { "Chest", 1.0f } }) };
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.SetParameter(biped, "Waving", true));
		system.Update(Step);
		CHECK(system.GetState(biped) == "Walk");
		CHECK(system.GetLayerState(biped, "Upper") == "Wave");
		// Half way through the layer's one-second fade, the arm is half way from Lean's (the rest) to Wave's -90 degrees.
		system.Update(0.5f - Step);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), AxisAngle(Z, -45), 1e-3f));
		system.Update(1.0f);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), AxisAngle(Z, -90), 1e-3f));

		// The layer never moves the entity: root motion belongs to the base.
		CHECK(biped.GetTransform().Translation == glm::vec3(0));

		// A graph that cannot load hides the layer and is said once; an Additive graph layer needs a ReferenceClip.
		const auto since = Log::GetHistory().GetTotalCount();
		animator.Layers[0].StateMachine = "Assets/Animations/Missing.banimgraph";
		system.Update(Step);
		system.Update(Step);
		CHECK(CountLogMessages(since, "layer 'Upper': cannot load state machine 'Assets/Animations/Missing.banimgraph'") == 1);
		CHECK(system.GetLayerState(biped, "Upper") == "");
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), glm::quat(1, 0, 0, 0)));
		animator.Layers[0].StateMachine = upper;
		animator.Layers[0].Blend = AnimationLayerBlend::Additive;
		system.Update(Step);
		CHECK(CountLogMessages(since, "an Additive layer with a StateMachine needs a ReferenceClip") == 1);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), glm::quat(1, 0, 0, 0)));
		animator.Layers[0].ReferenceClip = "Lean";
		system.Update(Step);
		CHECK(system.GetLayerState(biped, "Upper") == "Lean");
		CHECK_FALSE(system.GetLayerState(biped, "Missing").has_value());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Layer graph and mask files are validated, saved by identity, moved and checked by project.validate")
	{
		TempProject project("LayerAssetReferences");
		ClearAssetCache clear;
		CommandRegistry registry;
		AutomationSession session;
		using BasaltTest::Run;
		using BasaltTest::RunError;
		const auto model = FileSystem::ReadTextFile(BipedPath());
		REQUIRE(model);
		project.WriteFile("Assets/Models/Biped.gltf", *model);
		// asset.write refuses invalid masks and graphs without touching the file.
		CHECK(RunError(registry, session, "asset.write", { { "path", "Assets/Masks/Upper.banimmask" }, { "content", R"({ "Joints": [ { "Joint": "Chest", "Weight": 2 } ] })" } }).find("invalid animation mask: Joints[0].Weight") != std::string::npos);
		CHECK_FALSE(FileSystem::Exists(Project::ResolvePath("Assets/Masks/Upper.banimmask")));
		CHECK(RunError(registry, session, "asset.write", { { "path", "Assets/Animations/Upper.banimgraph" }, { "content", "{}" } }).find("invalid animation graph") != std::string::npos);
		CHECK(RunError(registry, session, "asset.write", { { "path", "Assets/Masks/Upper.banimmask" }, { "content", "not json" } }).find("invalid animation mask") != std::string::npos);
		Run(registry, session, "asset.write", { { "path", "Assets/Masks/Upper.banimmask" }, { "content", R"({ "Joints": [ { "Joint": "Chest" } ] })" } });
		Run(registry, session, "asset.write", { { "path", "Assets/Animations/Upper.banimgraph" }, { "content", UpperBodyGraph().dump() } });

		Run(registry, session, "scene.new", { { "name", "Layers" }, { "empty", true } });
		Run(registry, session, "entity.create", { { "name", "Hero" } });
		const json layer = { { "Name", "Upper" }, { "StateMachine", "Assets/Animations/Upper.banimgraph" }, { "MaskAsset", "Assets/Masks/Upper.banimmask" } };
		Run(registry, session, "component.set", { { "entity", "Hero" }, { "component", "Animator" }, { "data", { { "Mesh", "Assets/Models/Biped.gltf" }, { "Layers", { layer } } } } });
		// References name files of their kind.
		CHECK(RunError(registry, session, "component.set", { { "entity", "Hero" }, { "component", "Animator" }, { "data", { { "Layers", { { { "Name", "Upper" }, { "MaskAsset", "Assets/Animations/Upper.banimgraph" } } } } } } }).find("Layers[0].MaskAsset 'Assets/Animations/Upper.banimgraph' must be a .banimmask file") != std::string::npos);
		CHECK(RunError(registry, session, "component.set", { { "entity", "Hero" }, { "component", "Animator" }, { "data", { { "Layers", { { { "Name", "Upper" }, { "StateMachine", "Assets/Masks/Upper.banimmask" } } } } } } }).find("must be a .banimgraph file") != std::string::npos);
		CHECK(RunError(registry, session, "component.set", { { "entity", "Hero" }, { "component", "Animator" }, { "data", { { "StateMachine", "Assets/Models/Biped.gltf" } } } }).find("StateMachine 'Assets/Models/Biped.gltf' must be a .banimgraph file") != std::string::npos);

		// Saved as UUID/path objects, and moves follow them.
		Run(registry, session, "scene.save", { { "path", "Assets/Scenes/Layers.bscene" } });
		const auto saved = ParseJson(*FileSystem::ReadTextFile(Project::ResolvePath("Assets/Scenes/Layers.bscene")));
		const json& savedLayer = saved["Entities"][0]["Components"]["Animator"]["Layers"][0];
		CHECK(savedLayer["MaskAsset"]["Path"] == "Assets/Masks/Upper.banimmask");
		CHECK(savedLayer["StateMachine"]["Path"] == "Assets/Animations/Upper.banimgraph");
		Run(registry, session, "asset.move", { { "from", "Assets/Masks/Upper.banimmask" }, { "to", "Assets/Shared/UpperBody.banimmask" } });
		Run(registry, session, "asset.move", { { "from", "Assets/Animations/Upper.banimgraph" }, { "to", "Assets/Shared/UpperBody.banimgraph" } });
		const auto& moved = session.GetScene()->GetAllEntitiesOrdered()[0].GetComponent<AnimatorComponent>().Layers[0];
		CHECK(moved.MaskAsset == "Assets/Shared/UpperBody.banimmask");
		CHECK(moved.StateMachine == "Assets/Shared/UpperBody.banimgraph");
		std::string error;
		const auto loaded = SceneSerializer::LoadScene(Project::ResolvePath("Assets/Scenes/Layers.bscene"), error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->GetAllEntitiesOrdered()[0].GetComponent<AnimatorComponent>().Layers[0].MaskAsset == "Assets/Shared/UpperBody.banimmask");
		CHECK(GetAnimationWarnings(session.GetScene()->GetAllEntitiesOrdered()[0]).empty());
		// A deleted file leaves an unresolved reference that loads (and saves back unchanged) rather than failing the scene.
		Run(registry, session, "scene.save", { { "path", "Assets/Scenes/Layers.bscene" } });
		const json reference = ParseJson(*FileSystem::ReadTextFile(Project::ResolvePath("Assets/Scenes/Layers.bscene")))["Entities"][0]["Components"]["Animator"]["Layers"][0];
		const auto mask = FileSystem::ReadTextFile(Project::ResolvePath("Assets/Shared/UpperBody.banimmask"));
		const auto graphText = FileSystem::ReadTextFile(Project::ResolvePath("Assets/Shared/UpperBody.banimgraph"));
		const auto maskMeta = FileSystem::ReadTextFile(Project::ResolvePath("Assets/Shared/UpperBody.banimmask.meta"));
		const auto graphMeta = FileSystem::ReadTextFile(Project::ResolvePath("Assets/Shared/UpperBody.banimgraph.meta"));
		REQUIRE(Project::GetActive()->GetAssets().Remove("Assets/Shared/UpperBody.banimmask", error));
		REQUIRE(Project::GetActive()->GetAssets().Remove("Assets/Shared/UpperBody.banimgraph", error));
		const auto orphaned = SceneSerializer::LoadScene(Project::ResolvePath("Assets/Scenes/Layers.bscene"), error);
		REQUIRE_MESSAGE(orphaned, error);
		const auto& unresolved = orphaned->GetAllEntitiesOrdered()[0].GetComponent<AnimatorComponent>().Layers[0];
		CHECK(AssetRegistry::IsUnresolvedKey(unresolved.MaskAsset));
		CHECK(AssetRegistry::IsUnresolvedKey(unresolved.StateMachine));
		REQUIRE(SceneSerializer::SaveScene(*orphaned, Project::ResolvePath("Assets/Scenes/Orphaned.bscene"), error));
		const json saved2 = ParseJson(*FileSystem::ReadTextFile(Project::ResolvePath("Assets/Scenes/Orphaned.bscene")))["Entities"][0]["Components"]["Animator"]["Layers"][0];
		CHECK(saved2["MaskAsset"] == reference["MaskAsset"]);
		CHECK(saved2["StateMachine"] == reference["StateMachine"]);
		// Restored with their identities, the references resolve again.
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Shared/UpperBody.banimmask"), *mask));
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Shared/UpperBody.banimgraph"), *graphText));
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Shared/UpperBody.banimmask.meta"), *maskMeta));
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Shared/UpperBody.banimgraph.meta"), *graphMeta));
		REQUIRE(Project::GetActive()->GetAssets().Refresh(error));

		// project.validate checks the files and the references.
		Project::GetActive()->GetConfig().StartScene = "Assets/Scenes/Layers.bscene";
		CHECK(ValidateProject(*Project::GetActive()).IsValid());
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Shared/UpperBody.banimmask"), "{}"));
		REQUIRE(FileSystem::WriteTextFile(Project::ResolvePath("Assets/Shared/UpperBody.banimgraph"), "[]"));
		const auto has = [](const ValidationReport& report, const std::string& file, const std::string& message) {
			return std::ranges::any_of(report.GetIssues(), [&](const ValidationIssue& issue) { return issue.File == file && issue.Message.find(message) != std::string::npos; });
		};
		auto report = ValidateProject(*Project::GetActive());
		CHECK(has(report, "Assets/Shared/UpperBody.banimmask", "invalid animation mask: Joints must be an array"));
		CHECK(has(report, "Assets/Shared/UpperBody.banimgraph", "invalid animation graph"));
		Run(registry, session, "component.set", { { "entity", "Hero" }, { "component", "Animator" }, { "data", { { "Layers", { { { "Name", "Upper" }, { "MaskAsset", "Assets/Shared/Gone.banimmask" } } } } } } });
		Run(registry, session, "scene.save", { { "path", "Assets/Scenes/Layers.bscene" } });
		report = ValidateProject(*Project::GetActive());
		CHECK(has(report, "Assets/Scenes/Layers.bscene", "the missing file 'Assets/Shared/Gone.banimmask'"));
	}

	TEST_CASE("StateMachine layer graphs play blend space states and follow edits to their samples")
	{
		ClearAssetCache clear;
		TempProject project("LayerGraphSpaces");
		const auto source = LoadBiped();
		// Idle breathes; Move plays the Arms space once, then returns to Idle at its end.
		const json graph = {
			{ "InitialState", "Idle" },
			{ "Parameters", json::array({ { { "Name", "Go" }, { "Type", "Trigger" } } }) },
			{ "States", json::array({ { { "Name", "Idle" }, { "Clip", "Breathe" } }, { { "Name", "Move" }, { "BlendSpace", "Arms" }, { "Looping", false } } }) },
			{ "Transitions", json::array({ { { "From", "Idle" }, { "To", "Move" }, { "Duration", 0 }, { "Conditions", json::array({ { { "Parameter", "Go" } } }) } },
										   { { "From", "Move" }, { "To", "Idle" }, { "Duration", 0 }, { "ExitTime", 1 } } }) },
		};
		Scene scene;
		auto biped = AddBiped(scene);
		auto& animator = biped.GetComponent<AnimatorComponent>();
		animator.BlendSpaces = { { "Arms", AnimationBlendSpaceType::OneD, { { "Wave", 0, 0, 0 } } } };
		animator.Layers = { GraphLayer("Upper", project.WriteFile("Assets/Animations/Upper.banimgraph", graph.dump())) };
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.SetTrigger(biped, "Go"));
		system.Update(0.5f);
		// The space's sample clip moves the arm (its channels are the layer's); the phase follows the layer's clock.
		CHECK(system.GetLayerState(biped, "Upper") == "Move");
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), AxisAngle(Z, -90), 1e-3f));
		// Replacing the sample's clip during play moves the channels with it: Lean turns the spine now.
		animator.BlendSpaces[0].Samples[0].Clip = "Lean";
		system.Update(Step);
		CHECK(NearRotation(LocalRotation(system, biped, *source, "Spine"), AxisAngle(Z, 20), 1e-3f));
		// The non-looping space finishes and the exit time takes the graph back to Idle.
		system.Update(1.0f);
		system.Update(Step);
		CHECK(system.GetLayerState(biped, "Upper") == "Idle");
		scene.OnRuntimeStop();
	}

	TEST_CASE("StateMachine layers pause with the Animator, keep parameters across restarts and log unplayable states once")
	{
		ClearAssetCache clear;
		TempProject project("LayerGraphControl");
		const auto source = LoadBiped();
		const std::string upper = project.WriteFile("Assets/Animations/Upper.banimgraph", UpperBodyGraph().dump());
		// The base declares Nod too (a trigger each graph consumes for itself), and Speed as a Float the layer reads as a Bool.
		json layerGraph = UpperBodyGraph();
		layerGraph["Parameters"].push_back({ { "Name", "Speed" }, { "Type", "Bool" } });
		project.WriteFile(upper, layerGraph.dump());
		const json base = {
			{ "InitialState", "Idle" },
			{ "Parameters", json::array({ { { "Name", "Nod" }, { "Type", "Trigger" } }, { { "Name", "Speed" }, { "Type", "Float" } } }) },
			{ "States", json::array({ { { "Name", "Idle" }, { "Clip", "Breathe" } }, { { "Name", "Walk" }, { "Clip", "Walk" } } }) },
			{ "Transitions", json::array({ { { "From", "Idle" }, { "To", "Walk" }, { "Duration", 0 }, { "Conditions", json::array({ { { "Parameter", "Nod" } } }) } } }) },
		};
		Scene scene;
		auto biped = AddBiped(scene, "");
		auto& animator = biped.GetComponent<AnimatorComponent>();
		animator.StateMachine = project.WriteFile("Assets/Animations/Base.banimgraph", base.dump());
		animator.Layers = { GraphLayer("Upper", upper, { { "Chest", 1.0f } }) };
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();

		// Paused, the layer takes no transition; resumed, it does.
		system.Pause(biped, true);
		REQUIRE(system.SetParameter(biped, "Waving", true));
		system.Update(Step);
		CHECK(system.GetLayerState(biped, "Upper") == "Lean");
		system.Pause(biped, false);
		system.Update(Step);
		CHECK(system.GetLayerState(biped, "Upper") == "Wave");
		// Restarting the graph keeps what scripts set: it starts at Lean and goes straight back to Wave.
		REQUIRE(system.PlayLayer(biped, "Upper", std::nullopt));
		CHECK(system.GetLayerState(biped, "Upper") == "Lean");
		system.Update(Step);
		CHECK(system.GetLayerState(biped, "Upper") == "Wave");
		// One trigger reaches both graphs, and each takes its own transition.
		REQUIRE(system.SetParameter(biped, "Waving", false));
		REQUIRE(system.SetTrigger(biped, "Nod"));
		system.Update(Step);
		CHECK(system.GetState(biped) == "Walk");
		CHECK(system.GetLayerState(biped, "Upper") == "Lean");
		// The base takes a Float Speed; the layer's Bool one reports the mismatch once, under the layer's name.
		const auto since = Log::GetHistory().GetTotalCount();
		CHECK(system.SetParameter(biped, "Speed", 1.0f));
		CHECK(system.SetParameter(biped, "Speed", 2.0f));
		CHECK(CountLogMessages(since, "Animator 'Biped' layer 'Upper': parameter 'Speed': type mismatch: expected Bool, got Float") == 1);
		// The other way round, the layer takes the value and the base reports its own mismatch, once.
		CHECK(system.SetParameter(biped, "Speed", true));
		CHECK(system.SetParameter(biped, "Speed", true));
		CHECK(CountLogMessages(since, "Animator 'Biped': parameter 'Speed': type mismatch: expected Float, got Bool") == 1);
		// A name only the layer declares is the layer's to report: no unknown name from the base.
		CHECK_FALSE(system.SetParameter(biped, "Waving", 1.0f));
		CHECK(CountLogMessages(since, "Animator 'Biped' layer 'Upper': parameter 'Waving': type mismatch: expected Bool, got Float") == 1);
		CHECK(CountLogMessages(since, "unknown name") == 0);
		// A typo between good calls stays logged once: a good call clears only its own name's report.
		for (int i = 0; i < 3; i++)
		{
			CHECK_FALSE(system.SetParameter(biped, "Wavin", true));
			CHECK(system.SetParameter(biped, "Waving", false));
		}
		CHECK(CountLogMessages(since, "parameter 'Wavin': unknown name; declared parameters: Nod Speed Waving") == 1);
		// A trigger latched before a restart belongs to the old run: the restarted graph does not take Nod.
		REQUIRE(system.SetParameter(biped, "Waving", true));
		system.Update(Step);
		REQUIRE(system.GetLayerState(biped, "Upper") == "Wave");
		REQUIRE(system.SetParameter(biped, "Waving", false));
		system.Pause(biped, true);
		REQUIRE(system.SetTrigger(biped, "Nod"));
		REQUIRE(system.PlayLayer(biped, "Upper", std::nullopt));
		system.Pause(biped, false);
		REQUIRE(system.SetParameter(biped, "Waving", true));
		system.Update(Step);
		system.Update(Step);
		CHECK(system.GetLayerState(biped, "Upper") == "Wave");
		// After a script clip replaced the graph, it starts from its defaults; a name only it declares is reported as
		// declared by a layer whose graph is not running.
		REQUIRE(system.PlayLayer(biped, "Upper", std::string("Breathe")));
		CHECK_FALSE(system.SetParameter(biped, "Waving", true));
		CHECK(CountLogMessages(since, "parameter 'Waving': declared by layer 'Upper', whose state machine is not running") == 1);
		REQUIRE(system.PlayLayer(biped, "Upper", std::nullopt));
		system.Update(Step);
		CHECK(system.GetLayerState(biped, "Upper") == "Lean");

		// A graph whose initial state cannot play shows nothing, and restarting it every frame logs the state once.
		json broken = UpperBodyGraph();
		broken["States"][0]["Clip"] = "Gone";
		project.WriteFile(upper, broken.dump());
		AssetManager::Reload(upper);
		system.Update(Step);
		for (int i = 0; i < 5; i++)
		{
			CHECK_FALSE(system.PlayLayer(biped, "Upper", std::nullopt));
			system.Update(Step);
		}
		CHECK(CountLogMessages(since, "layer 'Upper': state 'Lean' cannot play: clip 'Gone' not found") == 1);
		CHECK(system.GetLayerState(biped, "Upper") == "");
		CHECK(NearRotation(LocalRotation(system, biped, *source, "RightUpperArm"), glm::quat(1, 0, 0, 0)));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Parameter problems name what is missing: no graph at all, idle layer graphs or an undeclared name")
	{
		ClearAssetCache clear;
		TempProject project("ParameterMessages");
		Scene scene;
		auto plain = AddBiped(scene);
		auto idle = AddBiped(scene);
		idle.GetComponent<AnimatorComponent>().Layers = { GraphLayer("Upper", project.WriteFile("Assets/Animations/Broken.banimgraph", "{}")) };
		auto empty = AddBiped(scene, "");
		const json bare = { { "InitialState", "Idle" }, { "States", json::array({ { { "Name", "Idle" }, { "Clip", "Breathe" } } }) } };
		empty.GetComponent<AnimatorComponent>().StateMachine = project.WriteFile("Assets/Animations/Bare.banimgraph", bare.dump());
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		const auto since = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(system.SetParameter(plain, "Go", true));
		CHECK_FALSE(system.SetParameter(idle, "Go", true));
		CHECK_FALSE(system.SetParameter(empty, "Go", true));
		CHECK(CountLogMessages(since, "parameter 'Go': Animator has no StateMachine") == 1);
		CHECK(CountLogMessages(since, "parameter 'Go': no state machine is running (the layers' graphs show nothing or a script clip replaced them)") == 1);
		// A running graph without parameters: the name is unknown, not the graph missing.
		CHECK(CountLogMessages(since, "parameter 'Go': unknown name; declared parameters:") == 1);
		scene.OnRuntimeStop();
	}
}

TEST_SUITE("Animation")
{
	TEST_CASE("Layers with Events fire the Animator's markers for what they play, and only then")
	{
		ClearAssetCache clear;
		Scene scene;
		auto biped = AddBiped(scene, "Lean");
		auto& animator = biped.GetComponent<AnimatorComponent>();
		animator.Events = { { .Clip = "Wave", .Time = 0.25f, .Name = "Hit" }, { .Clip = "", .Time = 0.1f, .Name = "Any" } };
		animator.Layers = { Layer("Upper", "Wave") };
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		const auto names = [&] {
			std::vector<std::string> fired;
			for (const AnimationEventRecord& record : system.TakeEvents())
				fired.push_back(record.Name + "@" + record.Clip + "/" + record.Layer);
			return fired;
		};
		const std::vector<std::string> baseOnly{ "Any@Lean/" };
		// By default a layer stays silent, as it always was: a marker without a Clip fires once, for the base.
		system.Update(0.2f);
		CHECK(names() == baseOnly);
		system.Seek(biped, 0);
		// With Events it fires its own clip's markers, and the record names it. The base's own marker comes after.
		animator.Layers[0].Events = true;
		system.PlayLayer(biped, "Upper", std::nullopt);
		system.Update(0.3f);
		CHECK(names() == std::vector<std::string>{ "Any@Wave/Upper", "Hit@Wave/Upper", "Any@Lean/" });

		// A layer that does not show fires nothing, though its clock runs.
		REQUIRE(system.PlayLayer(biped, "Upper", std::nullopt));
		REQUIRE(system.SetLayerWeight(biped, "Upper", 0, 0));
		system.Update(0.3f);
		for (const std::string& name : names())
			CHECK(name.find("/Upper") == std::string::npos);
		REQUIRE(system.SetLayerWeight(biped, "Upper", 1, 0));
		// Nor does a clock that did not move.
		REQUIRE(system.PlayLayer(biped, "Upper", std::nullopt));
		system.Update(0);
		CHECK(names().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Layer graphs fire events as the base does, across fades and blend spaces")
	{
		ClearAssetCache clear;
		TempProject project("LayerGraphEvents");
		const std::string graph = project.WriteFile("Assets/Animations/Upper.banimgraph", UpperBodyGraph(0.5f).dump());
		Scene scene;
		auto biped = AddBiped(scene, "Breathe");
		auto& animator = biped.GetComponent<AnimatorComponent>();
		animator.Events = { { .Clip = "Lean", .Time = 0.05f, .Name = "Leaning" }, { .Clip = "Lean", .Time = 0.35f, .Name = "LateLean" }, { .Clip = "Lean", .Time = 0.65f, .Name = "PastFade" }, { .Clip = "Wave", .Time = 0.05f, .Name = "Waving" } };
		animator.Layers = { GraphLayer("Upper", graph) };
		animator.Layers[0].Events = true;
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		const auto names = [&] {
			std::vector<std::string> fired;
			for (const AnimationEventRecord& record : system.TakeEvents())
				fired.push_back(record.Name + "/" + record.Layer);
			return fired;
		};
		system.Update(0.1f);
		CHECK(names() == std::vector<std::string>{ "Leaning/Upper" });
		// The crossfade's target fires from its first frame; the outgoing clip fires only as far as the fade shows it, so a
		// marker past the fade's end stays quiet.
		REQUIRE(system.SetParameter(biped, "Waving", true));
		system.Update(0.1f);
		CHECK(names() == std::vector<std::string>{ "Waving/Upper" });
		system.Update(0.2f);
		CHECK(names() == std::vector<std::string>{ "LateLean/Upper" });
		// The fade ends at Lean's 0.6 s, though this update's clock runs on to 0.7 s.
		system.Update(0.3f);
		CHECK(names().empty());
		scene.OnRuntimeStop();

		// A blend space state fires its heaviest sample's markers, once.
		const json spaces = {
			{ "InitialState", "Move" },
			{ "Parameters", json::array({ { { "Name", "Speed" }, { "Type", "Float" } } }) },
			{ "States", json::array({ { { "Name", "Move" }, { "BlendSpace", "Space" }, { "ParameterX", "Speed" } } }) },
		};
		Scene other;
		auto rig = AddBiped(other, "Breathe");
		auto& config = rig.GetComponent<AnimatorComponent>();
		config.BlendSpaces = { { .Name = "Space", .Samples = { { .Clip = "Lean", .X = 0 }, { .Clip = "Wave", .X = 1 } } } };
		config.Events = { { .Clip = "Lean", .Time = 0.05f, .Name = "L" }, { .Clip = "Wave", .Time = 0.05f, .Name = "W" } };
		config.Layers = { GraphLayer("Upper", project.WriteFile("Assets/Animations/Spaces.banimgraph", spaces.dump())) };
		config.Layers[0].Events = true;
		other.OnRuntimeStart();
		auto& spaced = *other.GetAnimationSystem();
		spaced.Update(0.1f);
		const auto events = spaced.TakeEvents();
		REQUIRE(events.size() == 1);
		CHECK(events[0].Name == "L");
		CHECK(events[0].Layer == "Upper");
		other.OnRuntimeStop();
	}

	TEST_CASE("Layer events follow the layer's own clock and looping")
	{
		ClearAssetCache clear;
		TempProject project("LayerEventClocks");
		Scene scene;
		auto biped = AddBiped(scene, "Lean");
		auto& animator = biped.GetComponent<AnimatorComponent>();
		animator.Events = { { .Clip = "Wave", .Time = 0.1f, .Name = "Mark" } };
		animator.Layers = { Layer("Slow", "Wave"), Layer("Fast", "Wave"), Layer("Once", "Wave") };
		animator.Layers[0].Speed = 0.5f;
		animator.Layers[1].Speed = 3;
		animator.Layers[2].Looping = false;
		for (AnimationLayer& layer : animator.Layers)
			layer.Events = true;
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		// Half speed has not reached the marker at 0.1 s of clip time after 0.1 s; three times speed passed it.
		system.Update(0.1f);
		const auto first = system.TakeEvents();
		CHECK(std::ranges::count(first, std::string("Slow"), &AnimationEventRecord::Layer) == 0);
		CHECK(std::ranges::count(first, std::string("Fast"), &AnimationEventRecord::Layer) == 1);
		CHECK(std::ranges::count(first, std::string("Once"), &AnimationEventRecord::Layer) == 0);
		// Over three loops of a one-second clip a looping layer fires each time, and one that does not loop once.
		system.Update(3.0f);
		const auto loops = system.TakeEvents();
		CHECK(std::ranges::count(loops, std::string("Slow"), &AnimationEventRecord::Layer) == 2);
		CHECK(std::ranges::count(loops, std::string("Fast"), &AnimationEventRecord::Layer) == 9);
		CHECK(std::ranges::count(loops, std::string("Once"), &AnimationEventRecord::Layer) == 1);
		scene.OnRuntimeStop();

		// A graph layer's outgoing clip wraps with its own looping across a fade, and fires at its own speed.
		const json graph = {
			{ "InitialState", "A" },
			{ "Parameters", json::array({ { { "Name", "Go" }, { "Type", "Bool" } } }) },
			{ "States", json::array({ { { "Name", "A" }, { "Clip", "Lean" } }, { { "Name", "B" }, { "Clip", "Wave" } } }) },
			{ "Transitions", json::array({ { { "From", "A" }, { "To", "B" }, { "Duration", 2 }, { "Conditions", json::array({ { { "Parameter", "Go" }, { "Value", true } } }) } } }) },
		};
		Scene other;
		auto rig = AddBiped(other, "Breathe");
		auto& config = rig.GetComponent<AnimatorComponent>();
		config.Events = { { .Clip = "Lean", .Time = 0.5f, .Name = "L" } };
		config.Layers = { GraphLayer("Upper", project.WriteFile("Assets/Animations/Fade.banimgraph", graph.dump())) };
		config.Layers[0].Events = true;
		config.Layers[0].Speed = 2;
		other.OnRuntimeStart();
		auto& fading = *other.GetAnimationSystem();
		REQUIRE(fading.SetParameter(rig, "Go", true));
		// 1.5 s of a two-second fade at twice speed covers 3 s of the outgoing clip: its marker fires three times.
		fading.Update(1.5f);
		CHECK(std::ranges::count(fading.TakeEvents(), std::string("L"), &AnimationEventRecord::Name) == 3);
		// The fade ends, and Lean's clock stops with it: 0.5 s of fade left is 1 s of clip, from 0 to 1.
		fading.Update(1.0f);
		CHECK(std::ranges::count(fading.TakeEvents(), std::string("L"), &AnimationEventRecord::Name) == 1);
		fading.Update(1.0f);
		CHECK(fading.TakeEvents().empty());
		other.OnRuntimeStop();
	}

	TEST_CASE("A busy layer cannot crowd out the base's events")
	{
		ClearAssetCache clear;
		Scene scene;
		auto biped = AddBiped(scene, "Lean");
		auto& animator = biped.GetComponent<AnimatorComponent>();
		for (size_t i = 0; i < 2 * AnimationSystem::MaxEventsPerFrame; i++)
			animator.Events.push_back({ .Clip = "Wave", .Time = 0.01f, .Name = "E" + std::to_string(i) });
		animator.Events.push_back({ .Clip = "Lean", .Time = 0.01f, .Name = "Base" });
		animator.Layers = { Layer("Upper", "Wave"), Layer("Second", "Wave") };
		animator.Layers[0].Events = animator.Layers[1].Events = true;
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		const auto since = Log::GetHistory().GetTotalCount();
		system.Update(0.02f);
		const auto events = system.TakeEvents();
		// The layers share theirs, and come first in the queue.
		CHECK(events.size() == AnimationSystem::MaxEventsPerFrame + 1);
		CHECK(std::ranges::count(events, std::string(), &AnimationEventRecord::Layer) == 1);
		CHECK(events.back().Name == "Base");
		CHECK(CountLogMessages(since, "Animator 'Biped' layer 'Upper': more than 64 animation events in one frame") == 1);
		CHECK(CountLogMessages(since, "Animator 'Biped' layer 'Second': more than 64 animation events in one frame") == 1);
		// Each side says so once, whichever went over first.
		const auto later = Log::GetHistory().GetTotalCount();
		animator.Events.clear();
		for (size_t i = 0; i < 2 * AnimationSystem::MaxEventsPerFrame; i++)
			animator.Events.push_back({ .Clip = "Lean", .Time = 0.5f, .Name = "E" + std::to_string(i) });
		system.Update(0.6f);
		system.Update(1.0f);
		CHECK(CountLogMessages(later, "Animator 'Biped': more than 64 animation events in one frame") == 1);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Fading layers fire markers for the part of an update they show")
	{
		ClearAssetCache clear;
		Scene scene;
		auto biped = AddBiped(scene, "Lean");
		auto& animator = biped.GetComponent<AnimatorComponent>();
		animator.Events = { { .Clip = "Wave", .Time = 0.05f, .Name = "Early" }, { .Clip = "Wave", .Time = 0.15f, .Name = "Late" } };
		animator.Layers = { Layer("Upper", "Wave") };
		animator.Layers[0].Events = true;
		animator.Layers[0].Weight = 0;
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		const auto fired = [&] {
			std::vector<std::string> names;
			for (const AnimationEventRecord& record : system.TakeEvents())
				names.push_back(record.Name);
			return names;
		};
		// A fade in shows from its first instant, though the weight reads 0 as the update starts.
		REQUIRE(system.SetLayerWeight(biped, "Upper", 1, 0.2f));
		system.Update(0.1f);
		CHECK(fired() == std::vector<std::string>{ "Early" });
		system.Update(0.1f);
		CHECK(fired() == std::vector<std::string>{ "Late" });
		CHECK(system.GetLayerWeight(biped, "Upper") == doctest::Approx(1));

		// A fade out stops where it reaches 0, though this update's clock runs on.
		REQUIRE(system.PlayLayer(biped, "Upper", std::nullopt));
		system.Update(0.01f);
		CHECK(fired().empty());
		REQUIRE(system.SetLayerWeight(biped, "Upper", 0, 0.08f));
		system.Update(0.2f);
		CHECK(fired() == std::vector<std::string>{ "Early" });
		CHECK(system.GetLayerWeight(biped, "Upper") == doctest::Approx(0));
		// At 0 it fires nothing, its clock still running.
		system.Update(0.5f);
		CHECK(fired().empty());
		scene.OnRuntimeStop();

		FadingWeight weight(1);
		CHECK(weight.ShownShare(1) == 1);
		weight.FadeTo(0, 0.5f);
		CHECK(weight.ShownShare(0.25f) == 1);
		CHECK(weight.ShownShare(1) == doctest::Approx(0.5f));
		weight.Advance(1);
		CHECK(weight.ShownShare(1) == 0);
		weight.FadeTo(0.5f, 1);
		CHECK(weight.ShownShare(1) == 1);
	}

	TEST_CASE("OwnParameters keeps a layer's graph out of SetParameter and SetLayerParameter reaches one layer alone")
	{
		ClearAssetCache clear;
		TempProject project("LayerParameters");
		const std::string graph = project.WriteFile("Assets/Animations/Upper.banimgraph", UpperBodyGraph().dump());
		Scene scene;
		auto biped = AddBiped(scene);
		auto& animator = biped.GetComponent<AnimatorComponent>();
		animator.Layers = { GraphLayer("Own", graph), GraphLayer("Shared", graph), Layer("Clip", "Breathe") };
		animator.Layers[0].OwnParameters = true;
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();

		// The shared write skips the layer that keeps its own parameters.
		REQUIRE(system.SetParameter(biped, "Waving", true));
		system.Update(Step);
		CHECK(system.GetLayerState(biped, "Own") == "Lean");
		CHECK(system.GetLayerState(biped, "Shared") == "Wave");
		// A layer write reaches that layer alone, whichever kind.
		REQUIRE(system.SetLayerParameter(biped, "Own", "Waving", true));
		system.Update(Step);
		CHECK(system.GetLayerState(biped, "Own") == "Wave");
		REQUIRE(system.SetLayerTrigger(biped, "Shared", "Nod"));
		REQUIRE(system.SetParameter(biped, "Waving", false));
		system.Update(Step);
		CHECK(system.GetLayerState(biped, "Shared") == "Lean");
		CHECK(system.GetLayerState(biped, "Own") == "Wave");
		REQUIRE(system.SetLayerTrigger(biped, "Own", "Nod"));
		REQUIRE(system.SetLayerParameter(biped, "Own", "Waving", false));
		system.Update(Step);
		CHECK(system.GetLayerState(biped, "Own") == "Lean");

		// Each failure says what is wrong, once, and changes nothing.
		const auto since = Log::GetHistory().GetTotalCount();
		const auto twice = [](const auto& call) {
			CHECK_FALSE(call());
			CHECK_FALSE(call());
		};
		twice([&] { return system.SetLayerParameter(biped, "Nowhere", "Waving", true); });
		twice([&] { return system.SetLayerParameter(biped, "Clip", "Waving", true); });
		twice([&] { return system.SetLayerParameter(biped, "Own", "Missing", true); });
		twice([&] { return system.SetLayerParameter(biped, "Own", "Waving", 1.0f); });
		twice([&] { return system.SetLayerTrigger(biped, "Own", "Waving"); });
		CHECK_FALSE(system.SetLayerParameter(biped, "Own", "Nod", std::numeric_limits<float>::quiet_NaN()));
		CHECK(CountLogMessages(since, "no layer named 'Nowhere'") == 1);
		CHECK(CountLogMessages(since, "layer 'Clip' runs no state machine") == 1);
		CHECK(CountLogMessages(since, "layer 'Own': parameter 'Missing': unknown name; declared parameters: Waving Nod") == 1);
		CHECK(CountLogMessages(since, "layer 'Own': parameter 'Waving': type mismatch: expected Bool, got Float") == 1);
		CHECK(CountLogMessages(since, "layer 'Own': parameter 'Waving': type mismatch: expected Bool, got Trigger") == 1);
		CHECK(CountLogMessages(since, "layer 'Own': parameter 'Nod': value must be finite") == 1);
		system.Update(Step);
		CHECK(system.GetLayerState(biped, "Own") == "Lean");

		// A name only such a layer declares is not the shared write's to take, and the message says where it lives.
		animator.Layers[1].OwnParameters = true;
		const auto later = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(system.SetParameter(biped, "Waving", true));
		CHECK_FALSE(system.SetParameter(biped, "Waving", true));
		CHECK(CountLogMessages(later, "parameter 'Waving': declared by layer 'Own', which keeps its own parameters") == 1);
		// Also while that layer's graph is idle (a script clip plays instead).
		REQUIRE(system.PlayLayer(biped, "Own", std::string("Breathe")));
		REQUIRE(system.PlayLayer(biped, "Shared", std::string("Breathe")));
		const auto idle = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(system.SetTrigger(biped, "Nod"));
		CHECK(CountLogMessages(idle, "parameter 'Nod': declared by layer 'Own', which keeps its own parameters") == 1);
		REQUIRE(system.PlayLayer(biped, "Own", std::nullopt));
		REQUIRE(system.PlayLayer(biped, "Shared", std::nullopt));
		// A restart of the layer's graph keeps what a script set.
		REQUIRE(system.SetLayerParameter(biped, "Own", "Waving", true));
		REQUIRE(system.PlayLayer(biped, "Own", std::nullopt));
		system.Update(Step);
		CHECK(system.GetLayerState(biped, "Own") == "Wave");
		scene.OnRuntimeStop();
	}

	TEST_CASE("Lua sets one layer's parameters and hears which layer fired an event")
	{
		ClearAssetCache clear;
		TempProject project("LayerLua");
		const std::string graph = project.WriteFile("Assets/Animations/Upper.banimgraph", UpperBodyGraph().dump());
		const std::string script = project.WriteFile("Assets/Scripts/Rig.lua", R"(
			local Rig = {}
			function Rig:OnAnimationEvent(name, clip, layer)
				Fired = (Fired or "") .. name .. "/" .. tostring(layer) .. ";"
			end
			return Rig
		)");
		Scene scene;
		auto biped = AddBiped(scene, "Breathe");
		auto& animator = biped.GetComponent<AnimatorComponent>();
		animator.Events = { { .Clip = "Lean", .Time = 0.01f, .Name = "Lean" }, { .Clip = "Breathe", .Time = 0.01f, .Name = "Base" } };
		animator.Layers = { GraphLayer("Upper", graph) };
		animator.Layers[0].OwnParameters = true;
		animator.Layers[0].Events = true;
		biped.AddComponent<ScriptComponent>().Script = script;
		scene.CreateEntity("Plain");
		scene.OnRuntimeStart();
		scene.OnUpdate(Step);
		const auto run = [&](const std::string& code) { return Eval(scene, "local e = Scene.FindEntityByName('Biped'); " + code); };
		// The layer's name for its clip's marker, nil for the base's.
		CHECK(run("return Fired") == "Lean/Upper;Base/nil;");
		CHECK(run("return tostring(e:SetAnimationParameter('Waving', true))") == "false");
		CHECK(run("return tostring(e:SetAnimationLayerParameter('Upper', 'Waving', 1))") == "false");
		CHECK(run("return tostring(e:SetAnimationLayerParameter('Upper', 'Missing', true))") == "false");
		CHECK(run("return tostring(e:SetAnimationLayerTrigger('Upper', 'Nod'))") == "true");
		CHECK(run("return tostring(e:SetAnimationLayerParameter('Upper', 'Waving', true))") == "true");
		scene.OnUpdate(Step);
		CHECK(run("return e:GetAnimationLayerState('Upper')") == "Wave");
		const std::pair<const char*, const char*> errors[] = {
			{ "Scene.FindEntityByName('Biped'):SetAnimationLayerParameter('Upper', 'Waving')", "value must be a number or boolean" },
			{ "Scene.FindEntityByName('Biped'):SetAnimationLayerParameter('Upper', 'Waving', 'on')", "value must be a number or boolean" },
			{ "Scene.FindEntityByName('Plain'):SetAnimationLayerParameter('Upper', 'Waving', true)", "needs the Animator component" },
			{ "Scene.FindEntityByName('Plain'):SetAnimationLayerTrigger('Upper', 'Nod')", "needs the Animator component" },
		};
		CheckLuaErrors(*scene.GetScriptEngine(), errors);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Layer warnings name settings that do nothing")
	{
		ClearAssetCache clear;
		Scene scene;
		auto biped = AddBiped(scene);
		auto& animator = biped.GetComponent<AnimatorComponent>();
		animator.Layers = { Layer("Upper", "Wave") };
		animator.Layers[0].OwnParameters = true;
		animator.Layers[0].Events = true;
		const auto warnings = GetAnimationWarnings(biped);
		CHECK(std::ranges::find(warnings, "Animator layer 'Upper': OwnParameters has no effect without a StateMachine") != warnings.end());
		CHECK(std::ranges::find(warnings, "Animator layer 'Upper': Events has no effect: the Animator has no Events") != warnings.end());
		// Neither says so when it has its effect.
		TempProject project("LayerWarnings");
		animator.Events = { { .Name = "Step" } };
		animator.Layers[0].OwnParameters = true;
		animator.Layers[0].Clip = "";
		animator.Layers[0].StateMachine = project.WriteFile("Assets/Animations/Upper.banimgraph", UpperBodyGraph().dump());
		const auto quiet = GetAnimationWarnings(biped);
		CHECK(std::ranges::none_of(quiet, [](const std::string& line) { return line.find("OwnParameters") != std::string::npos || line.find("Events has no effect") != std::string::npos; }));
		json written = ComponentRegistry::Find("Animator")->Serialize(biped);
		CHECK(written["Layers"][0]["OwnParameters"] == true);
		CHECK(written["Layers"][0]["Events"] == true);
		written["Layers"][0]["Events"] = false;
		std::string error;
		REQUIRE_MESSAGE(ComponentRegistry::AddOrPatch(biped, "Animator", written, error), error);
		CHECK_FALSE(animator.Layers[0].Events);
	}
}

TEST_SUITE("Animation")
{
	TEST_CASE("A looping state of a layer graph fires across its wrap, also while fading out")
	{
		ClearAssetCache clear;
		TempProject project("LayerGraphWrap");
		const std::string graph = project.WriteFile("Assets/Animations/Upper.banimgraph", UpperBodyGraph(0.5f).dump());
		for (const bool fading : { false, true })
		{
			CAPTURE(fading);
			Scene scene;
			auto biped = AddBiped(scene, "Breathe");
			auto& animator = biped.GetComponent<AnimatorComponent>();
			animator.Events = { { .Clip = "Lean", .Time = 0.05f, .Name = "Hit" } };
			animator.Layers = { GraphLayer("Upper", graph) };
			animator.Layers[0].Events = true;
			scene.OnRuntimeStart();
			auto& system = *scene.GetAnimationSystem();
			system.Update(0.9f);
			CHECK(system.TakeEvents().size() == 1);
			if (fading)
				REQUIRE(system.SetParameter(biped, "Waving", true));
			system.Update(0.2f);
			CHECK(system.TakeEvents().size() == 1);
			scene.OnRuntimeStop();
		}
	}

	TEST_CASE("SetLayerParameter and Lua hand a Float parameter its value")
	{
		ClearAssetCache clear;
		TempProject project("LayerFloat");
		const json graph = {
			{ "InitialState", "Lean" },
			{ "Parameters", json::array({ { { "Name", "Amount" }, { "Type", "Float" } } }) },
			{ "States", json::array({ { { "Name", "Lean" }, { "Clip", "Lean" } }, { { "Name", "Wave" }, { "Clip", "Wave" } } }) },
			{ "Transitions", json::array({ { { "From", "Lean" }, { "To", "Wave" }, { "Duration", 0 }, { "Conditions", json::array({ { { "Parameter", "Amount" }, { "Operator", "Greater" }, { "Value", 0.5 } } }) } },
										   { { "From", "Wave" }, { "To", "Lean" }, { "Duration", 0 }, { "Conditions", json::array({ { { "Parameter", "Amount" }, { "Operator", "Less" }, { "Value", 0.5 } } }) } } }) },
		};
		const std::string path = project.WriteFile("Assets/Animations/Upper.banimgraph", graph.dump());
		const std::string script = project.WriteFile("Assets/Scripts/Rig.lua", "local Rig = {} return Rig");
		Scene scene;
		auto biped = AddBiped(scene, "Breathe");
		biped.GetComponent<AnimatorComponent>().Layers = { GraphLayer("Upper", path) };
		biped.AddComponent<ScriptComponent>().Script = script;
		scene.OnRuntimeStart();
		scene.OnUpdate(Step);
		auto& system = *scene.GetAnimationSystem();
		REQUIRE(system.SetLayerParameter(biped, "Upper", "Amount", 0.75f));
		scene.OnUpdate(Step);
		CHECK(system.GetLayerState(biped, "Upper") == "Wave");
		CHECK(Eval(scene, "return tostring(Scene.FindEntityByName('Biped'):SetAnimationLayerParameter('Upper', 'Amount', 0.25))") == "true");
		scene.OnUpdate(Step);
		CHECK(system.GetLayerState(biped, "Upper") == "Lean");
		CHECK(Eval(scene, "return tostring(Scene.FindEntityByName('Biped'):SetAnimationLayerParameter('Upper', 'Amount', 0.75))") == "true");
		scene.OnUpdate(Step);
		CHECK(system.GetLayerState(biped, "Upper") == "Wave");
		scene.OnRuntimeStop();
	}

	TEST_CASE("An own layer's message is for names it declares and only running graphs")
	{
		ClearAssetCache clear;
		TempProject project("LayerNames");
		const std::string graph = project.WriteFile("Assets/Animations/Upper.banimgraph", UpperBodyGraph().dump());
		Scene scene;
		auto biped = AddBiped(scene);
		auto& animator = biped.GetComponent<AnimatorComponent>();
		animator.Layers = { Layer("Plain", "Breathe"), GraphLayer("Own", graph), GraphLayer("Shared", graph) };
		animator.Layers[0].OwnParameters = true;
		animator.Layers[1].OwnParameters = true;
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		const auto since = Log::GetHistory().GetTotalCount();
		// A name nobody declares is not an own layer's.
		CHECK_FALSE(system.SetParameter(biped, "Missing", true));
		CHECK(CountLogMessages(since, "declared by layer") == 0);
		// The layer that runs no graph does not hold the name.
		animator.Layers[2].OwnParameters = true;
		CHECK_FALSE(system.SetParameter(biped, "Waving", true));
		CHECK(CountLogMessages(since, "declared by layer 'Plain'") == 0);
		CHECK(CountLogMessages(since, "declared by layer 'Own'") == 1);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Unknown names are logged per name and per type, and a success lets a problem log again")
	{
		ClearAssetCache clear;
		TempProject project("LayerReport");
		const std::string graph = project.WriteFile("Assets/Animations/Upper.banimgraph", UpperBodyGraph().dump());
		Scene scene;
		auto biped = AddBiped(scene);
		biped.GetComponent<AnimatorComponent>().Layers = { GraphLayer("Own", graph) };
		biped.GetComponent<AnimatorComponent>().Layers[0].OwnParameters = true;
		scene.OnRuntimeStart();
		auto& system = *scene.GetAnimationSystem();
		const auto since = Log::GetHistory().GetTotalCount();
		CHECK_FALSE(system.SetLayerParameter(biped, "Own", "First", true));
		CHECK_FALSE(system.SetLayerParameter(biped, "Own", "Second", true));
		CHECK(CountLogMessages(since, "parameter 'First': unknown name") == 1);
		CHECK(CountLogMessages(since, "parameter 'Second': unknown name") == 1);
		CHECK_FALSE(system.SetLayerParameter(biped, "Own", "Third", 1.0f));
		CHECK_FALSE(system.SetLayerParameter(biped, "Own", "Third", true));
		CHECK(CountLogMessages(since, "parameter 'Third': unknown name") == 2);

		// An unknown layer again after a good call is news.
		CHECK_FALSE(system.SetLayerParameter(biped, "Nowhere", "Waving", true));
		REQUIRE(system.SetLayerParameter(biped, "Own", "Waving", true));
		CHECK_FALSE(system.SetLayerParameter(biped, "Nowhere", "Waving", true));
		CHECK(CountLogMessages(since, "no layer named 'Nowhere'") == 2);

		// So is a layer that stops running its graph again.
		REQUIRE(system.PlayLayer(biped, "Own", std::string("Wave")));
		CHECK_FALSE(system.SetLayerParameter(biped, "Own", "Waving", true));
		REQUIRE(system.PlayLayer(biped, "Own", std::nullopt));
		REQUIRE(system.SetLayerParameter(biped, "Own", "Waving", true));
		REQUIRE(system.PlayLayer(biped, "Own", std::string("Wave")));
		CHECK_FALSE(system.SetLayerParameter(biped, "Own", "Waving", true));
		CHECK(CountLogMessages(since, "layer 'Own' runs no state machine") == 2);
		scene.OnRuntimeStop();
	}
}

// Round 2 of PR #72: appended to Tests/Source/AnimationLayerTests.cpp, whose helpers these use.
TEST_SUITE("Animation")
{
	TEST_CASE("A graph layer fading out stops its markers where it reaches 0, in every way a graph plays")
	{
		ClearAssetCache clear;
		TempProject project("LayerGraphFadeOut");
		const json spaces = {
			{ "InitialState", "Move" },
			{ "Parameters", json::array({ { { "Name", "Speed" }, { "Type", "Float" } } }) },
			{ "States", json::array({ { { "Name", "Move" }, { "BlendSpace", "Space" }, { "ParameterX", "Speed" } } }) },
		};
		const std::string clipGraph = project.WriteFile("Assets/Animations/Clip.banimgraph", UpperBodyGraph().dump());
		const std::string fadeGraph = project.WriteFile("Assets/Animations/Fade.banimgraph", UpperBodyGraph(0.5f).dump());
		const std::string spaceGraph = project.WriteFile("Assets/Animations/Space.banimgraph", spaces.dump());
		enum class Plays
		{
			StateClip,
			OutgoingClip,
			BlendSpace
		};
		for (const Plays plays : { Plays::StateClip, Plays::OutgoingClip, Plays::BlendSpace })
		{
			CAPTURE(static_cast<int>(plays));
			Scene scene;
			auto biped = AddBiped(scene, "Breathe");
			auto& animator = biped.GetComponent<AnimatorComponent>();
			animator.BlendSpaces = { { .Name = "Space", .Samples = { { .Clip = "Lean", .X = 0 }, { .Clip = "Wave", .X = 1 } } } };
			animator.Events = { { .Clip = "Lean", .Time = 0.05f, .Name = "Early" }, { .Clip = "Lean", .Time = 0.15f, .Name = "Late" } };
			animator.Layers = { GraphLayer("Upper", plays == Plays::StateClip ? clipGraph : plays == Plays::OutgoingClip ? fadeGraph
																														 : spaceGraph) };
			animator.Layers[0].Events = true;
			scene.OnRuntimeStart();
			auto& system = *scene.GetAnimationSystem();
			system.Update(0.01f);
			CHECK(system.TakeEvents().empty());
			if (plays == Plays::OutgoingClip)
				REQUIRE(system.SetParameter(biped, "Waving", true));
			// 0.08 s of the 0.2 s update shows: the clock passes Early and would pass Late, but the layer is gone by then.
			REQUIRE(system.SetLayerWeight(biped, "Upper", 0, 0.08f));
			system.Update(0.2f);
			const auto events = system.TakeEvents();
			REQUIRE(events.size() == 1);
			CHECK(events[0].Name == "Early");
			CHECK(events[0].Layer == "Upper");
			scene.OnRuntimeStop();
		}
	}
}
