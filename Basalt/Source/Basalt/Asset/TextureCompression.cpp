#include "Basalt/Asset/TextureCompression.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Hash.h"
#include "Basalt/Core/Log.h"

#include <basisu_transcoder.h>
#include <bc7enc.h>
#include <bc7decomp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

namespace Basalt {

	namespace {
		constexpr std::array<uint8_t, 12> Identifier = { 0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 13, 10, 26, 10 };
		constexpr size_t MaxPayload = size_t{ 256 } * 1024 * 1024;
		uint32_t Read32(const uint8_t* p)
		{
			return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
		}
		uint64_t Read64(const uint8_t* p)
		{
			return Read32(p) | (uint64_t(Read32(p + 4)) << 32);
		}
		void Put(std::vector<uint8_t>& out, size_t offset, uint64_t value, size_t count = 4)
		{
			for (size_t i = 0; i < count; i++)
				out[offset + i] = static_cast<uint8_t>(value >> (8 * i));
		}
		size_t LevelSize(uint32_t w, uint32_t h)
		{
			return size_t((w + 3) / 4) * ((h + 3) / 4) * 16;
		}
		void Initialize()
		{
			static std::once_flag s_Once;
			std::call_once(s_Once, [] { basist::basisu_transcoder_init(); bc7enc_compress_block_init(); });
		}
	}

	bool TextureCompression::IsKTX2(const uint8_t* data, size_t size)
	{
		return data && size >= Identifier.size() && std::equal(Identifier.begin(), Identifier.end(), data);
	}

	Ref<TextureSource> TextureCompression::LoadKTX2(const uint8_t* data, size_t size, std::string& outError)
	{
		outError = "invalid or unsupported KTX2 (expected a 2D BC7 or Basis LDR texture)";
		if (!IsKTX2(data, size) || size < 104 || size > MaxPayload)
			return nullptr;
		const uint32_t format = Read32(data + 12), width = Read32(data + 20), height = Read32(data + 24), levels = Read32(data + 40);
		if (!width || !height || width > 16384 || height > 16384 || Read32(data + 28) || Read32(data + 32) || Read32(data + 36) != 1 || !levels || levels > 15 || 80 + size_t(levels) * 24 > size)
			return nullptr;
		uint32_t maxLevels = 1;
		for (uint32_t n = std::max(width, height); n > 1; n /= 2)
			maxLevels++;
		if (levels > maxLevels)
			return nullptr;
		const uint32_t dfd = Read32(data + 48), dfdSize = Read32(data + 52);
		if (dfd < 80 + levels * 24 || dfdSize < 28 || dfd > size || dfdSize > size - dfd || Read32(data + dfd) != dfdSize)
			return nullptr;
		// Validate budgets and every referenced range before the transcoder can allocate/decompress.
		uint64_t totalBytes = 0;
		for (uint32_t mip = 0; mip < levels; mip++)
		{
			const auto* index = data + 80 + size_t(mip) * 24;
			const uint64_t start = Read64(index), length = Read64(index + 8), expanded = Read64(index + 16);
			if (start > size || length > size - start || expanded > MaxPayload)
				return nullptr;
			totalBytes += LevelSize(std::max(1u, width >> mip), std::max(1u, height >> mip));
			if (totalBytes > MaxPayload)
				return nullptr;
		}
		const uint32_t kvd = Read32(data + 56), kvdSize = Read32(data + 60);
		if (kvd > size || kvdSize > size - kvd)
			return nullptr;
		for (size_t pos = kvd; pos < size_t(kvd) + kvdSize;)
		{
			const size_t end = size_t(kvd) + kvdSize;
			if (end - pos < 4)
				return nullptr;
			const uint32_t length = Read32(data + pos);
			pos += 4;
			if (!length || length > end - pos)
				return nullptr;
			const auto* zero = static_cast<const uint8_t*>(std::memchr(data + pos, 0, length));
			if (!zero)
				return nullptr;
			const std::string_view key(reinterpret_cast<const char*>(data + pos), static_cast<size_t>(zero - (data + pos)));
			const std::string_view value(reinterpret_cast<const char*>(zero + 1), length - static_cast<size_t>(zero + 1 - (data + pos)));
			if ((key == "KTXorientation" && value != "rd" && value != std::string_view("rd\0", 3)) || (key == "KTXswizzle" && value != "rgba" && value != std::string_view("rgba\0", 5)))
				return nullptr;
			pos += (length + 3u) & ~size_t(3);
			if (pos > end)
				return nullptr;
		}
		Initialize();
		basist::ktx2_transcoder transcoder;
		const bool native = format == 145 || format == 146;
		if (native ? (dfdSize != 44 || data[dfd + 12] != 134 || data[dfd + 14] != (format == 146 ? 2 : 1) || Read32(data + 44) != 0 || Read32(data + 16) != 1) : (format != 0 || !transcoder.init(data, static_cast<uint32_t>(size)) || !transcoder.is_ldr() || !transcoder.start_transcoding()))
			return nullptr;
		auto texture = CreateRef<TextureSource>();
		texture->Width = width;
		texture->Height = height;
		texture->Format = TextureFormat::BC7;
		texture->SRGB = native ? format == 146 : data[dfd + 14] == 2;
		uint32_t w = width, h = height;
		for (uint32_t mip = 0; mip < levels; mip++)
		{
			const size_t bytes = LevelSize(w, h), offset = texture->Pixels.size();
			if (bytes > MaxPayload - offset)
				return nullptr;
			const uint8_t* index = data + 80 + size_t(mip) * 24;
			const uint64_t start = Read64(index), length = Read64(index + 8);
			if (start > size || length > size - start || (native && (length != bytes || Read64(index + 16) != bytes || start % 16 || start < dfd + dfdSize)))
				return nullptr;
			texture->MipOffsets.push_back(offset);
			texture->Pixels.resize(offset + bytes);
			if (native)
				std::memcpy(texture->Pixels.data() + offset, data + start, bytes);
			else if (!transcoder.transcode_image_level(mip, 0, 0, texture->Pixels.data() + offset, static_cast<uint32_t>(bytes / 16), basist::transcoder_texture_format::cTFBC7_RGBA))
				return nullptr;
			w = std::max(1u, w / 2);
			h = std::max(1u, h / 2);
		}
		texture->UpdateMemoryUsage();
		outError.clear();
		return texture;
	}

	bool TextureCompression::EncodeKTX2(const TextureSource& source, bool srgb, std::vector<uint8_t>& out, std::string& outError)
	{
		out.clear();
		if (source.Format != TextureFormat::RGBA8 || !source.Width || !source.Height || source.Width > 16384 || source.Height > 16384 || source.Pixels.size() != size_t(source.Width) * source.Height * 4)
		{
			outError = "BC7 compression requires a valid RGBA8 source (at most 16384 per side)";
			return false;
		}
		Initialize();
		uint32_t levels = 1;
		for (uint32_t n = std::max(source.Width, source.Height); n > 1; n /= 2)
			levels++;
		size_t payload = 0;
		for (uint32_t mip = 0; mip < levels; mip++)
			payload += LevelSize(std::max(1u, source.Width >> mip), std::max(1u, source.Height >> mip));
		if (payload > MaxPayload - 512)
		{
			outError = "BC7 mip chain exceeds the 256 MiB KTX2 limit";
			return false;
		}
		const size_t dfd = 80 + levels * 24;
		out.resize((dfd + 44 + 15) & ~size_t(15));
		std::copy(Identifier.begin(), Identifier.end(), out.begin());
		Put(out, 12, srgb ? 146 : 145);
		Put(out, 16, 1);
		Put(out, 20, source.Width);
		Put(out, 24, source.Height);
		Put(out, 36, 1);
		Put(out, 40, levels);
		Put(out, 48, dfd);
		Put(out, 52, 44);
		Put(out, dfd, 44);
		Put(out, dfd + 8, 2 | (40u << 16));
		out[dfd + 12] = 134;
		out[dfd + 13] = 1;
		out[dfd + 14] = srgb ? 2 : 1;
		out[dfd + 16] = 3;
		out[dfd + 17] = 3;
		out[dfd + 20] = 16;
		out[dfd + 30] = 127;
		Put(out, dfd + 40, 0xFFFFFFFFu);
		bc7enc_compress_block_params params;
		bc7enc_compress_block_params_init(&params);
		if (!srgb)
			bc7enc_compress_block_params_init_linear_weights(&params);
		std::vector<std::vector<uint8_t>> encoded(levels);
		std::vector<uint8_t> pixels = source.Pixels;
		uint32_t w = source.Width, h = source.Height;
		for (uint32_t mip = 0; mip < levels; mip++)
		{
			encoded[mip].resize(LevelSize(w, h));
			for (uint32_t by = 0; by < (h + 3) / 4; by++)
				for (uint32_t bx = 0; bx < (w + 3) / 4; bx++)
				{
					std::array<uint8_t, 64> block{};
					for (uint32_t y = 0; y < 4; y++)
						for (uint32_t x = 0; x < 4; x++)
							std::memcpy(block.data() + (size_t(y) * 4 + x) * 4, pixels.data() + (size_t(std::min(by * 4 + y, h - 1)) * w + std::min(bx * 4 + x, w - 1)) * 4, 4);
					bc7enc_compress_block(encoded[mip].data() + (size_t(by) * ((w + 3) / 4) + bx) * 16, block.data(), &params);
				}
			if (mip + 1 == levels)
				break;
			const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
			std::vector<uint8_t> next(size_t(nw) * nh * 4);
			// Area filtering includes the final row/column of odd-sized sources.
			for (uint32_t y = 0; y < nh; y++)
				for (uint32_t x = 0; x < nw; x++)
					for (uint32_t c = 0; c < 4; c++)
					{
						double sum = 0;
						uint32_t count = 0;
						for (uint32_t sy = y * h / nh; sy < (y + 1) * h / nh; sy++)
							for (uint32_t sx = x * w / nw; sx < (x + 1) * w / nw; sx++)
							{
								double v = pixels[(size_t(sy) * w + sx) * 4 + c] / 255.0;
								if (srgb && c < 3)
									v = v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
								sum += v;
								count++;
							}
						double v = sum / count;
						if (srgb && c < 3)
							v = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
						next[(size_t(y) * nw + x) * 4 + c] = static_cast<uint8_t>(std::clamp(std::lround(v * 255), 0l, 255l));
					}
			pixels = std::move(next);
			w = nw;
			h = nh;
		}
		// KTX2 stores the smallest level first; the index remains in largest-first order.
		for (uint32_t mip = levels; mip-- > 0;)
		{
			Put(out, 80 + size_t(mip) * 24, out.size(), 8);
			Put(out, 88 + size_t(mip) * 24, encoded[mip].size(), 8);
			Put(out, 96 + size_t(mip) * 24, encoded[mip].size(), 8);
			out.insert(out.end(), encoded[mip].begin(), encoded[mip].end());
		}
		outError.clear();
		return true;
	}

	bool TextureCompression::Import(const std::filesystem::path& source, const std::filesystem::path& output, const std::filesystem::path& cacheDirectory, bool srgb, bool& cacheHit, std::string& outError)
	{
		cacheHit = false;
		std::error_code pathError;
		if (source.lexically_normal() == output.lexically_normal() || std::filesystem::equivalent(source, output, pathError))
		{
			outError = "compressed output must be a separate file from the source";
			return false;
		}
		const auto bytes = FileSystem::ReadBinaryFile(source);
		if (!bytes || bytes->empty())
		{
			outError = "cannot read texture source";
			return false;
		}
		std::string key(reinterpret_cast<const char*>(bytes->data()), bytes->size());
		key += srgb ? "bc7-v1-srgb" : "bc7-v1-linear";
		const auto cache = cacheDirectory / (ToHex64(HashFnv1a64(key)) + ".ktx2");
		std::vector<uint8_t> encoded;
		if (auto cached = FileSystem::ReadBinaryFile(cache); cached && LoadKTX2(cached->data(), cached->size(), outError))
		{
			encoded = std::move(*cached);
			cacheHit = true;
		}
		else
		{
			const auto texture = TextureSource::LoadFromMemory(bytes->data(), bytes->size(), outError);
			if (!texture || !EncodeKTX2(*texture, srgb, encoded, outError))
				return false;
			if (!FileSystem::WriteBinaryFile(cache, encoded.data(), encoded.size()))
			{
				outError = "cannot write compressed texture cache";
				return false;
			}
		}
		if (!FileSystem::WriteBinaryFile(output, encoded.data(), encoded.size()))
		{
			outError = "cannot write compressed texture output";
			return false;
		}
		outError.clear();
		return true;
	}

	Ref<TextureSource> TextureSource::DecodeRGBA8(std::string& outError) const
	{
		if (Width > 16384 || Height > 16384 || Format != TextureFormat::BC7 || MipOffsets.empty() || MipOffsets[0] != 0 || !Width || !Height || Pixels.size() < LevelSize(Width, Height))
		{
			outError = "invalid BC7 source";
			return nullptr;
		}
		auto result = CreateRef<TextureSource>();
		result->Width = Width;
		result->Height = Height;
		result->SRGB = SRGB;
		result->Pixels.resize(size_t(Width) * Height * 4);
		for (uint32_t by = 0; by < (Height + 3) / 4; by++)
			for (uint32_t bx = 0; bx < (Width + 3) / 4; bx++)
			{
				bc7decomp::color_rgba block[16];
				if (!bc7decomp::unpack_bc7(Pixels.data() + (size_t(by) * ((Width + 3) / 4) + bx) * 16, block))
				{
					outError = "invalid BC7 block";
					return nullptr;
				}
				for (uint32_t y = 0; y < 4 && by * 4 + y < Height; y++)
					for (uint32_t x = 0; x < 4 && bx * 4 + x < Width; x++)
						std::memcpy(result->Pixels.data() + (size_t(by * 4 + y) * Width + size_t(bx) * 4 + x) * 4, block[y * 4 + x].m_comps, 4);
			}
		result->UpdateMemoryUsage();
		outError.clear();
		return result;
	}
}
