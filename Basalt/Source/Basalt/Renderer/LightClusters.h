#pragma once

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <cstddef>

namespace Basalt {

	inline constexpr uint32_t MaxSceneLights = BS_MAX_LIGHTS;
	inline constexpr glm::uvec3 LightClusterDimensions{ 16, 9, 24 };
	inline constexpr uint32_t LightClusterCount = LightClusterDimensions.x * LightClusterDimensions.y * LightClusterDimensions.z;

	// Encoded in GpuLightData::Color.w; keep Include/LightData.glsl in sync.
	enum class GpuLightType : uint32_t
	{
		Point = 0,
		Spot = 1,
		Directional = 2
	};

	// Shared std430 light record; the packed buffer starts with MaxSceneLights of these,
	// followed by one count and MaxSceneLights indices per cell (uint32 words).
	struct GpuLightData
	{
		glm::vec4 Position;
		glm::vec4 Color;
		glm::vec4 Direction;
		glm::vec4 Params;
	};
	static_assert(sizeof(GpuLightData) == 64);
	static_assert(offsetof(GpuLightData, Position) == 0 && offsetof(GpuLightData, Color) == 16);
	static_assert(offsetof(GpuLightData, Direction) == 32 && offsetof(GpuLightData, Params) == 48);
	inline constexpr uint32_t LightClusterWordOffset = sizeof(GpuLightData) * MaxSceneLights / sizeof(uint32_t);
	inline constexpr uint32_t LightClusterStride = MaxSceneLights + 1;

	// Headless mirrors of the GPU partition/plane math, used as independent test oracles.
	// GPU depth tuple: near, far, orthographic (0/1), fragment count-diagnostic flag. Perspective requires 0 < near < far;
	// malformed ranges are logged and replaced with a finite default interval.
	// Orthographic near planes may be negative; only perspective uses logarithmic depth.
	glm::vec4 LightClusterDepths(float nearClip, float farClip, bool orthographic);
	float LightClusterBoundary(const glm::vec4& depths, uint32_t slice);
	uint32_t LightClusterSlice(const glm::vec4& depths, float viewDepth);
	// World-space inward planes; a sphere test conservatively bounds point and spot influence.
	std::array<glm::vec4, 6> LightClusterPlanes(const glm::mat4& view, const glm::mat4& projection,
												const glm::vec4& depths, glm::uvec3 cell);
	bool LightClusterIntersects(const std::array<glm::vec4, 6>& planes, glm::vec3 position, float radius, float relativeTolerance = 1e-4f);
	uint32_t LightClusterIndex(glm::uvec3 cell);
	uint32_t LightClusterListOffset(uint32_t index);

	// Builds ordered per-cell light lists on the GPU. Every cell can hold the full scene budget,
	// so there is no truncation or overflow fallback. Light records and lists share one buffer to
	// retain the four-SSBO frame layout on devices at Vulkan's minimum per-stage descriptor limit.
	// Upload the light-record prefix before Build; the compute pass only writes the list suffix.
	// Resources live for the renderer's lifetime.
	class LightClusters
	{
	public:
		bool Initialize(nvrhi::IDevice* device);
		bool Build(nvrhi::ICommandList* commands, const glm::mat4& view, const glm::mat4& projection,
				   const glm::vec4& depths, uint32_t lightCount);
		bool IsValid() const { return m_Set && m_Pipeline; }
		nvrhi::IBuffer* GetBuffer() const { return m_Buffer; }

	private:
		nvrhi::BufferHandle m_Constants, m_Buffer;
		nvrhi::BindingLayoutHandle m_Layout;
		nvrhi::BindingSetHandle m_Set;
		nvrhi::ComputePipelineHandle m_Pipeline;
	};

}
