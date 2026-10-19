#include "Basalt/UI/UIDrawCommand.h"

#include "Basalt/UI/UIRules.h"

#include <cmath>
#include <algorithm>

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

		template<glm::length_t L>
		void Clamp(glm::vec<L, float, glm::defaultp>& value, float min, float max, const glm::vec<L, float, glm::defaultp>& fallback)
		{
			for (glm::length_t i = 0; i < L; i++)
				value[i] = ClampOrFallback(value[i], min, max, fallback[i]);
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
		for (glm::length_t col = 0; col < 3; ++col)
			if (!IsFinite(command.Transform[col]))
				return false;
		if (command.Clip && (!IsFinite(command.Clip->Min) || !IsFinite(command.Clip->Size)))
			return false;
		const UIDrawCommand defaults;
		constexpr float Limit = UIRectComponent::MaxMagnitude;
		Clamp(command.Position, -Limit, Limit, defaults.Position);
		Clamp(command.Size, 0.0f, Limit, defaults.Size);
		Clamp(command.Anchor, 0.0f, 1.0f, defaults.Anchor);
		Clamp(command.Pivot, 0.0f, 1.0f, defaults.Pivot);
		command.Rotation = ClampOrFallback(command.Rotation, -6283.185f, 6283.185f, 0.0f);
		Clamp(command.Scale, 0.001f, 1000.0f, defaults.Scale);
		Clamp(command.Align, 0.0f, 1.0f, defaults.Align);
		Clamp(command.Color, 0.0f, 1.0f, defaults.Color);
		Clamp(command.UVMin, 0.0f, 1.0f, defaults.UVMin);
		Clamp(command.UVMax, 0.0f, 1.0f, defaults.UVMax);
		command.SpriteColumns = std::clamp(command.SpriteColumns, 1u, 4096u);
		command.SpriteRows = std::clamp(command.SpriteRows, 1u, 4096u);
		command.SpriteFrame = std::min(command.SpriteFrame, command.SpriteColumns * command.SpriteRows - 1);
		Clamp(command.Slice, 0.0f, UIImageComponent::MaxSlice, defaults.Slice);
		command.SliceScale = ClampOrFallback(command.SliceScale, UIImageComponent::MinSliceScale, UIImageComponent::MaxSliceScale, defaults.SliceScale);
		command.FontSize = ClampOrFallback(command.FontSize, UITextComponent::MinFontSize, UITextComponent::MaxFontSize, defaults.FontSize);
		return true;
	}

}
