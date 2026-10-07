#include "Basalt/Asset/ImageUtils.h"

#include "Basalt/Asset/TextureSource.h"
#include "Basalt/Core/FileSystem.h"

#include <stb_image_write.h>

#include <algorithm>
#include <cstdlib>

namespace Basalt {

	bool WritePng(const std::filesystem::path& path, const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height, std::string& outError)
	{
		if (rgba.size() < static_cast<size_t>(width) * height * 4)
		{
			outError = "pixel buffer is too small";
			return false;
		}
		std::string png;
		const int ok = stbi_write_png_to_func([](void* context, void* data, int size) { static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<size_t>(size)); },
											  &png, static_cast<int>(width), static_cast<int>(height), 4, rgba.data(), static_cast<int>(width * 4));
		if (!ok || !FileSystem::WriteBinaryFile(path, png.data(), png.size()))
		{
			outError = "could not write '" + path.string() + "'";
			return false;
		}
		return true;
	}

	ImageCompareResult CompareImages(const TextureSource& actual, const TextureSource& reference, const ImageCompareOptions& options, bool makeDiffImage)
	{
		ImageCompareResult result;
		result.Width = actual.Width;
		result.Height = actual.Height;
		const size_t pixelCount = static_cast<size_t>(actual.Width) * actual.Height;
		result.SizeMatches = actual.Width == reference.Width && actual.Height == reference.Height && actual.Format == TextureFormat::RGBA8 &&
							 reference.Format == TextureFormat::RGBA8 && actual.Pixels.size() >= pixelCount * 4 && reference.Pixels.size() >= pixelCount * 4;
		if (!result.SizeMatches || pixelCount == 0)
			return result;

		if (makeDiffImage)
			result.DiffImage.resize(pixelCount * 4);

		size_t differing = 0;
		uint64_t deltaSum = 0;
		for (size_t i = 0; i < pixelCount; i++)
		{
			const uint8_t* a = actual.Pixels.data() + i * 4;
			const uint8_t* b = reference.Pixels.data() + i * 4;
			int delta = 0;
			for (int c = 0; c < 3; c++)
				delta = std::max(delta, std::abs(static_cast<int>(a[c]) - static_cast<int>(b[c])));

			deltaSum += static_cast<uint64_t>(delta);
			result.MaxDelta = std::max(result.MaxDelta, static_cast<uint8_t>(delta));
			const bool differs = delta > options.PixelThreshold;
			if (differs)
				differing++;

			if (makeDiffImage)
			{
				uint8_t* out = result.DiffImage.data() + i * 4;
				const auto gray = static_cast<uint8_t>(std::min(delta * 4, 255));
				out[0] = differs ? 255 : gray;
				out[1] = differs ? 0 : gray;
				out[2] = differs ? 0 : gray;
				out[3] = 255;
			}
		}

		result.MeanDelta = static_cast<double>(deltaSum) / static_cast<double>(pixelCount);
		result.DifferingPercent = 100.0 * static_cast<double>(differing) / static_cast<double>(pixelCount);
		result.Matches = result.DifferingPercent <= options.MaxDifferingPercent;
		return result;
	}

}
