#pragma once

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>

namespace Basalt {

	inline constexpr uint32_t MaxClusterLights = BS_MAX_LIGHTS;
	inline constexpr glm::uvec3 LightClusterDimensions{ 16, 9, 24 };
	inline constexpr uint32_t LightClusterCount = LightClusterDimensions.x * LightClusterDimensions.y * LightClusterDimensions.z;

	// Shared std430 light record; the packed buffer starts with MaxClusterLights of these,
	// followed by one count and MaxClusterLights indices per cell (uint32 words).
	struct GpuLightData
	{
		glm::vec4 Position;
		glm::vec4 Color;
		glm::vec4 Direction;
		glm::vec4 Params;
	};
	static_assert(sizeof(GpuLightData) == 64);
	inline constexpr uint32_t LightClusterWordOffset = sizeof(GpuLightData) * MaxClusterLights / sizeof(uint32_t);
	inline constexpr uint32_t LightClusterStride = MaxClusterLights + 1;

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
	// so there is no truncation or overflow fallback. Light records and lists share one buffer to
	// retain the four-SSBO frame layout on devices at Vulkan's minimum per-stage descriptor limit.
	// Upload the light-record prefix before Build; the compute pass only writes the list suffix.
	// Resources live for the renderer's lifetime.
	class LightClusters
	{
	public:
		bool Initialize(nvrhi::IDevice* device);
		void Build(nvrhi::ICommandList* commands, const glm::mat4& view, const glm::mat4& projection,
				   const glm::vec4& depths, uint32_t lightCount);
		nvrhi::IBuffer* GetBuffer() const { return m_Buffer; }

	private:
		nvrhi::BufferHandle m_Constants, m_Buffer;
		nvrhi::BindingLayoutHandle m_Layout;
		nvrhi::BindingSetHandle m_Set;
		nvrhi::ComputePipelineHandle m_Pipeline;
	};

}
