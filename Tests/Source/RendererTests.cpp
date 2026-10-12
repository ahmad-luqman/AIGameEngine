#include <doctest/doctest.h>

#include <Basalt/Renderer/DeviceSelection.h>
#include <Basalt/Renderer/SceneLights.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>

#include <glm/gtc/epsilon.hpp>

#include <vector>

using namespace Basalt;

TEST_SUITE("DeviceSelection")
{
	// A laptop with hybrid graphics plus Mesa's software rasterizer, in a typical enumeration order.
	const std::vector<PhysicalDeviceInfo> HybridLaptop = {
		{ "Intel(R) UHD Graphics 630", PhysicalDeviceKind::Integrated, true },
		{ "NVIDIA GeForce RTX 3060 Laptop GPU", PhysicalDeviceKind::Discrete, true },
		{ "llvmpipe (LLVM 17.0.6, 256 bits)", PhysicalDeviceKind::Cpu, true },
	};

	TEST_CASE("Automatic selection prefers discrete, then integrated, then CPU")
	{
		std::string error;
		CHECK(SelectPhysicalDevice(HybridLaptop, "", error) == 1u);

		const std::vector<PhysicalDeviceInfo> noDiscrete = { HybridLaptop[2], HybridLaptop[0] };
		CHECK(SelectPhysicalDevice(noDiscrete, "", error) == 1u);

		// lavapipe alone (CI): a CPU device is still usable.
		const std::vector<PhysicalDeviceInfo> cpuOnly = { HybridLaptop[2] };
		CHECK(SelectPhysicalDevice(cpuOnly, "", error) == 0u);
	}

	TEST_CASE("Automatic selection skips unsuitable devices and keeps enumeration order on ties")
	{
		std::vector<PhysicalDeviceInfo> devices = HybridLaptop;
		devices[1].Suitable = false;
		std::string error;
		CHECK(SelectPhysicalDevice(devices, "", error) == 0u);

		const std::vector<PhysicalDeviceInfo> twoDiscrete = {
			{ "GPU A", PhysicalDeviceKind::Discrete, true },
			{ "GPU B", PhysicalDeviceKind::Discrete, true },
		};
		CHECK(SelectPhysicalDevice(twoDiscrete, "", error) == 0u);
	}

	TEST_CASE("Automatic selection fails with a message when nothing is suitable")
	{
		std::vector<PhysicalDeviceInfo> devices = HybridLaptop;
		for (PhysicalDeviceInfo& device : devices)
			device.Suitable = false;
		std::string error;
		CHECK_FALSE(SelectPhysicalDevice(devices, "", error).has_value());
		CHECK_FALSE(error.empty());
		CHECK_FALSE(SelectPhysicalDevice({}, "", error).has_value());
	}

	TEST_CASE("An index selects the device at that enumeration position")
	{
		std::string error;
		CHECK(SelectPhysicalDevice(HybridLaptop, "0", error) == 0u);
		CHECK(SelectPhysicalDevice(HybridLaptop, "2", error) == 2u);
		CHECK_FALSE(SelectPhysicalDevice(HybridLaptop, "3", error).has_value());
		CHECK(error.find("available") != std::string::npos);
		CHECK_FALSE(SelectPhysicalDevice(HybridLaptop, "99999999999999999999999", error).has_value());
	}

	TEST_CASE("A name selector is a case-insensitive substring that must match exactly one device")
	{
		std::string error;
		CHECK(SelectPhysicalDevice(HybridLaptop, "nvidia", error) == 1u);
		CHECK(SelectPhysicalDevice(HybridLaptop, "INTEL", error) == 0u);
		CHECK(SelectPhysicalDevice(HybridLaptop, "llvmpipe", error) == 2u);

		CHECK_FALSE(SelectPhysicalDevice(HybridLaptop, "radeon", error).has_value());
		CHECK(error.find("matches no GPU") != std::string::npos);

		// "g" appears in two names ("Graphics", "GeForce"): ambiguous, never "first match".
		CHECK_FALSE(SelectPhysicalDevice(HybridLaptop, "g", error).has_value());
		CHECK(error.find("ambiguous") != std::string::npos);
	}

	TEST_CASE("An explicit selector never falls back from an unsuitable device")
	{
		std::vector<PhysicalDeviceInfo> devices = HybridLaptop;
		devices[0].Suitable = false;
		std::string error;
		CHECK_FALSE(SelectPhysicalDevice(devices, "intel", error).has_value());
		CHECK(error.find("Intel") != std::string::npos);
		CHECK_FALSE(SelectPhysicalDevice(devices, "0", error).has_value());
	}
}

TEST_SUITE("SceneLights")
{
	Entity AddDirectionalLight(Scene & scene, UUID id, float intensity, bool castShadows)
	{
		Entity entity = scene.CreateEntityWithUUID(id, "Light");
		auto& light = entity.AddComponent<DirectionalLightComponent>();
		light.Intensity = intensity;
		light.CastShadows = castShadows;
		return entity;
	}

	std::vector<uint64_t> CollectIds(Scene & scene)
	{
		std::vector<uint64_t> ids;
		for (const DirectionalLightInfo& light : CollectDirectionalLights(scene))
			ids.push_back(static_cast<uint64_t>(light.ID));
		return ids;
	}

	// Regression: the renderer used to light the scene with the first directional light in ECS storage
	// order only, silently ignoring the others.
	TEST_CASE("Every directional light is collected, the shadow caster first, then by brightness and UUID")
	{
		Scene scene;
		AddDirectionalLight(scene, 10, 8.0f, false);
		AddDirectionalLight(scene, 20, 1.0f, true);
		AddDirectionalLight(scene, 30, 2.0f, false);
		AddDirectionalLight(scene, 5, 2.0f, false);
		AddDirectionalLight(scene, 40, 3.0f, true);

		CHECK(CollectIds(scene) == std::vector<uint64_t>{ 40, 20, 10, 5, 30 });
		const std::vector<DirectionalLightInfo> lights = CollectDirectionalLights(scene);
		CHECK(lights[0].CastShadows);
		CHECK(lights[0].Radiance == glm::vec3(3.0f));
	}

	TEST_CASE("The primary light does not depend on creation order, so a play copy keeps it")
	{
		auto scene = CreateRef<Scene>();
		AddDirectionalLight(*scene, 2, 1.0f, false);
		AddDirectionalLight(*scene, 1, 1.0f, false);
		CHECK(CollectIds(*scene) == std::vector<uint64_t>{ 1, 2 });

		Ref<Scene> copy = Scene::Copy(scene);
		CHECK(CollectIds(*copy) == CollectIds(*scene));
	}

	TEST_CASE("Directions come from the world transform and are normalized; collapsed lights are skipped")
	{
		Scene scene;
		Entity parent = scene.CreateEntityWithUUID(1, "Parent");
		parent.GetTransform().SetRotationEuler({ 0.0f, glm::radians(90.0f), 0.0f });
		parent.GetTransform().Scale = glm::vec3(3.0f);
		Entity light = AddDirectionalLight(scene, 2, 1.0f, true);
		scene.SetParent(light, parent, false);
		AddDirectionalLight(scene, 3, 5.0f, true).GetTransform().Scale = glm::vec3(0.0f);

		const std::vector<DirectionalLightInfo> lights = CollectDirectionalLights(scene);
		REQUIRE(lights.size() == 1);
		CHECK(lights[0].ID == UUID(2));
		// -Z rotated 90 degrees about +Y travels along -X.
		CHECK(glm::all(glm::epsilonEqual(lights[0].Direction, glm::vec3(-1.0f, 0.0f, 0.0f), 1e-5f)));
	}

	TEST_CASE("A scene without directional lights yields none")
	{
		Scene scene;
		scene.CreateEntity("Empty");
		CHECK(CollectDirectionalLights(scene).empty());
	}
}
