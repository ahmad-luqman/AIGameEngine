#pragma once

#include "Basalt/Scene/Components.h"

#include <glm/glm.hpp>

#include <vector>

namespace Basalt {

	// The weight of each of the space's samples (in sample order) at `parameter`. The weights are nonnegative and
	// sum to 1, and a parameter on a sample gives that sample weight 1. 1D spaces interpolate linearly between
	// the two samples around X (clamping outside them) and ignore Y; 2D spaces use gradient band interpolation
	// (Johansen 2009, "freeform cartesian"), which needs no triangulation and handles any sample layout. Empty
	// for a space without samples.
	std::vector<float> ComputeBlendSpaceWeights(const AnimationBlendSpace& space, glm::vec2 parameter);

}
