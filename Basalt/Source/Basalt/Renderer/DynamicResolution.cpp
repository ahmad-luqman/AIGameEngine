#include "Basalt/Renderer/DynamicResolution.h"

#include <algorithm>
#include <cmath>

namespace Basalt {

	namespace {

		bool IsPositive(float value)
		{
			return std::isfinite(value) && value > 0.0f;
		}

	}

	void DynamicResolutionController::Reset(float scale)
	{
		m_Scale = scale;
		m_FullScaleCost = 0.0f;
		m_HasCost = false;
		m_RaiseSamples = 0;
	}

	float DynamicResolutionController::GetScale(float minScale, float maxScale) const
	{
		return std::clamp(m_Scale, std::min(minScale, maxScale), maxScale);
	}

	void DynamicResolutionController::AddSample(float gpuMilliseconds, float renderedScale, float budgetMilliseconds, float minScale, float maxScale)
	{
		if (!IsPositive(gpuMilliseconds) || !IsPositive(renderedScale) || !IsPositive(budgetMilliseconds))
			return;
		const float low = std::min(minScale, maxScale);
		m_Scale = GetScale(minScale, maxScale);

		const float cost = gpuMilliseconds / (renderedScale * renderedScale);
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
	}

}
