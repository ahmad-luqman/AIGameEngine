#pragma once

#include <glm/glm.hpp>

#include <cstddef>

namespace Basalt {

	// Push constants of the separable SSAO blur (BlurPush in SSAOBlur.frag), shared by the scene renderer and the GPU
	// test that drives the blur pass on its own.
	struct SsaoBlurPush
	{
		glm::ivec2 Axis; // (1, 0) for the horizontal pass, (0, 1) for the vertical one
		float Near;
		float Far;
		float Orthographic;    // 1 for orthographic cameras (depth is already linear)
		float Padding;         // aligns RenderSize to 8 bytes, as std430 does
		glm::ivec2 RenderSize; // texels of the render area in the top-left of the targets
	};
	static_assert(sizeof(SsaoBlurPush) == 32);
	static_assert(offsetof(SsaoBlurPush, Near) == 8 && offsetof(SsaoBlurPush, RenderSize) == 24);

}
