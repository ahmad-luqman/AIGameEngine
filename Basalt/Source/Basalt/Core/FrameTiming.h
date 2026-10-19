#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace Basalt {

	// Rolling host-frame latency, independent of the simulation timestep and its stall clamp.
	class FrameTiming
	{
	public:
		static constexpr size_t Window = 60;

		void Add(float milliseconds)
		{
			if (!std::isfinite(milliseconds) || milliseconds <= 0)
				return;
			m_Samples[m_Next] = milliseconds;
			m_Next = (m_Next + 1) % Window;
			if (m_Count < Window)
				++m_Count;
		}
		float GetMilliseconds() const
		{
			double sum = 0;
			for (size_t i = 0; i < m_Count; ++i)
				sum += m_Samples[i];
			return m_Count ? static_cast<float>(sum / static_cast<double>(m_Count)) : 0.0f;
		}

	private:
		std::array<float, Window> m_Samples{};
		size_t m_Count = 0;
		size_t m_Next = 0;
	};

}
