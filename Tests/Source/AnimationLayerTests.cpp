#include <doctest/doctest.h>

#include "AnimationTestUtils.h"
#include "TestUtils.h"

#include <Basalt/Animation/AnimationPose.h>
#include <Basalt/Animation/AnimationSystem.h>
#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Asset/MeshImporter.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scripting/ScriptEngine.h>

using namespace Basalt;
using namespace BasaltTest;
using json = nlohmann::json;

using namespace AnimationTest;

namespace {
	const AnimationClip& Clip(const MeshSource& source, const std::string& name)
	{
		const auto clip = std::ranges::find(source.Animations, name, &AnimationClip::Name);
		REQUIRE(clip != source.Animations.end());
		return *clip;
	}
	AnimationLocalPose Sample(const MeshSource& source, const std::string& clip, float time)
	{
		return SampleAnimation(source, clip.empty() ? nullptr : &Clip(source, clip), time);
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
		// Each distinct problem logs once, however often a script repeats it, also between good calls on other layers.
		for (int i = 0; i < 3; i++)
		{
			CHECK_FALSE(system.SetLayerWeight(biped, "Wave", 1.5f, 0));
			CHECK(system.GetLayerWeight(biped, "Lean").has_value());
		}
		CHECK_FALSE(system.SetLayerWeight(biped, "Wave", 1, -1));
		CHECK_FALSE(system.SetLayerWeight(biped, "Wave", std::numeric_limits<float>::quiet_NaN(), 0));
		CHECK(CountLogMessages(since, "layer 'Wave': weight must be between 0 and 1") == 3);
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
		CHECK(CountLogMessages(since, "no layer named 'Lower'") == 1);
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
			system.Update(0.25f);
			// Walk bobs the hips to 1.01 m at 0.25 s; root motion leaves the base's hips in place.
			const auto& globals = *system.GetPose(biped);
			CHECK(globals[Node(*source, "Hips")][3].y == doctest::Approx(rootMotion ? 1.0f : 1.01f));
			CHECK(NearRotation(LocalRotation(system, biped, *source, "LeftUpperLeg"), AxisAngle(X, 0)));
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
		const AnimationLayerMask mask{ .Weights = { 1, 1 }, .Channels = { AnimatedTranslation | AnimatedRotation | AnimatedScale, 0 } };

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
		OverrideAnimationPose(overridden, layer, { .Weights = { 0.5f, 1 }, .Channels = { AnimatedScale, AnimatedRotation } }, 1);
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
		CHECK(CountLogMessages(since, "layer 'Wave' cannot play") == 1);
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
		CHECK(CountLogMessages(since, "Weight 3 must be between 0 and 1") == 1);
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
		system.Update(0.25f);
		// Walk bobs the hips, above the root joint Spine: the layer leaves them alone, and says why.
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
}
