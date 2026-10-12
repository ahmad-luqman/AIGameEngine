#include "Basalt/Renderer/DynamicResolution.h"

#include "Basalt/Core/Log.h" // fmt::format (bundled with spdlog)
#include "Basalt/Renderer/RenderMath.h"

#include <algorithm>
#include <cmath>

namespace Basalt {

	namespace {

		bool IsPositive(float value)
		{
			return std::isfinite(value) && value > 0.0f;
		}

		float MedianOfThree(float a, float b, float c)
		{
			return std::max(std::min(a, b), std::min(std::max(a, b), c));
		}

	}

	DynamicResolutionSettings SanitizeDynamicResolution(const DynamicResolutionSettings& settings)
	{
		DynamicResolutionSettings result = settings;
		const DynamicResolutionSettings defaults;
		if (std::isnan(settings.TargetFrameRate))
			result.TargetFrameRate = defaults.TargetFrameRate;
		else
			result.TargetFrameRate = std::clamp(settings.TargetFrameRate, MinTargetFrameRate, UnreachableTargetFrameRate);
		result.MinScale = std::isnan(settings.MinScale) ? defaults.MinScale : std::clamp(settings.MinScale, MinRenderScale, MaxRenderScale);
		return result;
	}

	DynamicResolutionState GetDynamicResolutionState(const DynamicResolutionSettings& settings, float renderScale, bool gpuTimers)
	{
		if (!settings.Enabled)
			return DynamicResolutionState::Off;
		if (!gpuTimers)
			return DynamicResolutionState::NoGpuTimers;
		if (settings.MinScale >= renderScale)
			return DynamicResolutionState::EmptyRange;
		return DynamicResolutionState::Active;
	}

	std::string DescribeDynamicResolutionState(DynamicResolutionState state, const DynamicResolutionSettings& settings, float renderScale)
	{
		switch (state)
		{
			case DynamicResolutionState::Off:
				return "off";
			case DynamicResolutionState::NoGpuTimers:
				return "inactive: needs GPU timer queries, which this renderer does not have";
			case DynamicResolutionState::EmptyRange:
				return fmt::format("inactive: nothing to adjust, the minimum render scale ({}) is not below the render scale ({})", settings.MinScale, renderScale);
			case DynamicResolutionState::Active:
				return "active";
		}
		return "unknown";
	}

	void DynamicResolutionController::Reset(float scale)
	{
		m_Scale = scale;
		ForgetHistory();
	}

	void DynamicResolutionController::ForgetHistory()
	{
		m_RecentCount = 0;
		m_RecentNext = 0;
		m_FullScaleCost = 0.0f;
		m_RaiseSamples = 0;
	}

	float DynamicResolutionController::GetScale(float minScale, float maxScale) const
	{
		return std::clamp(m_Scale, std::min(minScale, maxScale), maxScale);
	}

	bool DynamicResolutionController::AddSample(float gpuMilliseconds, float renderedScale, float budgetMilliseconds, float minScale, float maxScale)
	{
		if (!IsPositive(gpuMilliseconds) || !IsPositive(renderedScale) || !IsPositive(budgetMilliseconds))
			return false;
		const float low = std::min(minScale, maxScale);
		m_Scale = GetScale(minScale, maxScale);

		m_RecentCosts[m_RecentNext] = gpuMilliseconds / (renderedScale * renderedScale);
		m_RecentNext = (m_RecentNext + 1) % static_cast<uint32_t>(m_RecentCosts.size());
		m_RecentCount = std::min(m_RecentCount + 1, static_cast<uint32_t>(m_RecentCosts.size()));
		// Median of three; of two, the lower (a spike right after a reset is not a trend either). ForgetHistory
		// restarts the ring at index 0, so the first samples are always at the front.
		float cost = m_RecentCosts[0];
		if (m_RecentCount == 2)
			cost = std::min(m_RecentCosts[0], m_RecentCosts[1]);
		else if (m_RecentCount == 3)
			cost = MedianOfThree(m_RecentCosts[0], m_RecentCosts[1], m_RecentCosts[2]);
		// The first sample is taken as is, so an over-budget start drops at once instead of easing in.
		m_FullScaleCost = m_RecentCount > 1 ? m_FullScaleCost + Smoothing * (cost - m_FullScaleCost) : cost;

		const float target = std::clamp(std::sqrt(budgetMilliseconds * Headroom / m_FullScaleCost), low, maxScale);
		// Changes within the deadband are skipped, except the last step onto a bound (clamp returns the bound
		// exactly), so the bounds stay reachable.
		const bool lower = target < m_Scale && (m_Scale - target > Deadband || target == low);
		const bool higher = target > m_Scale && (target - m_Scale > Deadband || target == maxScale);
		if (lower)
		{
			m_Scale = target;
			m_RaiseSamples = 0;
		}
		else if (higher)
		{
			if (++m_RaiseSamples >= RaiseDelay)
			{
				m_Scale = std::min(target, m_Scale + MaxRaiseStep);
				m_RaiseSamples = 0;
			}
		}
		else
		{
			m_RaiseSamples = 0;
		}
		return true;
	}

}
