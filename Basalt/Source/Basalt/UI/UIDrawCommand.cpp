#include "Basalt/UI/UIDrawCommand.h"

#include "Basalt/Scene/Components.h"

#include <algorithm>
#include <cmath>

namespace Basalt {

	namespace {

		template<glm::length_t L>
		bool IsFinite(const glm::vec<L, float, glm::defaultp>& value)
		{
			for (glm::length_t i = 0; i < L; i++)
			{
				if (!std::isfinite(value[i]))
					return false;
			}
			return true;
		}

		// Non-finite values become `fallback`, the rest are clamped.
		float Clamp(float value, float min, float max, float fallback)
		{
			return std::isfinite(value) ? std::clamp(value, min, max) : fallback;
		}

		template<glm::length_t L>
		void Clamp(glm::vec<L, float, glm::defaultp>& value, float min, float max, float fallback)
		{
			for (glm::length_t i = 0; i < L; i++)
				value[i] = Clamp(value[i], min, max, fallback);
		}

	}

	UIBox ResolveUIBox(const UIDrawCommand& command, const glm::vec2& canvasSize)
	{
		return { canvasSize * command.Anchor + command.Position - command.Pivot * command.Size, command.Size };
	}

	bool SanitizeUIDrawCommand(UIDrawCommand& command)
	{
		if (!IsFinite(command.Anchor) || !IsFinite(command.Position) || !IsFinite(command.Size) || !IsFinite(command.Pivot))
			return false;
		command.Size = glm::max(command.Size, glm::vec2(0.0f));
		Clamp(command.Color, 0.0f, 1.0f, 1.0f);
		Clamp(command.Slice, 0.0f, UIImageComponent::MaxSlice, 0.0f);
		command.SliceScale = Clamp(command.SliceScale, 1e-3f, UIImageComponent::MaxSliceScale, 1.0f);
		command.FontSize = Clamp(command.FontSize, UITextComponent::MinFontSize, UITextComponent::MaxFontSize, 32.0f);
		Clamp(command.Align, 0.0f, 1.0f, 0.0f);
		return true;
	}

}
