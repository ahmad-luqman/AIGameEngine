#include "Basalt/Renderer/LightClusters.h"

#include "Basalt/Core/Log.h"
#include "Basalt/Renderer/ShaderUtils.h"

#include <algorithm>
#include <cmath>

namespace Basalt {

	namespace {
		struct ClusterConstants
		{
			glm::mat4 ViewProjection, View;
			glm::vec4 Depths;
			glm::uvec4 GridAndLightCount;
		};
		static_assert(sizeof(ClusterConstants) == 160);
	}

	glm::vec4 LightClusterDepths(float nearClip, float farClip, bool orthographic)
	{
		if (!std::isfinite(nearClip) || !std::isfinite(farClip) || nearClip >= farClip || (!orthographic && nearClip <= 0.0f) || !std::isfinite(farClip - nearClip) || (!orthographic && !std::isfinite(farClip / nearClip)))
		{
			BS_CORE_WARN("SceneRenderer: invalid cluster depth range [{}, {}]; using [0.1, 1000]", nearClip, farClip);
			nearClip = 0.1f;
			farClip = 1000.0f;
		}
		return { nearClip, farClip, orthographic ? 1.0f : 0.0f, 0.0f };
	}

	float LightClusterBoundary(const glm::vec4& depths, uint32_t slice)
	{
		const float t = static_cast<float>(slice) / static_cast<float>(LightClusterDimensions.z);
		return depths.z > 0.5f ? glm::mix(depths.x, depths.y, t) : depths.x * std::pow(depths.y / depths.x, t);
	}

	uint32_t LightClusterSlice(const glm::vec4& depths, float viewDepth)
	{
		const float t = depths.z > 0.5f ? (viewDepth - depths.x) / (depths.y - depths.x) : std::log(std::max(viewDepth, depths.x) / depths.x) / std::log(depths.y / depths.x);
		return static_cast<uint32_t>(std::clamp(t * static_cast<float>(LightClusterDimensions.z), 0.0f, static_cast<float>(LightClusterDimensions.z - 1)));
	}

	std::array<glm::vec4, 6> LightClusterPlanes(const glm::mat4& view, const glm::mat4& projection,
												const glm::vec4& depths, glm::uvec3 cell)
	{
		const glm::mat4 rows = glm::transpose(projection * view);
		const glm::vec4 depthRow = -glm::transpose(view)[2];
		const float left = 2.0f * static_cast<float>(cell.x) / static_cast<float>(LightClusterDimensions.x) - 1.0f;
		const float right = 2.0f * static_cast<float>(cell.x + 1) / static_cast<float>(LightClusterDimensions.x) - 1.0f;
		const float top = 1.0f - 2.0f * static_cast<float>(cell.y) / static_cast<float>(LightClusterDimensions.y);
		const float bottom = 1.0f - 2.0f * static_cast<float>(cell.y + 1) / static_cast<float>(LightClusterDimensions.y);
		std::array<glm::vec4, 6> planes{ rows[0] - left * rows[3], right * rows[3] - rows[0],
										 rows[1] - bottom * rows[3], top * rows[3] - rows[1],
										 depthRow - glm::vec4(0, 0, 0, LightClusterBoundary(depths, cell.z)),
										 glm::vec4(0, 0, 0, LightClusterBoundary(depths, cell.z + 1)) - depthRow };
		for (auto& plane : planes)
			plane /= glm::length(glm::vec3(plane));
		// Lookup clamps out-of-range depths into the edge cells, so those cells must be unbounded.
		if (cell.z == 0)
			planes[4] = glm::vec4(0);
		if (cell.z + 1 == LightClusterDimensions.z)
			planes[5] = glm::vec4(0);
		return planes;
	}

	bool LightClusterIntersects(const std::array<glm::vec4, 6>& planes, glm::vec3 position, float radius, float relativeTolerance)
	{
		// Plane tests and fragment tile/slice lookup can round a boundary in opposite directions.
		const float tolerance = relativeTolerance * std::max(1.0f, glm::length(position) + std::abs(radius));
		for (const auto& plane : planes)
			if (glm::dot(plane, glm::vec4(position, 1)) < -radius - tolerance)
				return false;
		return true;
	}

	uint32_t LightClusterIndex(glm::uvec3 cell)
	{
		return (cell.z * LightClusterDimensions.y + cell.y) * LightClusterDimensions.x + cell.x;
	}

	uint32_t LightClusterListOffset(uint32_t index)
	{
		return index * LightClusterStride;
	}

	bool LightClusters::Initialize(nvrhi::IDevice* device)
	{
		// A failed retry must not leave old bindings looking usable with partially replaced resources.
		m_Set = nullptr;
		m_Pipeline = nullptr;
		auto fail = [](const char* stage) {
			BS_CORE_ERROR("SceneRenderer: failed to create light clusters ({})", stage);
			return false;
		};
		nvrhi::BufferDesc desc;
		desc.byteSize = sizeof(ClusterConstants);
		desc.isConstantBuffer = true;
		desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
		desc.keepInitialState = true;
		desc.debugName = "ClusterConstants";
		m_Constants = device->createBuffer(desc);
		m_ConstantsMemory.Resize(m_Constants ? static_cast<size_t>(desc.byteSize) : 0);
		if (!m_Constants)
			return fail("constants");
		desc = {};
		desc.structStride = sizeof(uint32_t);
		desc.canHaveUAVs = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.byteSize = sizeof(uint32_t) * (LightClusterWordOffset + LightClusterCount * LightClusterStride);
		desc.debugName = "ClusteredLightBuffer";
		m_Buffer = device->createBuffer(desc);
		m_BufferMemory.Resize(m_Buffer ? static_cast<size_t>(desc.byteSize) : 0);
		if (!m_Buffer)
			return fail("light records/lists");
		auto layout = MakeLayout(0, nvrhi::ShaderType::Compute);
		layout.bindings = { nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3) };
		m_Layout = device->createBindingLayout(layout);
		const auto shader = CreateEmbeddedShader(device, "LightClusters.comp", nvrhi::ShaderType::Compute);
		if (!m_Layout)
			return fail("layout");
		if (!shader)
			return fail("shader");
		m_Set = device->createBindingSet(nvrhi::BindingSetDesc()
											 .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_Constants))
											 .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(3, m_Buffer)),
										 m_Layout);
		if (!m_Set)
			return fail("bindings");
		nvrhi::ComputePipelineDesc pipeline;
		pipeline.CS = shader;
		pipeline.bindingLayouts = { m_Layout };
		m_Pipeline = device->createComputePipeline(pipeline);
		if (!m_Pipeline)
			return fail("pipeline");
		return true;
	}

	bool LightClusters::Build(nvrhi::ICommandList* commands, const glm::mat4& view, const glm::mat4& projection,
							  const glm::vec4& depths, uint32_t lightCount)
	{
		if (!IsValid())
		{
			BS_CORE_ERROR("SceneRenderer: cannot build light clusters before successful initialization");
			return false;
		}
		if (lightCount > MaxSceneLights)
		{
			BS_CORE_WARN("SceneRenderer: cluster light count {} exceeds {}; clamping", lightCount, MaxSceneLights);
			lightCount = MaxSceneLights;
		}
		const ClusterConstants constants{ projection * view, view, depths, glm::uvec4(LightClusterDimensions, lightCount) };
		commands->writeBuffer(m_Constants, &constants, sizeof(constants));
		commands->setComputeState(nvrhi::ComputeState().setPipeline(m_Pipeline).addBindingSet(m_Set));
		commands->dispatch(LightClusterDimensions.x, LightClusterDimensions.y, LightClusterDimensions.z);
		return true;
	}

}
