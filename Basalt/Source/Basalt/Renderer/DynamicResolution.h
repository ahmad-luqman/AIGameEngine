#pragma once

#include <cstdint>

namespace Basalt {

	// Range of DynamicResolutionSettings::TargetFrameRate in project files and the automation API.
	constexpr float MinTargetFrameRate = 10.0f;
	constexpr float MaxTargetFrameRate = 1000.0f;

	// Dynamic resolution settings (a project setting, next to RenderScale).
	struct DynamicResolutionSettings
	{
		bool Enabled = false;
		// The scene's GPU work must fit in one frame at this rate.
		float TargetFrameRate = 60.0f;
		// Lowest render scale, in [MinRenderScale, MaxRenderScale] (RenderMath.h). The project RenderScale is the
		// highest; a minimum above it acts as RenderScale.
		float MinRenderScale = 0.5f;

		bool operator==(const DynamicResolutionSettings&) const = default;
	};

	// Picks the render scale that keeps the scene's GPU time within a budget. GPU time is modelled as
	// proportional to the pixel count (the scale squared), so each sample becomes the cost of a full-scale
	// frame, which is smoothed; the scale whose predicted cost fits the budget (with Headroom) is the target.
	// The scale drops as soon as the target is lower and rises only after RaiseDelay samples in a row with
	// room to spare, in steps of at most MaxRaiseStep, so it neither oscillates nor overshoots into a slow
	// frame. Fixed costs (shadow maps) make the model optimistic; repeated samples converge anyway, because a
	// scale that is still too slow produces a higher normalized cost. Headless; SceneRenderer feeds it timer
	// query results.
	class DynamicResolutionController
	{
	public:
		// Fraction of the budget the controller aims for, leaving room for frame-to-frame variance.
		static constexpr float Headroom = 0.9f;
		// Weight of a new sample in the smoothed cost.
		static constexpr float Smoothing = 0.25f;
		// Smaller changes are ignored (each change resamples the image), except to reach a bound.
		static constexpr float Deadband = 0.02f;
		static constexpr uint32_t RaiseDelay = 30;
		static constexpr float MaxRaiseStep = 0.1f;

		// Forgets the measurements; frames render at `scale` until samples arrive.
		void Reset(float scale);
		// Feeds the GPU time of one frame and the scale it rendered at. Samples that are not positive and
		// finite are ignored, as are non-positive budgets.
		void AddSample(float gpuMilliseconds, float renderedScale, float budgetMilliseconds, float minScale, float maxScale);
		// The scale for the next frame, within [minScale, maxScale] (minScale is capped at maxScale).
		float GetScale(float minScale, float maxScale) const;

	private:
		float m_Scale = 1.0f;
		// Smoothed GPU milliseconds of a frame at scale 1.
		float m_FullScaleCost = 0.0f;
		bool m_HasCost = false;
		uint32_t m_RaiseSamples = 0;
	};

}
