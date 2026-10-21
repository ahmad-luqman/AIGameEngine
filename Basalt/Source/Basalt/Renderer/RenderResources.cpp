#include "Basalt/Renderer/RenderResources.h"

#include "Basalt/Core/Log.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace Basalt {

	namespace {

		// sRGB <-> linear conversions through lookup tables: mip generation touches every texel, and pow()
		// per channel makes large textures stall for hundreds of milliseconds.
		const std::array<float, 256>& SRGBToLinearTable()
		{
			static const std::array<float, 256> s_Table = []() {
				std::array<float, 256> table{};
				for (size_t i = 0; i < table.size(); i++)
				{
					const float value = static_cast<float>(i) / 255.0f;
					table[i] = value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
				}
				return table;
			}();
			return s_Table;
		}

		constexpr size_t LinearTableSize = 4096;

		const std::array<uint8_t, LinearTableSize>& LinearToSRGBTable()
		{
			static const std::array<uint8_t, LinearTableSize> s_Table = []() {
				std::array<uint8_t, LinearTableSize> table{};
				for (size_t i = 0; i < table.size(); i++)
				{
					const float value = static_cast<float>(i) / static_cast<float>(LinearTableSize - 1);
					const float encoded = value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
					table[i] = static_cast<uint8_t>(std::clamp(encoded * 255.0f + 0.5f, 0.0f, 255.0f));
				}
				return table;
			}();
			return s_Table;
		}

		// Box-filters an RGBA8 image to half size (clamped at 1). Color channels of sRGB images are
		// averaged in linear space.
		void Downsample(const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height, bool srgb, std::vector<uint8_t>& out, uint32_t& outWidth, uint32_t& outHeight)
		{
			const auto& toLinear = SRGBToLinearTable();
			const auto& toSRGB = LinearToSRGBTable();
			outWidth = std::max(1u, width / 2);
			outHeight = std::max(1u, height / 2);
			out.resize(static_cast<size_t>(outWidth) * outHeight * 4);

			for (uint32_t y = 0; y < outHeight; y++)
			{
				const uint32_t y0 = std::min(y * 2, height - 1);
				const uint32_t y1 = std::min(y * 2 + 1, height - 1);
				for (uint32_t x = 0; x < outWidth; x++)
				{
					const uint32_t x0 = std::min(x * 2, width - 1);
					const uint32_t x1 = std::min(x * 2 + 1, width - 1);
					const size_t texels[4] = { (static_cast<size_t>(y0) * width + x0) * 4, (static_cast<size_t>(y0) * width + x1) * 4, (static_cast<size_t>(y1) * width + x0) * 4, (static_cast<size_t>(y1) * width + x1) * 4 };
					uint8_t* destination = out.data() + (static_cast<size_t>(y) * outWidth + x) * 4;
					for (uint32_t c = 0; c < 4; c++)
					{
						if (srgb && c < 3)
						{
							float sum = 0.0f;
							for (size_t texel : texels)
								sum += toLinear[pixels[texel + c]];
							destination[c] = toSRGB[static_cast<size_t>(std::lround(std::clamp(sum * 0.25f, 0.0f, 1.0f) * static_cast<float>(LinearTableSize - 1)))];
						}
						else
						{
							uint32_t sum = 0;
							for (size_t texel : texels)
								sum += pixels[texel + c];
							destination[c] = static_cast<uint8_t>((sum + 2) / 4);
						}
					}
				}
			}
		}

	}

	RenderResources::RenderResources(nvrhi::IDevice* device)
		: m_Device(device)
	{
	}

	RenderResources::~RenderResources()
	{
		Clear();
	}

	void RenderResources::Initialize(nvrhi::ICommandList* commandList)
	{
		m_White = CreateTexture(*TextureSource::CreateSolid(0xFFFFFFFF, "White"), false, "White", commandList);
		m_Black = CreateTexture(*TextureSource::CreateSolid(0x000000FF, "Black"), false, "Black", commandList);
		m_FlatNormal = CreateTexture(*TextureSource::CreateSolid(0x8080FFFF, "FlatNormal"), false, "FlatNormal", commandList);
		m_EmptyMorph = CreateMorphTexture(1, 1, "EmptyMorphDeltas");
		if (m_EmptyMorph)
		{
			const glm::vec4 zero(0);
			commandList->writeTexture(m_EmptyMorph, 0, 0, &zero, sizeof(zero));
		}
		else
			BS_CORE_ERROR("RenderResources: failed to create the empty morph texture");
	}

	nvrhi::TextureHandle RenderResources::CreateMorphTexture(uint32_t width, uint32_t height, const std::string& debugName)
	{
		nvrhi::TextureDesc desc;
		desc.width = width;
		desc.height = height;
		desc.format = nvrhi::Format::RGBA32_FLOAT;
		desc.debugName = debugName;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		return m_Device->createTexture(desc);
	}

	nvrhi::TextureHandle RenderResources::CreateTexture(const TextureSource& source, bool srgb, const char* debugName, nvrhi::ICommandList* commandList)
	{
		if (source.Format == TextureFormat::BC7)
		{
			if (!source.HasValidBC7Layout())
			{
				BS_CORE_ERROR("Texture '{}': invalid BC7 mip layout", debugName);
				return nullptr;
			}
			nvrhi::Format format = srgb ? nvrhi::Format::BC7_UNORM_SRGB : nvrhi::Format::BC7_UNORM;
			const auto required = nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::ShaderSample;
			const bool supported = (m_Device->queryFormatSupport(format) & required) == required;
			if (!supported)
			{
				if (!m_ReportedBC7Fallback)
					BS_CORE_WARN("BC7 sampling unavailable; textures use decoded RGBA8 mip chains (4x the memory)");
				m_ReportedBC7Fallback = true;
				format = srgb ? nvrhi::Format::SRGBA8_UNORM : nvrhi::Format::RGBA8_UNORM;
			}
			if (source.SRGB != srgb)
				BS_CORE_WARN("Texture '{}': authored {} but used as {}; recompress with the matching srgb setting for correct mip filtering", debugName, source.SRGB ? "sRGB" : "linear", srgb ? "sRGB" : "linear");
			nvrhi::TextureDesc desc;
			desc.width = source.Width;
			desc.height = source.Height;
			desc.mipLevels = static_cast<uint32_t>(source.MipOffsets.size());
			desc.format = format;
			desc.debugName = debugName;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			auto texture = m_Device->createTexture(desc);
			if (!texture)
				return nullptr;
			for (uint32_t mip = 0; mip < desc.mipLevels; mip++)
			{
				if (supported)
				{
					const size_t rowPitch = size_t((std::max(1u, source.Width >> mip) + 3) / 4) * 16; // one row of 4x4 blocks
					commandList->writeTexture(texture, 0, mip, source.Pixels.data() + source.MipOffsets[mip], rowPitch);
					continue;
				}
				std::string error;
				const auto decoded = source.DecodeRGBA8(mip, error);
				if (!decoded)
				{
					BS_CORE_ERROR("BC7 fallback '{}': {}", debugName, error);
					return nullptr;
				}
				commandList->writeTexture(texture, 0, mip, decoded->Pixels.data(), size_t(decoded->Width) * 4);
			}
			return texture;
		}
		// HDR images are stored as half floats: RGBA32F is not filterable on every GPU, and half precision
		// covers the dynamic range of environment maps.
		const bool isFloat = source.Format == TextureFormat::RGBA32F;
		nvrhi::TextureDesc desc;
		desc.width = source.Width;
		desc.height = source.Height;
		desc.mipLevels = isFloat ? 1 : TextureSource::MipCount(source.Width, source.Height);
		desc.format = isFloat ? nvrhi::Format::RGBA16_FLOAT : (srgb ? nvrhi::Format::SRGBA8_UNORM : nvrhi::Format::RGBA8_UNORM);
		desc.debugName = debugName;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		nvrhi::TextureHandle texture = m_Device->createTexture(desc);
		if (!texture)
			return nullptr;

		if (isFloat)
		{
			const auto* floats = reinterpret_cast<const float*>(source.Pixels.data());
			const size_t count = static_cast<size_t>(source.Width) * source.Height * 4;
			std::vector<uint16_t> halves(count);
			for (size_t i = 0; i < count; i++)
				halves[i] = glm::packHalf1x16(std::min(floats[i], 65000.0f));
			commandList->writeTexture(texture, 0, 0, halves.data(), static_cast<size_t>(source.Width) * 8);
		}
		else
		{
			commandList->writeTexture(texture, 0, 0, source.Pixels.data(), static_cast<size_t>(source.Width) * 4);
		}
		if (!isFloat)
		{
			std::vector<uint8_t> level = source.Pixels;
			std::vector<uint8_t> next;
			uint32_t width = source.Width;
			uint32_t height = source.Height;
			for (uint32_t mip = 1; mip < desc.mipLevels; mip++)
			{
				uint32_t nextWidth = 0;
				uint32_t nextHeight = 0;
				Downsample(level, width, height, srgb, next, nextWidth, nextHeight);
				level.swap(next);
				width = nextWidth;
				height = nextHeight;
				commandList->writeTexture(texture, 0, mip, level.data(), static_cast<size_t>(width) * 4);
			}
		}
		return texture;
	}

	const GpuMesh* RenderResources::GetMesh(const Ref<MeshSource>& source, nvrhi::ICommandList* commandList)
	{
		if (!source || source->Vertices.empty() || source->Indices.empty())
			return nullptr;

		auto it = m_Meshes.find(source.get());
		if (it != m_Meshes.end() && !it->second.Source.expired())
			return &it->second.Mesh;

		GpuMesh mesh;
		nvrhi::BufferDesc vertexDesc;
		vertexDesc.byteSize = source->Vertices.size() * sizeof(Vertex);
		vertexDesc.isVertexBuffer = true;
		vertexDesc.debugName = source->Key + " (vertices)";
		vertexDesc.initialState = nvrhi::ResourceStates::VertexBuffer;
		vertexDesc.keepInitialState = true;
		mesh.VertexBuffer = m_Device->createBuffer(vertexDesc);

		nvrhi::BufferDesc indexDesc;
		indexDesc.byteSize = source->Indices.size() * sizeof(uint32_t);
		indexDesc.isIndexBuffer = true;
		indexDesc.debugName = source->Key + " (indices)";
		indexDesc.initialState = nvrhi::ResourceStates::IndexBuffer;
		indexDesc.keepInitialState = true;
		mesh.IndexBuffer = m_Device->createBuffer(indexDesc);

		if (!mesh.VertexBuffer || !mesh.IndexBuffer)
		{
			BS_CORE_ERROR("RenderResources: failed to create GPU buffers for '{}'", source->Key);
			return nullptr;
		}

		commandList->writeBuffer(mesh.VertexBuffer, source->Vertices.data(), vertexDesc.byteSize);
		commandList->writeBuffer(mesh.IndexBuffer, source->Indices.data(), indexDesc.byteSize);

		// Two texels per delta (Morphing.glsl), in rows of MorphTextureWidth.
		size_t morphBytes = 0;
		if (!source->MorphDeltas.empty())
		{
			const size_t texels = source->MorphDeltas.size() * 2;
			const auto width = static_cast<uint32_t>(std::min<size_t>(texels, MorphTextureWidth));
			const auto height = static_cast<uint32_t>((texels + width - 1) / width);
			// The importer keeps MorphDeltas within MaxMorphDeltas; a hand-built source may not.
			if (height > MaxMorphTextureRows)
				BS_CORE_ERROR("RenderResources: '{}' has {} morph deltas, past the limit of {}; it draws without morph targets", source->Key, source->MorphDeltas.size(), MaxMorphDeltas);
			else
			{
				mesh.MorphDeltas = CreateMorphTexture(width, height, source->Key + " (morph targets)");
				if (!mesh.MorphDeltas)
					BS_CORE_ERROR("RenderResources: failed to create the morph target texture for '{}'; it draws without morph targets", source->Key);
			}
			if (mesh.MorphDeltas)
			{
				std::vector<glm::vec4> data(static_cast<size_t>(width) * height, glm::vec4(0));
				for (size_t i = 0; i < source->MorphDeltas.size(); i++)
				{
					data[i * 2] = glm::vec4(source->MorphDeltas[i].Position, 0);
					data[i * 2 + 1] = glm::vec4(source->MorphDeltas[i].Normal, 0);
				}
				commandList->writeTexture(mesh.MorphDeltas, 0, 0, data.data(), static_cast<size_t>(width) * sizeof(glm::vec4));
				morphBytes = data.size() * sizeof(glm::vec4);
			}
		}

		MeshEntry& entry = m_Meshes[source.get()];
		entry.Source = source;
		entry.Mesh = std::move(mesh);
		entry.Memory.Resize(static_cast<size_t>(vertexDesc.byteSize + indexDesc.byteSize) + morphBytes);
		return &entry.Mesh;
	}

	nvrhi::ITexture* RenderResources::GetTexture(const Ref<TextureSource>& source, bool srgb, nvrhi::ICommandList* commandList)
	{
		if (!source || source->Width == 0 || source->Height == 0)
			return nullptr;

		auto& cache = m_Textures[srgb ? 1 : 0];
		auto it = cache.find(source.get());
		if (it != cache.end() && !it->second.Source.expired())
			return it->second.Texture;

		if (it != cache.end() && it->second.Failed && !it->second.Source.expired())
			return nullptr;
		nvrhi::TextureHandle texture = CreateTexture(*source, srgb, source->Key.c_str(), commandList);
		TextureEntry& entry = cache[source.get()];
		if (!texture)
		{
			// Remember the failure (e.g. larger than the GPU's maximum size) instead of retrying every frame.
			BS_CORE_ERROR("RenderResources: failed to create texture '{}' ({}x{})", source->Key, source->Width, source->Height);
			entry.Source = source;
			entry.Failed = true;
			return nullptr;
		}
		entry.Failed = false;
		entry.Source = source;
		entry.Texture = texture;
		return texture;
	}

	void RenderResources::CollectGarbage()
	{
		std::erase_if(m_Meshes, [](const auto& item) { return item.second.Source.expired(); });
		for (auto& cache : m_Textures)
			std::erase_if(cache, [](const auto& item) { return item.second.Source.expired(); });
	}

	void RenderResources::Clear()
	{
		m_Meshes.clear();
		m_Textures[0].clear();
		m_Textures[1].clear();
	}

}
