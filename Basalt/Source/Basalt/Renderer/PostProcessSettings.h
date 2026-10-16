#pragma once

#include "Basalt/Core/UUID.h"

#include <iterator>
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
	inline constexpr size_t TonemapperCount = 4;
	static_assert(std::size(TonemapperNames) == TonemapperCount);
	const char* TonemapperName(Tonemapper tonemapper);
	inline constexpr const char* PostProcessOverrideKeys[] = { "Exposure", "Tonemapper", "SSAOEnabled", "SSAORadius", "SSAOIntensity", "SSAOBias" };
	// The six fields shared by legacy Renderer settings and an authored PostProcess component.
	template<typename Source, typename Target>
	void CopyPostProcessSettings(const Source& source, Target& target)
	{
		target.Exposure = source.Exposure;
		target.Tonemap = source.Tonemap;
		target.SSAOEnabled = source.SSAOEnabled;
		target.SSAORadius = source.SSAORadius;
		target.SSAOIntensity = source.SSAOIntensity;
		target.SSAOBias = source.SSAOBias;
	}
	// Shared validation for authored components and defensive renderer consumption.
	std::string ValidatePostProcess(const PostProcessComponent& settings);
	// Highest Priority enabled component wins; equal priorities use the lowest UUID. No volumes/blending.
	// Writes zero to selected when falling back to legacy settings (entity UUID zero is reserved).
	PostProcessComponent ResolvePostProcess(const Scene& scene, UUID* selected = nullptr);
}
