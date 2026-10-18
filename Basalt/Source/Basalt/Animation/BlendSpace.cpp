#include "Basalt/Animation/BlendSpace.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Basalt {

	namespace {

		// Gives `weight` to the samples at position `x`, split equally among them.
		void Share(const std::vector<AnimationBlendSample>& samples, float x, float weight, std::vector<float>& weights)
		{
			const auto count = std::ranges::count(samples, x, &AnimationBlendSample::X);
			for (size_t i = 0; i < samples.size(); i++)
				if (samples[i].X == x)
					weights[i] += weight / static_cast<float>(count);
		}

		void Weights1D(const std::vector<AnimationBlendSample>& samples, float x, std::vector<float>& weights)
		{
			// The nearest positions at or below and at or above x; a missing side clamps to the other.
			float below = -std::numeric_limits<float>::infinity();
			float above = std::numeric_limits<float>::infinity();
			for (const AnimationBlendSample& sample : samples)
			{
				if (sample.X <= x)
					below = std::max(below, sample.X);
				if (sample.X >= x)
					above = std::min(above, sample.X);
			}
			if (std::isinf(below) || below == above)
				Share(samples, above, 1.0f, weights);
			else if (std::isinf(above))
				Share(samples, below, 1.0f, weights);
			else
			{
				const float t = (x - below) / (above - below);
				Share(samples, below, 1.0f - t, weights);
				Share(samples, above, t, weights);
			}
		}

		void Weights2D(const std::vector<AnimationBlendSample>& samples, glm::vec2 point, std::vector<float>& weights)
		{
			float total = 0.0f;
			for (size_t i = 0; i < samples.size(); i++)
			{
				const glm::vec2 origin(samples[i].X, samples[i].Y);
				// A sample's influence falls to 0 at every other sample, along the line towards it. Samples at the
				// same position skip each other.
				float weight = 1.0f;
				for (size_t j = 0; j < samples.size() && weight > 0.0f; j++)
				{
					const glm::vec2 edge = glm::vec2(samples[j].X, samples[j].Y) - origin;
					const float lengthSquared = glm::dot(edge, edge);
					if (j == i || lengthSquared <= 0.0f)
						continue;
					weight = std::min(weight, std::clamp(1.0f - glm::dot(point - origin, edge) / lengthSquared, 0.0f, 1.0f));
				}
				// Coincident samples each reach full weight there; splitting it keeps them one position's worth.
				size_t coincident = 0;
				for (const AnimationBlendSample& other : samples)
					coincident += other.X == samples[i].X && other.Y == samples[i].Y;
				weights[i] = weight / static_cast<float>(coincident);
				total += weights[i];
			}
			if (total > 0.0f && std::isfinite(total))
			{
				for (float& weight : weights)
					weight /= total;
				return;
			}
			// The sample nearest the point always keeps a positive weight; this only guards non-finite input that
			// skipped validation (C++ writes), by playing the nearest sample.
			size_t nearest = 0;
			float best = std::numeric_limits<float>::infinity();
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
		}

	}

	void ComputeBlendSpaceWeights(const AnimationBlendSpace& space, glm::vec2 parameter, std::vector<float>& outWeights)
	{
		outWeights.assign(space.Samples.size(), 0.0f);
		if (space.Samples.empty())
			return;
		if (space.Type == AnimationBlendSpaceType::OneD)
			Weights1D(space.Samples, parameter.x, outWeights);
		else
			Weights2D(space.Samples, parameter, outWeights);
	}

}
