#include "Basalt/ImGui/ImGuiRenderer.h"

#include "Basalt/Core/Log.h"
#include "Basalt/Renderer/ShaderUtils.h"

#include <imgui.h>

#include <cstring>
#include <vector>

namespace Basalt {

	namespace {

		struct PushConstants
		{
			float Scale[2];
			float Translate[2];
		};

		// Owned by ImTextureData::BackendUserData for textures that ImGui itself manages (font atlas).
		struct ImGuiBackendTexture
		{
			nvrhi::TextureHandle Texture;
		};

		// Binding sets for textures that have not been drawn for this many frames are released.
		constexpr uint64_t BindingSetRetentionFrames = 8;

	}

	bool ImGuiRenderer::Init(nvrhi::IDevice* device)
	{
		m_Device = device;
		m_CommandList = m_Device->createCommandList();

		m_VertexShader = CreateEmbeddedShader(m_Device, "ImGui.vert", nvrhi::ShaderType::Vertex);
		m_PixelShader = CreateEmbeddedShader(m_Device, "ImGui.frag", nvrhi::ShaderType::Pixel);
		if (!m_VertexShader || !m_PixelShader)
			return false;

		const nvrhi::VertexAttributeDesc attributes[] = {
			nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(ImDrawVert, pos)).setElementStride(sizeof(ImDrawVert)),
			nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(ImDrawVert, uv)).setElementStride(sizeof(ImDrawVert)),
			nvrhi::VertexAttributeDesc().setName("COLOR").setFormat(nvrhi::Format::RGBA8_UNORM).setOffset(offsetof(ImDrawVert, col)).setElementStride(sizeof(ImDrawVert)),
		};
		m_InputLayout = m_Device->createInputLayout(attributes, static_cast<uint32_t>(std::size(attributes)), m_VertexShader);

		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = nvrhi::ShaderType::All;
		layoutDesc.bindingOffsets = ZeroBindingOffsets;
		layoutDesc.bindings = {
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(PushConstants)),
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Sampler(1),
		};
		m_BindingLayout = m_Device->createBindingLayout(layoutDesc);

		nvrhi::SamplerDesc samplerDesc;
		samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Wrap).setAllFilters(true);
		m_Sampler = m_Device->createSampler(samplerDesc);

		ImGuiIO& io = ImGui::GetIO();
		io.BackendRendererName = "Basalt_nvrhi";
		io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
		io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

		return m_InputLayout && m_BindingLayout && m_Sampler;
	}

	void ImGuiRenderer::Shutdown()
	{
		if (!m_Device)
			return;

		m_Device->waitForIdle();
		for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
		{
			if (texture->RefCount != 1)
				continue;
			delete static_cast<ImGuiBackendTexture*>(texture->BackendUserData);
			texture->BackendUserData = nullptr;
			texture->SetTexID(ImTextureID_Invalid);
			texture->SetStatus(ImTextureStatus_Destroyed);
		}

		ImGuiIO& io = ImGui::GetIO();
		io.BackendRendererName = nullptr;
		io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);

		m_BindingSets.clear();
		m_Pipeline = nullptr;
		m_VertexBuffer = nullptr;
		m_IndexBuffer = nullptr;
		m_Sampler = nullptr;
		m_BindingLayout = nullptr;
		m_InputLayout = nullptr;
		m_PixelShader = nullptr;
		m_VertexShader = nullptr;
		m_CommandList = nullptr;
		m_Device = nullptr;
	}

	void ImGuiRenderer::UpdateTexture(ImTextureData* texture, nvrhi::ICommandList* commandList)
	{
		if (texture->Status == ImTextureStatus_WantCreate)
		{
			nvrhi::TextureDesc desc;
			desc.width = static_cast<uint32_t>(texture->Width);
			desc.height = static_cast<uint32_t>(texture->Height);
			desc.format = nvrhi::Format::RGBA8_UNORM;
			desc.debugName = "ImGuiTexture";
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;

			auto* backendTexture = new ImGuiBackendTexture();
			backendTexture->Texture = m_Device->createTexture(desc);

			// Font atlases may be Alpha8; the GPU texture is always RGBA8 (white + alpha).
			std::vector<uint32_t> rgba;
			const void* pixels = texture->GetPixels();
			size_t rowPitch = static_cast<size_t>(texture->Width) * 4;
			if (texture->Format == ImTextureFormat_Alpha8)
			{
				rgba.resize(static_cast<size_t>(texture->Width) * texture->Height);
				const auto* alpha = static_cast<const uint8_t*>(pixels);
				for (size_t i = 0; i < rgba.size(); i++)
					rgba[i] = IM_COL32(255, 255, 255, alpha[i]);
				pixels = rgba.data();
			}
			commandList->writeTexture(backendTexture->Texture, 0, 0, pixels, rowPitch);

			texture->BackendUserData = backendTexture;
			texture->SetTexID(reinterpret_cast<ImTextureID>(backendTexture->Texture.Get()));
			texture->SetStatus(ImTextureStatus_OK);
		}
		else if (texture->Status == ImTextureStatus_WantUpdates)
		{
			auto* backendTexture = static_cast<ImGuiBackendTexture*>(texture->BackendUserData);
			if (!backendTexture)
				return;

			// nvrhi writes whole subresources, so re-upload the full image. Updates are rare (new glyphs).
			std::vector<uint32_t> rgba(static_cast<size_t>(texture->Width) * texture->Height);
			if (texture->Format == ImTextureFormat_Alpha8)
			{
				const auto* alpha = static_cast<const uint8_t*>(texture->GetPixels());
				for (size_t i = 0; i < rgba.size(); i++)
					rgba[i] = IM_COL32(255, 255, 255, alpha[i]);
			}
			else
			{
				std::memcpy(rgba.data(), texture->GetPixels(), rgba.size() * sizeof(uint32_t));
			}
			commandList->writeTexture(backendTexture->Texture, 0, 0, rgba.data(), static_cast<size_t>(texture->Width) * 4);
			texture->SetStatus(ImTextureStatus_OK);
		}
		else if (texture->Status == ImTextureStatus_WantDestroy && texture->UnusedFrames > 0)
		{
			auto* backendTexture = static_cast<ImGuiBackendTexture*>(texture->BackendUserData);
			if (backendTexture)
			{
				m_BindingSets.erase(backendTexture->Texture.Get());
				delete backendTexture; // nvrhi defers the GPU release until in-flight work completes.
			}
			texture->BackendUserData = nullptr;
			texture->SetTexID(ImTextureID_Invalid);
			texture->SetStatus(ImTextureStatus_Destroyed);
		}
	}

	bool ImGuiRenderer::EnsureBuffers(const ImDrawData* drawData)
	{
		const uint64_t vertexBytes = static_cast<uint64_t>(drawData->TotalVtxCount) * sizeof(ImDrawVert);
		const uint64_t indexBytes = static_cast<uint64_t>(drawData->TotalIdxCount) * sizeof(ImDrawIdx);

		if (!m_VertexBuffer || m_VertexBuffer->getDesc().byteSize < vertexBytes)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = std::max<uint64_t>(vertexBytes + 5000 * sizeof(ImDrawVert), uint64_t{ 64 } * 1024);
			desc.debugName = "ImGuiVertexBuffer";
			desc.isVertexBuffer = true;
			desc.initialState = nvrhi::ResourceStates::VertexBuffer;
			desc.keepInitialState = true;
			m_VertexBuffer = m_Device->createBuffer(desc);
		}

		if (!m_IndexBuffer || m_IndexBuffer->getDesc().byteSize < indexBytes)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = std::max<uint64_t>(indexBytes + 10000 * sizeof(ImDrawIdx), uint64_t{ 64 } * 1024);
			desc.debugName = "ImGuiIndexBuffer";
			desc.isIndexBuffer = true;
			desc.initialState = nvrhi::ResourceStates::IndexBuffer;
			desc.keepInitialState = true;
			m_IndexBuffer = m_Device->createBuffer(desc);
		}

		return m_VertexBuffer && m_IndexBuffer;
	}

	bool ImGuiRenderer::EnsurePipeline(nvrhi::IFramebuffer* framebuffer)
	{
		const nvrhi::FramebufferInfo& info = framebuffer->getFramebufferInfo();
		if (m_Pipeline && m_PipelineFramebufferInfo == info)
			return true;

		nvrhi::BlendState blendState;
		blendState.targets[0]
			.setBlendEnable(true)
			.setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
			.setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
			.setSrcBlendAlpha(nvrhi::BlendFactor::One)
			.setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);

		nvrhi::RasterState rasterState;
		rasterState.setCullNone().setScissorEnable(true).setDepthClipEnable(true);

		nvrhi::DepthStencilState depthState;
		depthState.setDepthTestEnable(false).setDepthWriteEnable(false).setStencilEnable(false);

		nvrhi::GraphicsPipelineDesc desc;
		desc.primType = nvrhi::PrimitiveType::TriangleList;
		desc.inputLayout = m_InputLayout;
		desc.VS = m_VertexShader;
		desc.PS = m_PixelShader;
		desc.renderState.blendState = blendState;
		desc.renderState.rasterState = rasterState;
		desc.renderState.depthStencilState = depthState;
		desc.bindingLayouts = { m_BindingLayout };

		m_Pipeline = m_Device->createGraphicsPipeline(desc, info);
		m_PipelineFramebufferInfo = info;
		return m_Pipeline != nullptr;
	}

	nvrhi::IBindingSet* ImGuiRenderer::GetBindingSet(nvrhi::ITexture* texture)
	{
		auto it = m_BindingSets.find(texture);
		if (it != m_BindingSets.end())
		{
			it->second.LastUsedFrame = m_FrameIndex;
			return it->second.BindingSet;
		}

		nvrhi::BindingSetDesc desc;
		desc.bindings = {
			nvrhi::BindingSetItem::PushConstants(0, sizeof(PushConstants)),
			nvrhi::BindingSetItem::Texture_SRV(0, texture),
			nvrhi::BindingSetItem::Sampler(1, m_Sampler),
		};
		nvrhi::BindingSetHandle bindingSet = m_Device->createBindingSet(desc, m_BindingLayout);
		m_BindingSets[texture] = { bindingSet, m_FrameIndex };
		return bindingSet;
	}

	void ImGuiRenderer::Render(ImDrawData* drawData, nvrhi::IFramebuffer* framebuffer)
	{
		m_FrameIndex++;

		m_CommandList->open();
		m_CommandList->beginMarker("ImGui");

		if (drawData->Textures)
		{
			for (ImTextureData* texture : *drawData->Textures)
			{
				if (texture->Status != ImTextureStatus_OK)
					UpdateTexture(texture, m_CommandList);
			}
		}

		const float framebufferWidth = drawData->DisplaySize.x * drawData->FramebufferScale.x;
		const float framebufferHeight = drawData->DisplaySize.y * drawData->FramebufferScale.y;
		const bool hasGeometry = drawData->TotalVtxCount > 0 && framebufferWidth > 0.0f && framebufferHeight > 0.0f;

		if (hasGeometry && EnsureBuffers(drawData) && EnsurePipeline(framebuffer))
		{
			std::vector<ImDrawVert> vertices;
			std::vector<ImDrawIdx> indices;
			// nvrhi rounds small buffer writes up to 4 bytes and reads that many from the source pointer,
			// so leave room for padding (16-bit indices with an odd count).
			vertices.reserve(static_cast<size_t>(drawData->TotalVtxCount));
			indices.reserve(static_cast<size_t>(drawData->TotalIdxCount) + 2);
			for (const ImDrawList* drawList : drawData->CmdLists)
			{
				vertices.insert(vertices.end(), drawList->VtxBuffer.begin(), drawList->VtxBuffer.end());
				indices.insert(indices.end(), drawList->IdxBuffer.begin(), drawList->IdxBuffer.end());
			}
			m_CommandList->writeBuffer(m_VertexBuffer, vertices.data(), vertices.size() * sizeof(ImDrawVert));
			const size_t indexBytes = indices.size() * sizeof(ImDrawIdx);
			while ((indices.size() * sizeof(ImDrawIdx)) % 4 != 0)
				indices.push_back(0);
			m_CommandList->writeBuffer(m_IndexBuffer, indices.data(), indexBytes);

			PushConstants pushConstants;
			pushConstants.Scale[0] = 2.0f / drawData->DisplaySize.x;
			pushConstants.Scale[1] = 2.0f / drawData->DisplaySize.y;
			pushConstants.Translate[0] = -1.0f - drawData->DisplayPos.x * pushConstants.Scale[0];
			pushConstants.Translate[1] = -1.0f - drawData->DisplayPos.y * pushConstants.Scale[1];

			const ImVec2 clipOffset = drawData->DisplayPos;
			const ImVec2 clipScale = drawData->FramebufferScale;

			uint32_t globalVertexOffset = 0;
			uint32_t globalIndexOffset = 0;
			for (const ImDrawList* drawList : drawData->CmdLists)
			{
				for (const ImDrawCmd& command : drawList->CmdBuffer)
				{
					if (command.UserCallback)
					{
						// ImDrawCallback_ResetRenderState needs no work: state is set for every command.
						if (command.UserCallback != ImDrawCallback_ResetRenderState)
							command.UserCallback(drawList, &command);
						continue;
					}

					const ImVec2 clipMin((command.ClipRect.x - clipOffset.x) * clipScale.x, (command.ClipRect.y - clipOffset.y) * clipScale.y);
					const ImVec2 clipMax((command.ClipRect.z - clipOffset.x) * clipScale.x, (command.ClipRect.w - clipOffset.y) * clipScale.y);
					const int scissorMinX = std::max(0, static_cast<int>(clipMin.x));
					const int scissorMinY = std::max(0, static_cast<int>(clipMin.y));
					const int scissorMaxX = std::min(static_cast<int>(framebufferWidth), static_cast<int>(clipMax.x));
					const int scissorMaxY = std::min(static_cast<int>(framebufferHeight), static_cast<int>(clipMax.y));
					if (scissorMaxX <= scissorMinX || scissorMaxY <= scissorMinY)
						continue;

					auto* texture = reinterpret_cast<nvrhi::ITexture*>(command.GetTexID());
					if (!texture)
						continue;

					nvrhi::GraphicsState state;
					state.pipeline = m_Pipeline;
					state.framebuffer = framebuffer;
					state.bindings = { GetBindingSet(texture) };
					state.vertexBuffers = { nvrhi::VertexBufferBinding().setBuffer(m_VertexBuffer).setSlot(0).setOffset(0) };
					state.indexBuffer = nvrhi::IndexBufferBinding()
											.setBuffer(m_IndexBuffer)
											.setFormat(sizeof(ImDrawIdx) == 2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT)
											.setOffset(0);
					state.viewport.addViewport(nvrhi::Viewport(framebufferWidth, framebufferHeight));
					state.viewport.addScissorRect(nvrhi::Rect(scissorMinX, scissorMaxX, scissorMinY, scissorMaxY));

					m_CommandList->setGraphicsState(state);
					m_CommandList->setPushConstants(&pushConstants, sizeof(pushConstants));

					nvrhi::DrawArguments args;
					args.vertexCount = command.ElemCount;
					args.startIndexLocation = globalIndexOffset + command.IdxOffset;
					args.startVertexLocation = globalVertexOffset + command.VtxOffset;
					m_CommandList->drawIndexed(args);
				}
				globalVertexOffset += static_cast<uint32_t>(drawList->VtxBuffer.Size);
				globalIndexOffset += static_cast<uint32_t>(drawList->IdxBuffer.Size);
			}
		}

		m_CommandList->endMarker();
		m_CommandList->close();
		m_Device->executeCommandList(m_CommandList);

		// Drop binding sets (and the texture references they hold) for textures no longer shown.
		for (auto it = m_BindingSets.begin(); it != m_BindingSets.end();)
		{
			if (m_FrameIndex - it->second.LastUsedFrame > BindingSetRetentionFrames)
				it = m_BindingSets.erase(it);
			else
				++it;
		}
	}

}
