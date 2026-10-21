#include "Basalt/Renderer/TemporalHistory.h"

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
		const uint32_t sample = frame % TemporalJitterPeriod + 1;
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
								const glm::mat4& view, const glm::mat4& projection, uint64_t cameraIdentity)
	{
		const glm::mat4 oldCamera = glm::inverse(m_View), camera = glm::inverse(view);
		// Negated finite comparisons also reject singular/nonfinite camera history when the camera recovers.
		// Abrupt moves are cuts. Hosts can explicitly reset smaller cuts (including deterministic captures).
		const bool cut = !(glm::length(glm::vec3(camera[3] - oldCamera[3])) <= 10.0f) ||
						 !(glm::dot(glm::normalize(glm::vec3(camera[2])), glm::normalize(glm::vec3(oldCamera[2]))) >= 0.5f);
		if (!enabled || !m_Enabled || revision != m_Revision || component != m_Component || cameraIdentity != m_CameraIdentity || size != m_Size || projection != m_Projection || cut)
			Reset();
		m_Enabled = enabled;
		m_Revision = revision;
		m_Component = component;
		m_CameraIdentity = cameraIdentity;
		m_Size = size;
		m_View = view;
		m_Projection = projection;
	}

	glm::mat4 TemporalHistory::GetJitteredProjection() const
	{
		return m_Enabled ? JitterProjection(m_Projection, TemporalJitter(m_Frame), m_Size) : m_Projection;
	}

	glm::vec2 TemporalHistory::GetJitterDelta() const
	{
		if (!m_Enabled || m_Size.x == 0 || m_Size.y == 0)
			return glm::vec2(0);
		// Undo the change in raster jitter on the stable color grid; texture Y runs opposite clip Y.
		return (TemporalJitter(m_Frame) - TemporalJitter((m_Frame + TemporalJitterPeriod - 1) % TemporalJitterPeriod)) / glm::vec2(m_Size) * glm::vec2(1, -1);
	}

	void TemporalHistory::Commit()
	{
		m_PreviousProjection = GetJitteredProjection();
		m_PreviousViewProjection = m_PreviousProjection * m_View;
		m_Valid = m_Enabled;
		if (m_Enabled)
			m_Frame = (m_Frame + 1) % TemporalJitterPeriod;
	}

	glm::mat4 TemporalHistory::GetSkyReprojection() const
	{
		glm::mat4 rotationView = m_View;
		rotationView[3] = glm::vec4(0, 0, 0, 1);
		glm::mat4 previousRotationVP = m_PreviousViewProjection;
		// P * rotation-only V keeps P's fourth column. Zeroing it loses W for orthographic skies.
		previousRotationVP[3] = m_PreviousProjection[3];
		return previousRotationVP * glm::inverse(GetJitteredProjection() * rotationView);
	}

	void TemporalHistory::Reset()
	{
		m_Valid = false;
		m_Frame = 0;
	}
}
