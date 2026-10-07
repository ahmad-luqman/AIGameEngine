#pragma once

#include "Basalt/Asset/MeshSource.h"
#include "Basalt/Asset/TextureSource.h"
#include "Basalt/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <memory>
#include <unordered_map>

namespace Basalt {

	struct GpuMesh
	{
		nvrhi::BufferHandle VertexBuffer;
		nvrhi::BufferHandle IndexBuffer;
	};

	// GPU copies of CPU assets, created on first use and keyed by the source object. When an asset is
	// reloaded (new source object), the stale GPU copy is released on the next CollectGarbage().
	class RenderResources
	{
	public:
		explicit RenderResources(nvrhi::IDevice* device);
		~RenderResources();

		RenderResources(const RenderResources&) = delete;
		RenderResources& operator=(const RenderResources&) = delete;

		// Uploads happen on the given command list, which must be open.
		const GpuMesh* GetMesh(const Ref<MeshSource>& source, nvrhi::ICommandList* commandList);
		// sRGB textures (albedo, emissive) are sampled with hardware sRGB decoding. Full mip chain.
		nvrhi::ITexture* GetTexture(const Ref<TextureSource>& source, bool srgb, nvrhi::ICommandList* commandList);

		nvrhi::ITexture* GetWhiteTexture() const { return m_White; }
		nvrhi::ITexture* GetBlackTexture() const { return m_Black; }
		nvrhi::ITexture* GetFlatNormalTexture() const { return m_FlatNormal; }

		// Releases GPU copies whose source assets no longer exist.
		void CollectGarbage();
		void Clear();

		// Creates the default textures; call once with an open command list before rendering.
		void Initialize(nvrhi::ICommandList* commandList);

	private:
		nvrhi::TextureHandle CreateTexture(const TextureSource& source, bool srgb, const char* debugName, nvrhi::ICommandList* commandList);

	private:
		nvrhi::IDevice* m_Device = nullptr;

		struct MeshEntry
		{
			std::weak_ptr<MeshSource> Source;
			GpuMesh Mesh;
		};
		struct TextureEntry
		{
			std::weak_ptr<TextureSource> Source;
			nvrhi::TextureHandle Texture;
			bool Failed = false;
		};
		std::unordered_map<const MeshSource*, MeshEntry> m_Meshes;
		std::unordered_map<const TextureSource*, TextureEntry> m_Textures[2];

		nvrhi::TextureHandle m_White;
		nvrhi::TextureHandle m_Black;
		nvrhi::TextureHandle m_FlatNormal;
	};

}
