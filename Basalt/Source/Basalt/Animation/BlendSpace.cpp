#include "Basalt/Animation/BlendSpace.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace Basalt {

	namespace {

		std::vector<float> Weights1D(const std::vector<AnimationBlendSample>& samples, float x)
		{
			std::vector<size_t> order(samples.size());
			std::iota(order.begin(), order.end(), size_t(0));
			std::ranges::stable_sort(order, {}, [&](size_t i) { return samples[i].X; });
			std::vector<float> weights(samples.size(), 0.0f);
			if (x <= samples[order.front()].X)
			{
				weights[order.front()] = 1.0f;
				return weights;
			}
			if (x >= samples[order.back()].X)
			{
				weights[order.back()] = 1.0f;
				return weights;
			}
			for (size_t i = 1; i < order.size(); i++)
			{
				const AnimationBlendSample& low = samples[order[i - 1]];
				const AnimationBlendSample& high = samples[order[i]];
				if (x > high.X)
					continue;
				const float t = (x - low.X) / (high.X - low.X);
				weights[order[i - 1]] = 1.0f - t;
				weights[order[i]] = t;
				break;
			}
			return weights;
		}

		std::vector<float> Weights2D(const std::vector<AnimationBlendSample>& samples, glm::vec2 point)
		{
			std::vector<float> weights(samples.size(), 0.0f);
			float total = 0.0f;
			for (size_t i = 0; i < samples.size(); i++)
			{
				const glm::vec2 origin(samples[i].X, samples[i].Y);
				// A sample's influence falls to 0 at every other sample, along the line towards it.
				float weight = 1.0f;
				for (size_t j = 0; j < samples.size() && weight > 0.0f; j++)
				{
					if (j == i)
						continue;
					const glm::vec2 edge = glm::vec2(samples[j].X, samples[j].Y) - origin;
					const float lengthSquared = glm::dot(edge, edge);
					if (lengthSquared <= 0.0f)
						continue;
					weight = std::min(weight, std::clamp(1.0f - glm::dot(point - origin, edge) / lengthSquared, 0.0f, 1.0f));
				}
				weights[i] = weight;
				total += weight;
			}
			if (total > 0.0f && std::isfinite(total))
			{
				for (float& weight : weights)
					weight /= total;
				return weights;
			}
			// Should not happen for distinct samples; kept so a degenerate layout still plays the nearest clip.
			size_t nearest = 0;
			float best = INFINITY;
			for (size_t i = 0; i < samples.size(); i++)
			{
				const glm::vec2 offset = point - glm::vec2(samples[i].X, samples[i].Y);
				if (const float distance = glm::dot(offset, offset); distance < best)
				{
					best = distance;
					nearest = i;
				}
			}
			std::ranges::fill(weights, 0.0f);
			weights[nearest] = 1.0f;
			return weights;
		}

	}

	std::vector<float> ComputeBlendSpaceWeights(const AnimationBlendSpace& space, glm::vec2 parameter)
	{
		if (space.Samples.empty())
			return {};
		if (space.Type == AnimationBlendSpaceType::OneD)
			return Weights1D(space.Samples, parameter.x);
		return Weights2D(space.Samples, parameter);
	}

}
