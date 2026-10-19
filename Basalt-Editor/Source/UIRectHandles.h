#pragma once

#include "Basalt/UI/UIGeometry.h"
#include "Basalt/UI/UIRules.h"

#include <algorithm>
#include <cmath>

namespace Basalt {

	// Rect handle math stays independent of ImGui so authoring gestures can be checked headlessly.
	inline UIRectComponent ResizeUIRect(const UIRectComponent& initial, const UIBox& box, const glm::vec2& handle, const glm::vec2& localDelta)
	{
		UIRectComponent result = initial;
		glm::vec2 sizeDelta(0.0f);
		for (int axis = 0; axis < 2; ++axis)
			if (handle[axis] != 0.5f)
				sizeDelta[axis] = std::max((handle[axis] * 2.0f - 1.0f) * localDelta[axis], 1.0f - box.Size[axis]);
		result.Size += sizeDelta;
		const glm::vec2 shift = (initial.Pivot - (glm::vec2(1.0f) - handle)) * sizeDelta * initial.Scale;
		const float c = std::cos(initial.Rotation), s = std::sin(initial.Rotation);
		result.Position += glm::vec2(c * shift.x - s * shift.y, s * shift.x + c * shift.y);
		return SanitizeUIRect(result);
	}

}
