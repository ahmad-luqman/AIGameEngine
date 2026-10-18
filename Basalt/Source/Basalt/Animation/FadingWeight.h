#pragma once

#include <algorithm>
#include <cmath>

namespace Basalt {

	// Whether a script's weight (0..1) and fade time (finite, nonnegative) are valid for FadingWeight::FadeTo.
	inline bool IsValidFade(float weight, float seconds)
	{
		return std::isfinite(weight) && weight >= 0 && weight <= 1 && std::isfinite(seconds) && seconds >= 0;
	}

	// A 0..1 weight (animation layers, IK chains) that fades linearly toward a target. Its configured value comes from a
	// component and a changed one applies at once, ending a fade; scripts fade it. Every input is clamped to 0..1 (a
	// non-finite one becomes 0), so the weight always is.
	class FadingWeight
	{
	public:
		explicit FadingWeight(float configured = 1.0f)
			: m_Configured(Clamp(configured))
			, m_Value(m_Configured)
			, m_Target(m_Configured)
		{
		}

		void Configure(float configured)
		{
			configured = Clamp(configured);
			// Exact: both sides are clamped values, so an unchanged configuration compares equal.
			if (configured == m_Configured)
				return;
			m_Configured = m_Value = m_Target = configured;
			m_Rate = 0;
		}

		// Seconds 0 (or not finite) sets the weight at once.
		void FadeTo(float target, float seconds)
		{
			m_Target = Clamp(target);
			const float rate = std::abs(m_Target - m_Value) / seconds;
			if (std::isfinite(rate) && rate > 0 && seconds > 0)
				m_Rate = rate;
			else
			{
				m_Value = m_Target;
				m_Rate = 0;
			}
		}

		void Advance(float seconds)
		{
			if (m_Rate <= 0 || !(seconds > 0))
				return;
			const float step = seconds * m_Rate;
			// min/max land on the target exactly, which ends the fade.
			m_Value = m_Value < m_Target ? std::min(m_Value + step, m_Target) : std::max(m_Value - step, m_Target);
			if (m_Value == m_Target)
				m_Rate = 0;
		}

		float Get() const { return m_Value; }
		bool IsFading() const { return m_Rate > 0; }

	private:
		static float Clamp(float weight) { return std::isfinite(weight) ? std::clamp(weight, 0.0f, 1.0f) : 0.0f; }

		float m_Configured;
		float m_Value;
		float m_Target;
		// Weight per second while fading; 0 when not fading.
		float m_Rate = 0;
	};

}
