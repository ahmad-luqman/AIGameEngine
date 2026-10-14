#include "Basalt/Renderer/LightClusters.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Renderer/ShaderUtils.h"

#include <algorithm>
#include <cmath>

namespace Basalt {

	glm::vec4 LightClusterDepths(float nearClip, float farClip, bool orthographic)
	{
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
		return planes;
	}

	bool LightClusterIntersects(const std::array<glm::vec4, 6>& planes, glm::vec3 position, float radius)
	{
		// Expand rather than risk losing a light on a plane due to CPU/GPU rounding.
		const float tolerance = 1e-4f * std::max(1.0f, glm::length(position) + std::abs(radius));
		for (const auto& plane : planes)
			if (glm::dot(plane, glm::vec4(position, 1)) < -radius - tolerance)
				return false;
		return true;
	}

	bool LightClusters::Initialize(nvrhi::IDevice* device, nvrhi::IBuffer* lights)
	{
		auto fail = [](const char* stage) {
			BS_CORE_ERROR("SceneRenderer: failed to create light clusters ({})", stage);
			return false;
		};
		nvrhi::BufferDesc desc;
		desc.byteSize = 160;
		desc.isConstantBuffer = true;
		desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
		desc.keepInitialState = true;
		desc.debugName = "ClusterConstants";
		m_Constants = device->createBuffer(desc);
		if (!m_Constants)
			return fail("constants");
		desc = {};
		desc.structStride = sizeof(uint32_t);
		desc.canHaveUAVs = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.byteSize = sizeof(uint32_t) * LightClusterCount;
		desc.debugName = "ClusterCounts";
		m_Counts = device->createBuffer(desc);
		desc.byteSize *= MaxClusterLights;
		desc.debugName = "ClusterIndices";
		m_Indices = device->createBuffer(desc);
		if (!m_Counts || !m_Indices)
			return fail("lists");
		auto layout = MakeLayout(0, nvrhi::ShaderType::Compute);
		layout.bindings = { nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3),
							nvrhi::BindingLayoutItem::StructuredBuffer_UAV(15), nvrhi::BindingLayoutItem::StructuredBuffer_UAV(16) };
		m_Layout = device->createBindingLayout(layout);
		const auto shader = CreateEmbeddedShader(device, "LightClusters.comp", nvrhi::ShaderType::Compute);
		if (!m_Layout || !shader)
			return fail("layout/shader");
		m_Set = device->createBindingSet(nvrhi::BindingSetDesc()
											 .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_Constants))
											 .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, lights))
											 .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(15, m_Counts))
											 .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(16, m_Indices)),
										 m_Layout);
		nvrhi::ComputePipelineDesc pipeline;
		pipeline.CS = shader;
		pipeline.bindingLayouts = { m_Layout };
		m_Pipeline = device->createComputePipeline(pipeline);
		return (m_Set && m_Pipeline) || fail("bindings/pipeline");
	}

	void LightClusters::Build(nvrhi::ICommandList* commands, const glm::mat4& view, const glm::mat4& projection,
							  const glm::vec4& depths, uint32_t lightCount)
	{
		BS_CORE_ASSERT(lightCount <= MaxClusterLights, "cluster lists cover the scene light budget");
		struct Constants
		{
			glm::mat4 ViewProjection, View;
			glm::vec4 Depths;
			glm::uvec4 Grid;
		};
		static_assert(sizeof(Constants) == 160);
		const Constants constants{ projection * view, view, depths, glm::uvec4(LightClusterDimensions, lightCount) };
		commands->writeBuffer(m_Constants, &constants, sizeof(constants));
		commands->setComputeState(nvrhi::ComputeState().setPipeline(m_Pipeline).addBindingSet(m_Set));
		commands->dispatch(LightClusterDimensions.x, LightClusterDimensions.y, LightClusterDimensions.z);
	}

}
