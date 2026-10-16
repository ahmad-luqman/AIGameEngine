#pragma once

#include "Basalt/Core/UUID.h"

#include <string>

namespace Basalt {
	class Scene;
	struct PostProcessComponent;
	enum class Tonemapper
	{
		None = 0,
		Reinhard,
		ACES,
		AgX
	};
	inline constexpr const char* TonemapperNames[] = { "None", "Reinhard", "ACES", "AgX" };
	// Shared validation for authored components and defensive renderer consumption.
	std::string ValidatePostProcess(const PostProcessComponent& settings);
	// Highest Priority enabled component wins; equal priorities use the lowest UUID. No volumes/blending.
	PostProcessComponent ResolvePostProcess(const Scene& scene, UUID* selected = nullptr);
}
