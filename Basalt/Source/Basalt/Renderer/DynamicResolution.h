#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace Basalt {

	// Range of DynamicResolutionSettings::TargetFrameRate in project files and the automation API.
	constexpr float MinTargetFrameRate = 10.0f;
	constexpr float MaxTargetFrameRate = 1000.0f;
	// A target no GPU reaches, which pins the lowest scale. The runtime's --dynamic-resolution accepts rates up
	// to it (the GPU tests use it), and sanitizing maps higher rates and +infinity to it.
	constexpr float UnreachableTargetFrameRate = 1e6f;

	// Dynamic resolution settings (a project setting, next to RenderScale). Project files spell them
	// DynamicResolution / TargetFrameRate / MinRenderScale; the automation API (project.set, project.info,
	// render.stats) dynamicResolution / targetFrameRate / minRenderScale.
	struct DynamicResolutionSettings
	{
		bool Enabled = false;
		// Dynamic resolution aims the scene's GPU time (without the environment bake and uploads) at
		// DynamicResolutionController::Headroom (90%) of 1000 / TargetFrameRate ms. CPU time, the editor UI and
		// vsync are not measured, so this neither caps nor guarantees the frame rate.
		float TargetFrameRate = 60.0f;
		// Lowest render scale, in [Basalt::MinRenderScale, Basalt::MaxRenderScale] (RenderMath.h). The project
		// RenderScale is the highest; with a minimum at or above it there is nothing to adjust (EmptyRange).
		float MinScale = 0.5f;

		bool operator==(const DynamicResolutionSettings&) const = default;
	};

	// The settings with every value usable: the frame rate is clamped to [MinTargetFrameRate,
	// UnreachableTargetFrameRate], the minimum scale to the render scale range, and NaN values become the
	// defaults. Inputs are validated where they enter (project files, commands, command line); this is the
	// renderer's backstop for programmatic callers.
	DynamicResolutionSettings SanitizeDynamicResolution(const DynamicResolutionSettings& settings);

	// Whether dynamic resolution runs, and why not.
	enum class DynamicResolutionState
	{
		Off,         // not enabled
		NoGpuTimers, // enabled, but the renderer cannot time the GPU
		EmptyRange,  // enabled, but the minimum scale is not below the render scale
		Active
	};
	DynamicResolutionState GetDynamicResolutionState(const DynamicResolutionSettings& settings, float renderScale, bool gpuTimers);
	// "off", "active", or why it is inactive (for logs, render.stats, the editor and project.set warnings).
	std::string DescribeDynamicResolutionState(DynamicResolutionState state, const DynamicResolutionSettings& settings, float renderScale);

	// Picks the render scale that keeps the scene's GPU time within a budget. GPU time is modelled as
	// proportional to the pixel count (the scale squared), so each sample becomes the cost of a full-scale
	// frame; the median of the last three (one slow frame is not a trend) is smoothed, and the scale whose
	// predicted cost fits the budget (with Headroom) is the target. The first sample after Reset or
	// ForgetHistory (a resize, a new budget) is taken as is, so an over-budget start drops at once.
	// The scale drops as soon as the target is lower by more than Deadband (or is the minimum), and rises only
	// after RaiseDelay samples in a row with room to spare, in steps of at most MaxRaiseStep, which limits
	// oscillation and overshoot. Scale-independent costs (shadow maps, the output-size tonemap) make the model
	// optimistic when dropping and conservative when rising; repeated samples converge anyway, because a scale
	// that is still too slow produces a higher normalized cost. Headless; SceneRenderer feeds it timer query
	// results.
	class DynamicResolutionController
	{
	public:
		// Fraction of the budget the controller aims for, leaving room for frame-to-frame variance.
		static constexpr float Headroom = 0.9f;
		// Weight of a new sample in the smoothed cost.
		static constexpr float Smoothing = 0.25f;
		// Smaller changes (render-scale units) are ignored, because each change moves the sampling grid, which
		// shows as a visible pop; except the last step onto a bound.
		static constexpr float Deadband = 0.02f;
		// Samples in a row with room to spare before the scale rises.
		static constexpr uint32_t RaiseDelay = 30;
		// Largest rise per step, in render-scale units.
		static constexpr float MaxRaiseStep = 0.1f;

		// Forgets the measurements; frames render at `scale` until samples arrive.
		void Reset(float scale);
		// Forgets the measurements but keeps the current scale (the budget or the viewport changed, so old
		// costs no longer apply, but jumping back to the highest scale would cost a slow frame).
		void ForgetHistory();
		// Feeds the GPU time of one frame and the scale it rendered at. Returns false, ignoring the sample,
		// when the time, scale or budget is not positive and finite. Pass the same bounds as to GetScale.
		bool AddSample(float gpuMilliseconds, float renderedScale, float budgetMilliseconds, float minScale, float maxScale);
		// The scale for the next frame, within [minScale, maxScale] (minScale is capped at maxScale).
		float GetScale(float minScale, float maxScale) const;

	private:
		float m_Scale = 1.0f;
		// The last full-scale costs, for the median.
		std::array<float, 3> m_RecentCosts{};
		uint32_t m_RecentCount = 0;
		uint32_t m_RecentNext = 0;
		// Smoothed GPU milliseconds of a frame at scale 1 (valid once m_RecentCount > 0).
		float m_FullScaleCost = 0.0f;
		uint32_t m_RaiseSamples = 0;
	};

}
