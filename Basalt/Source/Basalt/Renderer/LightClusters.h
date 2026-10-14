#pragma once

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <array>

namespace Basalt {

	inline constexpr uint32_t MaxClusterLights = BS_MAX_LIGHTS;
	inline constexpr glm::uvec3 LightClusterDimensions{ 16, 9, 24 };
	inline constexpr uint32_t LightClusterCount = LightClusterDimensions.x * LightClusterDimensions.y * LightClusterDimensions.z;

	// Camera-space depth partition shared with the cluster builder and fragment lookup.
	// Orthographic near planes may be negative; only perspective uses logarithmic depth.
	glm::vec4 LightClusterDepths(float nearClip, float farClip, bool orthographic);
	float LightClusterBoundary(const glm::vec4& depths, uint32_t slice);
	uint32_t LightClusterSlice(const glm::vec4& depths, float viewDepth);
	// World-space inward planes; a sphere test conservatively bounds point and spot influence.
	std::array<glm::vec4, 6> LightClusterPlanes(const glm::mat4& view, const glm::mat4& projection,
												const glm::vec4& depths, glm::uvec3 cell);
	bool LightClusterIntersects(const std::array<glm::vec4, 6>& planes, glm::vec3 position, float radius);

	// Builds ordered per-cell light lists on the GPU. Every cell can hold the full scene budget,
	// so there is no truncation or overflow fallback. Buffers live for the renderer's lifetime.
	class LightClusters
	{
	public:
		bool Initialize(nvrhi::IDevice* device, nvrhi::IBuffer* lights);
		void Build(nvrhi::ICommandList* commands, const glm::mat4& view, const glm::mat4& projection,
				   const glm::vec4& depths, uint32_t lightCount);
		nvrhi::IBuffer* GetCounts() const { return m_Counts; }
		nvrhi::IBuffer* GetIndices() const { return m_Indices; }

	private:
		nvrhi::BufferHandle m_Constants, m_Counts, m_Indices;
		nvrhi::BindingLayoutHandle m_Layout;
		nvrhi::BindingSetHandle m_Set;
		nvrhi::ComputePipelineHandle m_Pipeline;
	};

}
