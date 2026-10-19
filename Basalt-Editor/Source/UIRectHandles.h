#pragma once

#include "Basalt/Automation/ViewportSettings.h"
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

	// Applies one gesture from its initial state. ParentTransform maps parent layout coordinates to canvas;
	// its inverse must be usable. A resize handle quantizes displacement before resizing, keeping the opposite
	// edge fixed even when rotation, stretched anchors and snapping are combined. A zero snapStep disables snap.
	inline UIRectComponent TransformUIRect(const UIRectComponent& initial, const UIBox& box, const glm::mat3& parentTransform, const glm::vec2& start, const glm::vec2& current, GizmoOperation operation, float snapStep = 0.0f, std::optional<glm::vec2> resizeHandle = std::nullopt)
	{
		const glm::mat3 inverse = glm::inverse(parentTransform);
		const bool snap = snapStep > 0.0f && std::isfinite(snapStep);
		if (resizeHandle)
		{
			glm::vec2 delta = glm::vec2(glm::inverse(parentTransform * ComputeUITransform(box, initial)) * glm::vec3(current - start, 0.0f));
			if (snap)
				delta = glm::round(delta / snapStep) * snapStep;
			return ResizeUIRect(initial, box, *resizeHandle, delta);
		}
		UIRectComponent result = initial;
		if (operation == GizmoOperation::Translate)
		{
			result.Position += glm::vec2(inverse * glm::vec3(current - start, 0.0f));
			if (snap)
				result.Position = glm::round(result.Position / snapStep) * snapStep;
		}
		else
		{
			const glm::vec2 pivot = box.Min + box.Size * initial.Pivot;
			const glm::vec2 before = glm::vec2(inverse * glm::vec3(start, 1.0f)) - pivot;
			const glm::vec2 after = glm::vec2(inverse * glm::vec3(current, 1.0f)) - pivot;
			if (operation == GizmoOperation::Rotate && glm::length(before) > 1e-3f && glm::length(after) > 1e-3f)
			{
				result.Rotation += std::atan2(after.y, after.x) - std::atan2(before.y, before.x);
				if (snap)
					result.Rotation = glm::radians(std::round(glm::degrees(result.Rotation) / snapStep) * snapStep);
			}
			else if (operation == GizmoOperation::Scale && glm::length(before) > 1e-3f)
			{
				result.Scale *= std::max(glm::length(after) / glm::length(before), 0.01f);
				if (snap)
					result.Scale = glm::max(glm::round(result.Scale / snapStep) * snapStep, glm::vec2(0.01f));
			}
		}
		return SanitizeUIRect(result);
	}

}
