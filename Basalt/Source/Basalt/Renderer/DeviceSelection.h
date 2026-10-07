#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Basalt {

	enum class PhysicalDeviceKind
	{
		Discrete,
		Integrated,
		Virtual,
		Cpu,
		Other
	};

	// What device selection needs to know about one Vulkan physical device, in enumeration order.
	// Kept free of Vulkan types so the policy can be unit-tested without a GPU.
	struct PhysicalDeviceInfo
	{
		std::string Name;
		PhysicalDeviceKind Kind = PhysicalDeviceKind::Other;
		// False when the device lacks something Basalt requires (Vulkan 1.3 features, a suitable queue).
		bool Suitable = false;
	};

	const char* ToString(PhysicalDeviceKind kind);

	// Picks the device to render with. An empty selector chooses automatically (discrete > integrated >
	// virtual/other > CPU, first in enumeration order on ties). Otherwise the selector names a device
	// explicitly (see the definition for the accepted forms). Returns the index into `devices`, or
	// std::nullopt with a message in outError; an explicit selector never falls back to another device.
	std::optional<size_t> SelectPhysicalDevice(std::span<const PhysicalDeviceInfo> devices, std::string_view selector, std::string& outError);

}
