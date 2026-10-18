#pragma once

#include "Basalt/Scene/Components.h"

#include <glm/glm.hpp>

#include <vector>

namespace Basalt {

	// Fills `outWeights` with the weight of each of the space's samples (in sample order) at `parameter`. The weights
	// are nonnegative and sum to 1, and a parameter on a sample gives that sample weight 1 (shared equally by samples at
	// the same position). 1D spaces interpolate linearly between the samples around X (clamping outside them) and
	// ignore Y; 2D spaces use gradient band interpolation (Johansen 2009; Unity's "2D Freeform Cartesian"), which needs no
	// triangulation and handles any sample layout. Empty for a space without samples.
	void ComputeBlendSpaceWeights(const AnimationBlendSpace& space, glm::vec2 parameter, std::vector<float>& outWeights);

}
