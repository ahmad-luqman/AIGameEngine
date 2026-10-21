#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace Basalt {
	// Deterministic pixel-space Halton jitter, repeated every eight successfully submitted frames.
	glm::vec2 TemporalJitter(uint32_t frame);
	// Offsets clip XY by clip W; works for both perspective and orthographic cameras (+Y up).
	glm::mat4 JitterProjection(const glm::mat4& projection, glm::vec2 pixels, glm::uvec2 size);

	// Per-view camera history. Begin validates continuity; only Commit advances a successful frame.
	class TemporalHistory
	{
	public:
		void Begin(bool enabled, uint64_t revision, uint64_t component, glm::uvec2 size,
				   const glm::mat4& view, const glm::mat4& projection);
		void Commit(const glm::mat4& viewProjection);
		void Reset();
		bool IsValid() const { return m_Valid; }
		uint32_t GetFrame() const { return m_Frame; }
		const glm::mat4& GetPreviousViewProjection() const { return m_PreviousViewProjection; }

	private:
		bool m_Enabled = false;
		bool m_Valid = false;
		uint32_t m_Frame = 0;
		uint64_t m_Revision = 0, m_Component = 0;
		glm::uvec2 m_Size{ 0 };
		glm::mat4 m_View{ 1 }, m_Projection{ 1 }, m_PreviousViewProjection{ 1 };
	};
}
