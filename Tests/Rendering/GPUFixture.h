#pragma once

#include <doctest/doctest.h>

#include <Basalt/Core/Log.h>
#include <Basalt/Renderer/GraphicsDevice.h>
#include <Basalt/Renderer/VulkanLoader.h>

// Offscreen fixture shared by GPU tests; missing Vulkan or validation is a failure, not a skip.
inline Basalt::Scope<Basalt::GraphicsDevice> CreateTestGraphics()
{
	Basalt::Log::Init();
	REQUIRE(Basalt::VulkanLoader::Load());
	Basalt::GraphicsDeviceSpecification spec;
	spec.EnableValidation = true;
	auto graphics = Basalt::GraphicsDevice::CreateOffscreen(64, 64, spec);
	REQUIRE(graphics);
	REQUIRE(graphics->IsValidationActive());
	return graphics;
}
