#pragma once

#include <doctest/doctest.h>

#include <Breccia/Core/Log.h>
#include <Breccia/Renderer/GraphicsDevice.h>
#include <Breccia/Renderer/VulkanLoader.h>

// Offscreen fixture shared by GPU tests; missing Vulkan or validation is a failure, not a skip.
inline Breccia::Scope<Breccia::GraphicsDevice> CreateTestGraphics()
{
	Breccia::Log::Init();
	REQUIRE(Breccia::VulkanLoader::Load());
	Breccia::GraphicsDeviceSpecification spec;
	spec.EnableValidation = true;
	auto graphics = Breccia::GraphicsDevice::CreateOffscreen(64, 64, spec);
	REQUIRE(graphics);
	REQUIRE(graphics->IsValidationActive());
	return graphics;
}
