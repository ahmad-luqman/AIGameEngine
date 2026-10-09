#pragma once

namespace Basalt {

	// How a contact combines the two bodies' friction (or restitution). When the bodies ask for different
	// modes, the one listed later wins, so Max beats every other mode and Default defers to the other body.
	// The order is part of the behaviour: CombinePriority, not the enum's values, decides the winner.
	enum class PhysicsCombineMode
	{
		Default = 0,   // Jolt's: the geometric mean for friction, the larger value for restitution
		GeometricMean, // sqrt(a * b)
		Average,
		Min,
		Multiply,
		Max
	};

	// Which mode wins when two bodies' modes differ (higher wins).
	constexpr int CombinePriority(PhysicsCombineMode mode)
	{
		switch (mode)
		{
			case PhysicsCombineMode::Default:
				return 0;
			case PhysicsCombineMode::GeometricMean:
				return 1;
			case PhysicsCombineMode::Average:
				return 2;
			case PhysicsCombineMode::Min:
				return 3;
			case PhysicsCombineMode::Multiply:
				return 4;
			case PhysicsCombineMode::Max:
				return 5;
		}
		return 0;
	}

	// Combine the friction or restitution values of two touching bodies. When the modes differ, the one
	// with the higher CombinePriority wins; when both are Default, friction uses the geometric mean and
	// restitution the larger value.
	float CombineFriction(PhysicsCombineMode modeA, float a, PhysicsCombineMode modeB, float b);
	float CombineRestitution(PhysicsCombineMode modeA, float a, PhysicsCombineMode modeB, float b);

}
