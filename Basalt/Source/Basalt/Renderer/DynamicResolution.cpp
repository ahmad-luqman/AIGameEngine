#include "Basalt/Renderer/DynamicResolution.h"

#include "Basalt/Renderer/RenderMath.h"

#include <algorithm>
#include <cmath>

namespace Basalt {

	namespace {

		bool IsPositive(float value)
		{
			return std::isfinite(value) && value > 0.0f;
		}

	}

	DynamicResolutionSettings SanitizeDynamicResolution(const DynamicResolutionSettings& settings)
	{
		DynamicResolutionSettings result = settings;
		const DynamicResolutionSettings defaults;
		result.TargetFrameRate = std::isnan(settings.TargetFrameRate) ? defaults.TargetFrameRate : std::max(settings.TargetFrameRate, MinTargetFrameRate);
		if (!std::isfinite(result.TargetFrameRate))
			result.TargetFrameRate = MinTargetFrameRate;
		result.MinScale = std::isnan(settings.MinScale) ? defaults.MinScale : std::clamp(settings.MinScale, MinRenderScale, MaxRenderScale);
		return result;
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
		m_HasCost = false;
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
		// Median of three; of two, the lower (a spike right after a reset is not a trend either).
		const auto& recent = m_RecentCosts;
		float cost = recent[0];
		if (m_RecentCount == 2)
			cost = std::min(recent[0], recent[1]);
		else if (m_RecentCount == 3)
			cost = std::max(std::min(recent[0], recent[1]), std::min(std::max(recent[0], recent[1]), recent[2]));
		// The first sample is taken as is, so an over-budget start drops at once instead of easing in.
		m_FullScaleCost = m_HasCost ? m_FullScaleCost + Smoothing * (cost - m_FullScaleCost) : cost;
		m_HasCost = true;

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
