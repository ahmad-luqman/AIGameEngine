#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Basalt {

	// Sub-assets of a model file share its identity: "<model>#image/<n>" names an embedded image and
	// "<model>#material/<n>" an imported material. Asset references, cache invalidation and thumbnails split keys
	// here so every consumer agrees on which file a key belongs to.
	inline constexpr std::string_view EmbeddedImageMarker = "#image/";
	inline constexpr std::string_view ImportedMaterialMarker = "#material/";

	// The position of the first sub-asset marker in key, or npos.
	inline size_t FindSubAssetMarker(std::string_view key)
	{
		size_t first = std::string_view::npos;
		for (const std::string_view marker : std::array{ EmbeddedImageMarker, ImportedMaterialMarker })
			first = std::min(first, key.find(marker));
		return first;
	}

	// The file a key loads from: the model of a sub-asset, the key itself otherwise.
	inline std::string_view GetAssetSource(std::string_view key)
	{
		return key.substr(0, FindSubAssetMarker(key));
	}

	// The sub-asset part of a key ("#image/0"), empty for a whole file.
	inline std::string_view GetSubAssetFragment(std::string_view key)
	{
		const size_t marker = FindSubAssetMarker(key);
		return marker == std::string_view::npos ? std::string_view{} : key.substr(marker);
	}

	// "<source><marker><index>", e.g. MakeSubAssetKey("Assets/Ship.glb", ImportedMaterialMarker, 0).
	inline std::string MakeSubAssetKey(std::string_view source, std::string_view marker, size_t index)
	{
		std::string key(source);
		key.append(marker).append(std::to_string(index));
		return key;
	}

	// The index after `marker` when key is "<source><marker><decimal index>"; nullopt otherwise (no marker, another
	// marker first, or a malformed index).
	inline std::optional<uint32_t> ParseSubAssetIndex(std::string_view key, std::string_view marker)
	{
		const size_t position = FindSubAssetMarker(key);
		if (position == std::string_view::npos || key.substr(position, marker.size()) != marker)
			return std::nullopt;
		const std::string_view text = key.substr(position + marker.size());
		uint32_t index = 0;
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), index);
		if (text.empty() || error != std::errc() || end != text.data() + text.size())
			return std::nullopt;
		return index;
	}

}
