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
	const auto decoded = texture->DecodeRGBA8(error);
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
	bool cacheHit = true;
	REQUIRE(WritePng(root / "source.png", source.Pixels, source.Width, source.Height, error));
	const auto original = FileSystem::ReadBinaryFile(root / "source.png");
	REQUIRE(TextureCompression::Import(root / "source.png", root / "color.ktx2", root / "Cache", true, cacheHit, error));
	CHECK_FALSE(cacheHit);
	REQUIRE(TextureCompression::Import(root / "source.png", root / "color.ktx2", root / "Cache", true, cacheHit, error));
	CHECK(cacheHit);
	REQUIRE(TextureCompression::Import(root / "source.png", root / "data.ktx2", root / "Cache", false, cacheHit, error));
	CHECK_FALSE(cacheHit);
	CHECK_FALSE(TextureSource::LoadFromFile(root / "data.ktx2", error)->SRGB);
	CHECK_FALSE(TextureCompression::Import(root / "source.png", root / "source.png", root / "Cache", true, cacheHit, error));
	CHECK(FileSystem::ReadBinaryFile(root / "source.png") == original);
	for (const auto& entry : std::filesystem::directory_iterator(root / "Cache"))
		REQUIRE(FileSystem::WriteTextFile(entry.path(), "corrupt"));
	REQUIRE(TextureCompression::Import(root / "source.png", root / "color.ktx2", root / "Cache", true, cacheHit, error));
	CHECK_FALSE(cacheHit);
	source.Pixels[0] = 0;
	REQUIRE(WritePng(root / "source.png", source.Pixels, source.Width, source.Height, error));
	REQUIRE(TextureCompression::Import(root / "source.png", root / "color.ktx2", root / "Cache", true, cacheHit, error));
	CHECK_FALSE(cacheHit);
	CHECK_FALSE(TextureCompression::Import(root / "missing.png", root / "color.ktx2", root / "Cache", true, cacheHit, error));
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
		CHECK_FALSE(error.empty());
		CHECK(info->Serialize(entity) == saved);
		CHECK_FALSE(info->Deserialize(entity, { { field, -0.1 } }, error));
		CHECK(info->Serialize(entity) == saved);
	}
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
		auto decoded = texture->DecodeRGBA8(error);
		REQUIRE(decoded);
		for (size_t pixel = 0; pixel < 64; pixel++)
		{
			CHECK(std::abs(int(decoded->Pixels[pixel * 4]) - 180) < 12);
			CHECK(std::abs(int(decoded->Pixels[pixel * 4 + 1]) - 40) < 12);
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
	const auto mip = srgb->DecodeRGBA8(error, 1);
	REQUIRE(mip);
	CHECK(mip->Width == 1);
	CHECK(mip->Height == 1);
	CHECK(mip->Pixels[0] > 150);
	CHECK(mip->Pixels[0] < 160);
	REQUIRE(TextureCompression::EncodeKTX2(source, false, bytes, error));
	auto linear = TextureSource::LoadFromMemory(bytes.data(), bytes.size(), error);
	REQUIRE(linear);
	const auto dataMip = linear->DecodeRGBA8(error, 1);
	REQUIRE(dataMip);
	CHECK(dataMip->Pixels[0] > 80);
	CHECK(dataMip->Pixels[0] < 90);
	srgb->MipOffsets[1]++;
	CHECK_FALSE(srgb->HasValidBC7Layout());
	CHECK_FALSE(srgb->DecodeRGBA8(error));
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

TEST_CASE("Compression cache is optional while output errors retain their filesystem cause")
{
	BasaltTest::TempDirectory temp("CompressionIO");
	const auto& root = temp.GetPath();
	const auto source = Source();
	std::string error;
	bool hit = false;
	REQUIRE(WritePng(root / "source.png", source.Pixels, source.Width, source.Height, error));
	REQUIRE(FileSystem::WriteTextFile(root / "BlockedCache", "file"));
	REQUIRE_MESSAGE(TextureCompression::Import(root / "source.png", root / "out.ktx2", root / "BlockedCache", true, hit, error), error);
	CHECK_FALSE(hit);
	CHECK(FileSystem::Exists(root / "out.ktx2"));
	REQUIRE(FileSystem::WriteTextFile(root / "BlockedOutput", "file"));
	CHECK_FALSE(TextureCompression::Import(root / "source.png", root / "BlockedOutput/out.ktx2", root / "Cache", true, hit, error));
	CHECK(error.find("BlockedOutput") != std::string::npos);
}

TEST_CASE("Malformed Zstandard texture fixture reports a decode error")
{
	std::string error;
	CHECK_FALSE(TextureSource::LoadFromFile(std::filesystem::path(BASALT_TEST_DATA_DIR) / "Textures/ZstdRegression.ktx2", error));
	CHECK(error.find("Basis transcode failed") != std::string::npos);
}
