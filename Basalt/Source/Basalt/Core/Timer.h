#pragma once

#include <chrono>

namespace Basalt {

	// Monotonic stopwatch.
	class Timer
	{
	public:
		Timer() { Reset(); }

		void Reset() { m_Start = std::chrono::steady_clock::now(); }

		float Elapsed() const
		{
			return std::chrono::duration<float>(std::chrono::steady_clock::now() - m_Start).count();
		}

		float ElapsedMillis() const { return Elapsed() * 1000.0f; }

	private:
		std::chrono::time_point<std::chrono::steady_clock> m_Start;
	};

}
