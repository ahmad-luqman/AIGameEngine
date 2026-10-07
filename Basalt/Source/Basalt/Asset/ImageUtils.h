#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Basalt {

	class TextureSource;

	// Encodes RGBA8 pixels (top row first) as a PNG file.
	bool WritePng(const std::filesystem::path& path, const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height, std::string& outError);

	// Tolerances for CompareImages. A pixel "differs" when any of its RGB channels is off by more than
	// PixelThreshold; the images match when no more than MaxDifferingPercent of the pixels differ. The
	// defaults allow driver-level noise (filtering, dithering, rounding) but catch any visible change.
	struct ImageCompareOptions
	{
		uint8_t PixelThreshold = 8;
		double MaxDifferingPercent = 0.5;
	};

	struct ImageCompareResult
	{
		bool SizeMatches = false;
		bool Matches = false;
		uint32_t Width = 0;
		uint32_t Height = 0;
		// Largest RGB channel difference over all pixels (0-255).
		uint8_t MaxDelta = 0;
		// Mean of each pixel's largest RGB channel difference.
		double MeanDelta = 0.0;
		double DifferingPercent = 0.0;
		// RGBA8 visualization (filled only when requested): differences amplified 4x in gray, differing
		// pixels in red, so a failing golden test shows where the image changed.
		std::vector<uint8_t> DiffImage;
	};

	// Compares two RGBA8 images; alpha is ignored (captures are opaque). Images of different sizes never match.
	ImageCompareResult CompareImages(const TextureSource& actual, const TextureSource& reference, const ImageCompareOptions& options, bool makeDiffImage);

}
