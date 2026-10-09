#include "Basalt/Physics/PhysicsMaterial.h"

#include <algorithm>
#include <cmath>

namespace Basalt {

	namespace {

		float CombineMaterial(PhysicsCombineMode modeA, float a, PhysicsCombineMode modeB, float b, PhysicsCombineMode fallback)
		{
			PhysicsCombineMode mode = CombinePriority(modeA) >= CombinePriority(modeB) ? modeA : modeB;
			if (mode == PhysicsCombineMode::Default)
				mode = fallback;
			switch (mode)
			{
				case PhysicsCombineMode::Default:
				case PhysicsCombineMode::GeometricMean:
					return std::sqrt(std::max(a * b, 0.0f));
				case PhysicsCombineMode::Average:
					return 0.5f * (a + b);
				case PhysicsCombineMode::Min:
					return std::min(a, b);
				case PhysicsCombineMode::Multiply:
					return a * b;
				case PhysicsCombineMode::Max:
					return std::max(a, b);
			}
			return std::max(a, b);
		}

	}

	float CombineFriction(PhysicsCombineMode modeA, float a, PhysicsCombineMode modeB, float b)
	{
		return CombineMaterial(modeA, a, modeB, b, PhysicsCombineMode::GeometricMean);
	}

	float CombineRestitution(PhysicsCombineMode modeA, float a, PhysicsCombineMode modeB, float b)
	{
		return CombineMaterial(modeA, a, modeB, b, PhysicsCombineMode::Max);
	}

}
