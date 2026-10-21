#include "TemporalHistory.h"

#include <cmath>

namespace Basalt {
	glm::vec2 TemporalJitter(uint32_t frame)
	{
		auto halton = [](uint32_t index, uint32_t base) {
			float value = 0, fraction = 1;
			while (index > 0)
			{
				fraction /= static_cast<float>(base);
				value += fraction * static_cast<float>(index % base);
				index /= base;
			}
			return value;
		};
		const uint32_t sample = frame % 8 + 1;
		return { halton(sample, 2) - 0.5f, halton(sample, 3) - 0.5f };
	}

	glm::mat4 JitterProjection(const glm::mat4& projection, glm::vec2 pixels, glm::uvec2 size)
	{
		if (size.x == 0 || size.y == 0)
			return projection;
		const glm::vec2 offset = 2.0f * pixels / glm::vec2(size);
		glm::mat4 result = projection;
		for (int column = 0; column < 4; column++)
		{
			result[column][0] += offset.x * projection[column][3];
			result[column][1] += offset.y * projection[column][3];
		}
		return result;
	}

	void TemporalHistory::Begin(bool enabled, uint64_t revision, uint64_t component, glm::uvec2 size,
								const glm::mat4& view, const glm::mat4& projection)
	{
		const glm::mat4 oldCamera = glm::inverse(m_View), camera = glm::inverse(view);
		// Abrupt moves are cuts. Hosts can explicitly reset smaller cuts (including deterministic captures).
		const bool cut = glm::length(glm::vec3(camera[3] - oldCamera[3])) > 10.0f ||
						 glm::dot(glm::normalize(glm::vec3(camera[2])), glm::normalize(glm::vec3(oldCamera[2]))) < 0.5f;
		if (!enabled || !m_Enabled || revision != m_Revision || component != m_Component || size != m_Size || projection != m_Projection || cut)
			Reset();
		m_Enabled = enabled;
		m_Revision = revision;
		m_Component = component;
		m_Size = size;
		m_View = view;
		m_Projection = projection;
	}

	void TemporalHistory::Commit(const glm::mat4& viewProjection)
	{
		m_PreviousViewProjection = viewProjection;
		m_Valid = m_Enabled;
		if (m_Enabled)
			m_Frame = (m_Frame + 1) % 8;
	}

	void TemporalHistory::Reset()
	{
		m_Valid = false;
		m_Frame = 0;
	}
}
