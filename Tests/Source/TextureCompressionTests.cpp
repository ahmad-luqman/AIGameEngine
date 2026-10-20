#include <doctest/doctest.h>

#include <Basalt/Asset/TextureCompression.h>
#include <Basalt/Asset/ImageUtils.h>
#include <Basalt/Asset/Thumbnails.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Core/FileSystem.h>

#include "TestUtils.h"

#include <algorithm>

using namespace Basalt;

namespace {
	TextureSource Source(uint32_t width = 7, uint32_t height = 5)
	{
		TextureSource source;
		source.Width = width;
		source.Height = height;
		source.Pixels.resize(size_t(width) * height * 4);
		for (size_t i = 0; i < source.Pixels.size(); i += 4)
		{
			source.Pixels[i] = 190;
			source.Pixels[i + 1] = 35;
			source.Pixels[i + 2] = 70;
			source.Pixels[i + 3] = 110;
		}
		return source;
	}
}

TEST_CASE("BC7 KTX2 preserves odd dimensions alpha mip payloads and CPU previews")
{
	std::string error;
	std::vector<uint8_t> encoded;
	const auto source = Source();
	REQUIRE(TextureCompression::EncodeKTX2(source, true, encoded, error));
	auto texture = TextureSource::LoadFromMemory(encoded.data(), encoded.size(), error);
	REQUIRE_MESSAGE(texture, error);
	CHECK(texture->Width == 7);
	CHECK(texture->Height == 5);
	CHECK(texture->Format == TextureFormat::BC7);
	CHECK(texture->SRGB);
	CHECK(texture->MipOffsets == std::vector<size_t>{ 0, 64, 80 });
	CHECK(texture->Pixels.size() == 96);
	const auto decoded = texture->DecodeRGBA8(0, error);
	REQUIRE(decoded);
	for (size_t i = 0; i < decoded->Pixels.size(); i++)
		CHECK(std::abs(int(decoded->Pixels[i]) - int(source.Pixels[i])) <= 4);
	const auto preview = MakeImageThumbnail(*texture, 4);
	CHECK(preview.Width == 4);
	CHECK(preview.Height == 3);
	CHECK(preview.Pixels.size() == 48);
	std::vector<uint8_t> repeated;
	REQUIRE(TextureCompression::EncodeKTX2(source, true, repeated, error));
	CHECK(repeated == encoded);
}

TEST_CASE("KTX2 rejects truncated overflowing unsupported and inconsistent headers")
{
	std::string error;
	std::vector<uint8_t> valid;
	REQUIRE(TextureCompression::EncodeKTX2(Source(), false, valid, error));
	for (size_t size = 0; size < valid.size(); size++)
	{
		CHECK_FALSE(TextureSource::LoadFromMemory(valid.data(), size, error));
	}
	for (size_t offset : { 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 80, 88, 96 })
	{
		auto bad = valid;
		std::fill_n(bad.begin() + static_cast<ptrdiff_t>(offset), 4, uint8_t(255));
		CHECK_FALSE(TextureSource::LoadFromMemory(bad.data(), bad.size(), error));
		CHECK_FALSE(error.empty());
	}
	auto source = Source();
	source.Format = TextureFormat::RGBA32F;
	CHECK_FALSE(TextureCompression::EncodeKTX2(source, false, valid, error));
	CHECK(error.find("HDR sources") != std::string::npos);
	source = Source();
	source.Pixels.pop_back();
	CHECK_FALSE(TextureCompression::EncodeKTX2(source, false, valid, error));
}

TEST_CASE("BC7 imports keep sources reuse content cache and recover corrupt cache entries")
{
	BasaltTest::TempDirectory temp("TextureImport");
	const auto& root = temp.GetPath();
	auto source = Source();
	std::string error;
	TextureCompression::ImportResult result;
	REQUIRE(WritePng(root / "source.png", source.Pixels, source.Width, source.Height, error));
	const auto original = FileSystem::ReadBinaryFile(root / "source.png");
	REQUIRE(TextureCompression::Import(root / "source.png", root / "color.ktx2", root / "Cache", true, result, error));
	CHECK_FALSE(result.CacheHit);
	CHECK(result.Warnings.empty());
	const auto encoded = FileSystem::ReadBinaryFile(root / "color.ktx2");
	REQUIRE(encoded);
	// A hit must rewrite the output from the cache alone.
	std::filesystem::remove(root / "color.ktx2");
	REQUIRE(TextureCompression::Import(root / "source.png", root / "color.ktx2", root / "Cache", true, result, error));
	CHECK(result.CacheHit);
	CHECK(FileSystem::ReadBinaryFile(root / "color.ktx2") == encoded);
	REQUIRE(TextureCompression::Import(root / "source.png", root / "data.ktx2", root / "Cache", false, result, error));
	CHECK_FALSE(result.CacheHit);
	CHECK_FALSE(TextureSource::LoadFromFile(root / "data.ktx2", error)->SRGB);
	CHECK_FALSE(TextureCompression::Import(root / "source.png", root / "source.png", root / "Cache", true, result, error));
	CHECK(FileSystem::ReadBinaryFile(root / "source.png") == original);
	for (const auto& entry : std::filesystem::directory_iterator(root / "Cache"))
		REQUIRE(FileSystem::WriteTextFile(entry.path(), "corrupt"));
	REQUIRE(TextureCompression::Import(root / "source.png", root / "color.ktx2", root / "Cache", true, result, error));
	CHECK_FALSE(result.CacheHit);
	REQUIRE(result.Warnings.size() == 1);
	CHECK(result.Warnings[0].find("rebuilding invalid cache entry") != std::string::npos);
	CHECK(FileSystem::ReadBinaryFile(root / "color.ktx2") == encoded);
	source.Pixels[0] = 0;
	REQUIRE(WritePng(root / "source.png", source.Pixels, source.Width, source.Height, error));
	REQUIRE(TextureCompression::Import(root / "source.png", root / "color.ktx2", root / "Cache", true, result, error));
	CHECK_FALSE(result.CacheHit);
	CHECK(FileSystem::ReadBinaryFile(root / "color.ktx2") != encoded);
	CHECK_FALSE(TextureCompression::Import(root / "missing.png", root / "color.ktx2", root / "Cache", true, result, error));
	// No cache directory: always encodes and writes nothing besides the output.
	REQUIRE(TextureCompression::Import(root / "source.png", root / "uncached.ktx2", {}, true, result, error));
	CHECK_FALSE(result.CacheHit);
	CHECK(result.Warnings.empty());
	// A compressed source is not re-encoded.
	CHECK_FALSE(TextureCompression::Import(root / "color.ktx2", root / "twice.ktx2", root / "Cache", true, result, error));
	CHECK(error.find("already a compressed KTX2") != std::string::npos);
	CHECK(error.find("color.ktx2") != std::string::npos);
}

TEST_CASE("Material extension fields round trip transactionally through the shared registry")
{
	Scene scene;
	auto entity = scene.CreateEntity("Material");
	const auto* info = ComponentRegistry::Find("Material");
	REQUIRE(info);
	std::string error;
	const nlohmann::json patch = { { "Clearcoat", 0.8f }, { "ClearcoatRoughness", 0.3f }, { "Transmission", 0.6f }, { "ClearcoatMap", "Assets/coat.ktx2" }, { "ClearcoatRoughnessMap", "Assets/rough.ktx2" }, { "ClearcoatNormalMap", "Assets/normal.ktx2" }, { "TransmissionMap", "Assets/glass.ktx2" }, { "UVOffset", { 0.2f, 0.3f } }, { "UVScale", { 2, -1 } }, { "UVRotation", 0.5 } };
	REQUIRE_MESSAGE(info->Deserialize(entity, patch, error), error);
	const auto saved = info->Serialize(entity);
	for (auto it = patch.begin(); it != patch.end(); ++it)
		CHECK(saved.at(it.key()) == it.value());
	for (const auto* field : { "Clearcoat", "ClearcoatRoughness", "Transmission" })
	{
		CHECK_FALSE(info->Deserialize(entity, { { field, 1.1 } }, error));
		CHECK(error.find(field) != std::string::npos);
		CHECK(info->Serialize(entity) == saved);
		CHECK_FALSE(info->Deserialize(entity, { { field, -0.1 } }, error));
		CHECK(info->Serialize(entity) == saved);
	}
	CHECK_FALSE(info->Deserialize(entity, { { "EmissiveIntensity", -1 } }, error));
	CHECK(error.find("EmissiveIntensity") != std::string::npos);
	CHECK(info->Serialize(entity) == saved);
	REQUIRE_MESSAGE(info->Deserialize(entity, { { "EmissiveIntensity", 0 } }, error), error);
	for (const auto* field : { "ClearcoatMap", "ClearcoatRoughnessMap", "ClearcoatNormalMap", "TransmissionMap" })
		CHECK(std::find(info->AssetFields.begin(), info->AssetFields.end(), field) != info->AssetFields.end());
}

TEST_CASE("Basis ETC1S and Zstd UASTC KTX2 transcode to BC7 with all mip levels")
{
	for (const auto* name : { "ETC1S.ktx2", "UASTC.ktx2" })
	{
		std::string error;
		auto texture = TextureSource::LoadFromFile(std::filesystem::path(BASALT_TEST_DATA_DIR) / "Textures" / name, error);
		REQUIRE_MESSAGE(texture, name, ": ", error);
		CHECK(texture->Format == TextureFormat::BC7);
		CHECK(texture->Width == 8);
		CHECK(texture->Height == 8);
		CHECK(texture->MipOffsets == std::vector<size_t>{ 0, 64, 80, 96 });
		auto decoded = texture->DecodeRGBA8(0, error);
		REQUIRE(decoded);
		CHECK(texture->SRGB); // basisu tags its default (perceptual) encodes as sRGB in the DFD
		for (size_t pixel = 0; pixel < 64; pixel++)
		{
			CHECK(std::abs(int(decoded->Pixels[pixel * 4]) - 180) < 12);
			CHECK(std::abs(int(decoded->Pixels[pixel * 4 + 1]) - 40) < 12);
			CHECK(std::abs(int(decoded->Pixels[pixel * 4 + 2]) - 70) < 12);
			CHECK(std::abs(int(decoded->Pixels[pixel * 4 + 3]) - 110) < 12);
		}
	}
}

TEST_CASE("KTX2 rejects overflowing and truncated Basis global data before third-party decoding")
{
	const auto original = FileSystem::ReadBinaryFile(std::filesystem::path(BASALT_TEST_DATA_DIR) / "Textures/ETC1S.ktx2");
	REQUIRE(original);
	for (const auto& range : { std::pair<uint64_t, uint64_t>{ UINT64_MAX - 15, 256 }, { UINT64_MAX, UINT64_MAX }, { original->size() - 1, 1 }, { 80, 1 } })
	{
		auto data = *original;
		for (size_t i = 0; i < 8; i++)
		{
			data[64 + i] = static_cast<uint8_t>(range.first >> (i * 8));
			data[72 + i] = static_cast<uint8_t>(range.second >> (i * 8));
		}
		std::string error;
		CHECK_FALSE(TextureSource::LoadFromMemory(data.data(), data.size(), error));
		CHECK_FALSE(error.empty());
	}
}

TEST_CASE("BC7 layout rejects mutated offsets and preserves gamma-correct odd mip averages")
{
	TextureSource source;
	source.Width = 3;
	source.Height = 3;
	source.Pixels.resize(36, 0);
	for (size_t i = 0; i < 9; i++)
		source.Pixels[i * 4 + 3] = 255;
	for (size_t i = 6; i < 9; i++)
		source.Pixels[i * 4] = source.Pixels[i * 4 + 1] = source.Pixels[i * 4 + 2] = 255;
	std::string error;
	std::vector<uint8_t> bytes;
	REQUIRE(TextureCompression::EncodeKTX2(source, true, bytes, error));
	auto srgb = TextureSource::LoadFromMemory(bytes.data(), bytes.size(), error);
	REQUIRE(srgb);
	REQUIRE(srgb->HasValidBC7Layout());
	const auto mip = srgb->DecodeRGBA8(1, error);
	REQUIRE(mip);
	CHECK(mip->Width == 1);
	CHECK(mip->Height == 1);
	CHECK(mip->Pixels[0] > 150);
	CHECK(mip->Pixels[0] < 160);
	REQUIRE(TextureCompression::EncodeKTX2(source, false, bytes, error));
	auto linear = TextureSource::LoadFromMemory(bytes.data(), bytes.size(), error);
	REQUIRE(linear);
	const auto dataMip = linear->DecodeRGBA8(1, error);
	REQUIRE(dataMip);
	CHECK(dataMip->Pixels[0] > 80);
	CHECK(dataMip->Pixels[0] < 90);
	srgb->MipOffsets[1]++;
	CHECK_FALSE(srgb->HasValidBC7Layout());
	CHECK_FALSE(srgb->DecodeRGBA8(0, error));
	CHECK(error == "invalid BC7 mip layout");
}

TEST_CASE("BC7 layout validation rejects every malformed chain")
{
	std::string error;
	std::vector<uint8_t> bytes;
	REQUIRE(TextureCompression::EncodeKTX2(Source(8, 2), true, bytes, error));
	const auto valid = TextureSource::LoadFromMemory(bytes.data(), bytes.size(), error);
	REQUIRE(valid);
	REQUIRE(valid->HasValidBC7Layout());
	CHECK(valid->MipOffsets.size() == 4); // 8x2, 4x1, 2x1, 1x1
	auto mutate = [&](const char* name, auto&& change) {
		INFO(name);
		TextureSource copy;
		copy.Width = valid->Width;
		copy.Height = valid->Height;
		copy.Format = valid->Format;
		copy.Pixels = valid->Pixels;
		copy.MipOffsets = valid->MipOffsets;
		change(copy);
		CHECK_FALSE(copy.HasValidBC7Layout());
	};
	mutate("trailing byte", [](TextureSource& t) { t.Pixels.push_back(0); });
	mutate("truncated pixels", [](TextureSource& t) { t.Pixels.pop_back(); });
	mutate("level past 1x1", [](TextureSource& t) {
		t.MipOffsets.push_back(t.Pixels.size());
		t.Pixels.resize(t.Pixels.size() + 16);
	});
	mutate("base offset", [](TextureSource& t) { t.MipOffsets[0] = 16; });
	mutate("no levels", [](TextureSource& t) {
		t.MipOffsets.clear();
		t.Pixels.clear();
	});
	mutate("not BC7", [](TextureSource& t) { t.Format = TextureFormat::RGBA8; });
	mutate("too wide", [](TextureSource& t) { t.Width = TextureSource::MaxDimension + 1; });
	// A chain may stop before 1x1.
	TextureSource prefix;
	prefix.Width = valid->Width;
	prefix.Height = valid->Height;
	prefix.Format = TextureFormat::BC7;
	prefix.MipOffsets = { 0, TextureSource::BC7LevelSize(8, 2, 0) };
	prefix.Pixels.resize(TextureSource::BC7LevelSize(8, 2, 0) + TextureSource::BC7LevelSize(8, 2, 1));
	CHECK(prefix.HasValidBC7Layout());
}

TEST_CASE("BC7 mip decoding uses each level's own block rows")
{
	// Four solid quadrants: every mip down to 4x4 keeps them, and the 16x16 level needs several block rows.
	TextureSource source;
	source.Width = source.Height = 16;
	source.Pixels.resize(size_t{ 16 } * 16 * 4);
	const std::array<std::array<uint8_t, 4>, 4> colors = { { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 255, 255, 255, 255 } } };
	for (uint32_t y = 0; y < 16; y++)
		for (uint32_t x = 0; x < 16; x++)
			std::copy_n(colors[(y / 8) * 2 + x / 8].begin(), 4, source.Pixels.begin() + static_cast<ptrdiff_t>((size_t(y) * 16 + x) * 4));
	std::string error;
	std::vector<uint8_t> bytes;
	REQUIRE(TextureCompression::EncodeKTX2(source, false, bytes, error));
	const auto texture = TextureSource::LoadFromMemory(bytes.data(), bytes.size(), error);
	REQUIRE(texture);
	for (uint32_t mip = 0; mip < 3; mip++)
	{
		INFO("mip " << mip);
		const auto decoded = texture->DecodeRGBA8(mip, error);
		REQUIRE_MESSAGE(decoded, error);
		const uint32_t size = 16 >> mip;
		CHECK(decoded->Width == size);
		CHECK(decoded->Height == size);
		for (uint32_t quadrant = 0; quadrant < 4; quadrant++)
		{
			const uint32_t x = (quadrant % 2) * size / 2 + size / 4, y = (quadrant / 2) * size / 2 + size / 4;
			for (uint32_t c = 0; c < 4; c++)
				CHECK(std::abs(int(decoded->Pixels[(size_t(y) * size + x) * 4 + c]) - int(colors[quadrant][c])) <= 4);
		}
	}
	CHECK_FALSE(texture->DecodeRGBA8(static_cast<uint32_t>(texture->MipOffsets.size()), error));
	CHECK(error.find("out of range") != std::string::npos);
}

TEST_CASE("KTX2 rejections name their cause")
{
	std::string error;
	std::vector<uint8_t> valid;
	REQUIRE(TextureCompression::EncodeKTX2(Source(), true, valid, error));
	auto expect = [&](const std::vector<uint8_t>& data, const char* cause) {
		INFO(cause);
		CHECK_FALSE(TextureSource::LoadFromMemory(data.data(), data.size(), error));
		CHECK(error.rfind("KTX2: ", 0) == 0);
		CHECK(error.find(cause) != std::string::npos);
	};
	auto put = [](std::vector<uint8_t>& data, size_t offset, uint32_t value) {
		for (size_t i = 0; i < 4; i++)
			data[offset + i] = static_cast<uint8_t>(value >> (8 * i));
	};
	// Appends one key/value entry and points the metadata range at it.
	auto withMetadata = [&](const std::string& key, const std::string& value) {
		auto data = valid;
		const std::string entry = key + '\0' + value + '\0';
		const size_t start = data.size();
		put(data, 56, static_cast<uint32_t>(start));
		data.resize(start + 4);
		put(data, start, static_cast<uint32_t>(entry.size()));
		data.insert(data.end(), entry.begin(), entry.end());
		data.resize((data.size() + 3) & ~size_t(3));
		put(data, 60, static_cast<uint32_t>(data.size() - start));
		return data;
	};
	const auto upright = withMetadata("KTXorientation", "rd");
	CHECK_MESSAGE(TextureSource::LoadFromMemory(upright.data(), upright.size(), error), error);
	expect(withMetadata("KTXorientation", "ru"), "KTXorientation 'ru' is unsupported");
	expect(withMetadata("KTXswizzle", "bgra"), "KTXswizzle 'bgra' is unsupported");
	auto data = valid;
	put(data, 28, 1);
	expect(data, "only 2D textures");
	data = valid;
	put(data, 44, 2);
	expect(data, "supercompressed native BC7");
	data = valid;
	put(data, 12, 37);
	expect(data, "unsupported vkFormat 37");
	data = std::move(valid);
	put(data, 20, TextureSource::MaxDimension + 1);
	expect(data, "dimensions 16385x5");
}

TEST_CASE("BC7 thumbnails report decode failures to the caller")
{
	std::string error;
	std::vector<uint8_t> bytes;
	REQUIRE(TextureCompression::EncodeKTX2(Source(), true, bytes, error));
	auto texture = TextureSource::LoadFromMemory(bytes.data(), bytes.size(), error);
	REQUIRE(texture);
	texture->MipOffsets[1]++;
	std::string reason;
	CHECK_FALSE(MakeImageThumbnail(*texture, 16, &reason).IsValid());
	CHECK(reason == "invalid BC7 mip layout");
}

TEST_CASE("Every material registry field participates in the editor preview key")
{
	Scene scene;
	auto entity = scene.CreateEntity("PreviewKey");
	const auto* info = ComponentRegistry::Find("Material");
	REQUIRE(info);
	entity.AddComponent<MaterialComponent>();
	const auto original = info->Serialize(entity);
	const auto key = MaterialPreviewKey(entity.GetComponent<MaterialComponent>());
	std::string error;
	for (auto it = original.begin(); it != original.end(); ++it)
	{
		INFO(it.key());
		REQUIRE(info->Deserialize(entity, original, error));
		auto value = it.value();
		if (value.is_number())
			value = 0.25f;
		else if (value.is_string())
			value = "Assets/changed.ktx2";
		else if (value.is_array())
			for (auto& element : value)
				element = 0.25f;
		REQUIRE_MESSAGE(info->Deserialize(entity, { { it.key(), value } }, error), error);
		CHECK(MaterialPreviewKey(entity.GetComponent<MaterialComponent>()) != key);
	}
}

TEST_CASE("Material previews track every texture asset field")
{
	// The editor re-renders a material preview when one of these files changes, so none may be missing.
	const auto* info = ComponentRegistry::Find("Material");
	REQUIRE(info);
	MaterialComponent material;
	const auto maps = MaterialPreviewMaps(material);
	CHECK(maps.size() == info->AssetFields.size());
	Scene scene;
	auto entity = scene.CreateEntity("Maps");
	entity.AddComponent<MaterialComponent>();
	std::string error;
	for (const auto& field : info->AssetFields)
	{
		INFO(field);
		REQUIRE(info->Deserialize(entity, { { field, "Assets/" + field + ".png" } }, error));
		const auto tracked = MaterialPreviewMaps(entity.GetComponent<MaterialComponent>());
		CHECK(std::any_of(tracked.begin(), tracked.end(), [&](const std::string* map) { return *map == "Assets/" + field + ".png"; }));
	}
}

TEST_CASE("Compression cache is optional while output errors retain their filesystem cause")
{
	BasaltTest::TempDirectory temp("CompressionIO");
	const auto& root = temp.GetPath();
	const auto source = Source();
	std::string error;
	TextureCompression::ImportResult result;
	REQUIRE(WritePng(root / "source.png", source.Pixels, source.Width, source.Height, error));
	REQUIRE(FileSystem::WriteTextFile(root / "BlockedCache", "file"));
	REQUIRE_MESSAGE(TextureCompression::Import(root / "source.png", root / "out.ktx2", root / "BlockedCache", true, result, error), error);
	CHECK_FALSE(result.CacheHit);
	REQUIRE(result.Warnings.size() == 1);
	CHECK(result.Warnings[0].find("cache unavailable") != std::string::npos);
	CHECK(FileSystem::Exists(root / "out.ktx2"));
	REQUIRE(FileSystem::WriteTextFile(root / "BlockedOutput", "file"));
	CHECK_FALSE(TextureCompression::Import(root / "source.png", root / "BlockedOutput/out.ktx2", root / "Cache", true, result, error));
	CHECK(error.find("BlockedOutput") != std::string::npos);
}

TEST_CASE("Uncompressed Basis levels must store their declared size")
{
	// A FuzzTexture crash: UASTC without supercompression whose level length differs from its uncompressed length
	// reached an assert inside the Basis transcoder.
	std::string error;
	CHECK_FALSE(TextureSource::LoadFromFile(std::filesystem::path(BASALT_TEST_DATA_DIR) / "Textures/UncompressedLengthMismatch.ktx2", error));
	CHECK(error.find("mip 0 stores 26 bytes but declares 64 uncompressed") != std::string::npos);
}

TEST_CASE("Malformed Zstandard texture fixture reports a decode error")
{
	std::string error;
	CHECK_FALSE(TextureSource::LoadFromFile(std::filesystem::path(BASALT_TEST_DATA_DIR) / "Textures/ZstdRegression.ktx2", error));
	CHECK(error.find("Basis transcode failed") != std::string::npos);
}
