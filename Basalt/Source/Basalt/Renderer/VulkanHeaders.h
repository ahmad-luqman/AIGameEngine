#pragma once

// Single include point for Vulkan in Basalt. The dynamic dispatcher must be configured identically in
// every translation unit (and match nvrhi's Vulkan backend), so never include <vulkan/vulkan.hpp> directly.

#ifndef VULKAN_HPP_DISPATCH_LOADER_DYNAMIC
	#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#endif

#include <vulkan/vulkan.hpp>
