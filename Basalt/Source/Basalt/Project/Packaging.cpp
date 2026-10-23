#include "Basalt/Project/Packaging.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Base.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Project/Project.h"

#include <spdlog/fmt/fmt.h>
#include <stb_image.h>
#include <stb_image_resize2.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string_view>

#if defined(BS_PLATFORM_WINDOWS)
	#include <windows.h>
#endif

namespace Basalt {

	namespace {

		constexpr std::array<uint8_t, 8> PngSignature = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

		// The sizes a platform icon format holds, limited to what the source can fill (but never empty).
		std::vector<uint32_t> IconSizes(const IconImage& icon, std::initializer_list<uint32_t> sizes)
		{
			const uint32_t side = std::max(icon.Width, icon.Height);
			std::vector<uint32_t> result;
			for (uint32_t size : sizes)
				if (size <= side || result.empty())
					result.push_back(size);
			return result;
		}

		// Letters, digits and hyphens (one per run of anything else), for a bundle identifier component.
		std::string IdentifierPart(const std::string& text)
		{
			std::string result;
			for (char c : text)
			{
				if (std::isalnum(static_cast<unsigned char>(c)) && static_cast<unsigned char>(c) < 0x80)
					result += c;
				else if (!result.empty() && result.back() != '-')
					result += '-';
			}
			while (!result.empty() && result.back() == '-')
				result.pop_back();
			return result;
		}

		// Text for XML character data: markup characters escaped, control characters (invalid in XML 1.0) dropped.
		std::string XmlEscape(std::string_view text)
		{
			std::string result;
			for (char c : text)
			{
				switch (c)
				{
					case '&':
						result += "&amp;";
						break;
					case '<':
						result += "&lt;";
						break;
					case '>':
						result += "&gt;";
						break;
					case '"':
						result += "&quot;";
						break;
					case '\'':
						result += "&apos;";
						break;
					default:
						if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F)
							result += c;
				}
			}
			return result;
		}

		// A desktop entry string value: one line, with the spec's escapes for backslashes and a leading space.
		std::string DesktopEscape(std::string_view text)
		{
			std::string result;
			for (char c : text)
			{
				if (c == '\\')
					result += "\\\\";
				else if (c == ' ' && result.empty())
					result += "\\s";
				else if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F)
					result += c;
			}
			return result;
		}

		// Length of the UTF-8 sequence a lead byte starts; 0 for a continuation or invalid byte.
		size_t Utf8SequenceLength(unsigned char lead)
		{
			if (lead < 0x80)
				return 1;
			if ((lead >> 5) == 0x6)
				return 2;
			if ((lead >> 4) == 0xE)
				return 3;
			if ((lead >> 3) == 0x1E)
				return 4;
			return 0;
		}

		// UTF-16 code units of UTF-8 text; each byte that starts no valid sequence becomes U+FFFD.
		std::u16string Utf8ToUtf16(std::string_view text)
		{
			// The payload bits of a lead byte and the smallest code point of each sequence length (anything below is overlong).
			constexpr std::array<uint32_t, 5> LeadMask = { 0, 0x7F, 0x1F, 0x0F, 0x07 };
			constexpr std::array<uint32_t, 5> Minimum = { 0, 0, 0x80, 0x800, 0x10000 };
			std::u16string result;
			for (size_t i = 0; i < text.size();)
			{
				const auto byte = static_cast<unsigned char>(text[i]);
				const size_t length = Utf8SequenceLength(byte);
				uint32_t code = byte & LeadMask[length];
				bool valid = length != 0 && i + length <= text.size();
				for (size_t j = 1; valid && j < length; ++j)
				{
					const auto next = static_cast<unsigned char>(text[i + j]);
					valid = (next >> 6) == 0x2;
					code = (code << 6) | (next & 0x3Fu);
				}
				// Overlong forms, surrogates and values past U+10FFFF are not characters.
				valid = valid && code >= Minimum[length] && code <= 0x10FFFF && (code < 0xD800 || code > 0xDFFF);
				if (!valid)
				{
					result += u'�';
					++i;
					continue;
				}
				if (code >= 0x10000)
				{
					code -= 0x10000;
					result += static_cast<char16_t>(0xD800 + (code >> 10));
					result += static_cast<char16_t>(0xDC00 + (code & 0x3FF));
				}
				else
					result += static_cast<char16_t>(code);
				i += length;
			}
			return result;
		}

		// Little-endian bytes for Windows resource structures.
		class ResourceWriter
		{
		public:
			void Byte(uint8_t value) { m_Bytes.push_back(value); }
			void Word(uint16_t value)
			{
				Byte(static_cast<uint8_t>(value));
				Byte(static_cast<uint8_t>(value >> 8));
			}
			void DWord(uint32_t value)
			{
				Word(static_cast<uint16_t>(value));
				Word(static_cast<uint16_t>(value >> 16));
			}
			// UTF-16 with its terminating null.
			void String(std::u16string_view text)
			{
				for (char16_t c : text)
					Word(static_cast<uint16_t>(c));
				Word(0);
			}
			void AlignToDWord()
			{
				while (m_Bytes.size() % 4 != 0)
					Byte(0);
			}
			void PatchWord(size_t offset, uint16_t value)
			{
				m_Bytes[offset] = static_cast<uint8_t>(value);
				m_Bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
			}
			size_t Size() const { return m_Bytes.size(); }
			std::vector<uint8_t> Take() { return std::move(m_Bytes); }

			// A version resource block (VS_VERSIONINFO, StringFileInfo, StringTable, String, VarFileInfo, Var): header
			// and key, then whatever writeBody adds (a value and/or child blocks). Its wLength covers all of that.
			template<typename Body>
			void Block(std::u16string_view key, uint16_t valueLength, bool text, Body&& writeBody)
			{
				AlignToDWord();
				const size_t start = Size();
				Word(0);
				Word(valueLength);
				Word(text ? 1 : 0);
				String(key);
				AlignToDWord();
				writeBody();
				// MakePackageInfo bounds the text (ValidateProjectText), so no block outgrows its 16-bit length.
				BS_CORE_ASSERT(Size() - start <= UINT16_MAX, "version resource block too long");
				PatchWord(start, static_cast<uint16_t>(Size() - start));
			}

		private:
			std::vector<uint8_t> m_Bytes;
		};

		std::string CopyrightNotice(const std::string& company)
		{
			return "Copyright \xC2\xA9 " + company;
		}

		void BigEndian32(std::vector<uint8_t>& bytes, uint32_t value)
		{
			for (int shift = 24; shift >= 0; shift -= 8)
				bytes.push_back(static_cast<uint8_t>(value >> shift));
		}

	}

	std::optional<IconImage> LoadIconImage(const std::filesystem::path& path, std::string& outError)
	{
		const std::string name = FileSystem::PathToUtf8(path);
		const auto bytes = FileSystem::ReadBinaryFile(path);
		if (!bytes)
		{
			outError = "cannot read icon '" + name + "'";
			return std::nullopt;
		}
		if (bytes->size() < PngSignature.size() || !std::equal(PngSignature.begin(), PngSignature.end(), bytes->begin()))
		{
			outError = "icon '" + name + "' is not a PNG file";
			return std::nullopt;
		}
		int width = 0;
		int height = 0;
		int channels = 0;
		const int size = static_cast<int>(std::min<size_t>(bytes->size(), INT32_MAX));
		stbi_uc* pixels = nullptr;
		// The size is checked before decoding, so a huge image is never allocated.
		if (stbi_info_from_memory(bytes->data(), size, &width, &height, &channels))
		{
			if (width > static_cast<int>(MaxIconSize) || height > static_cast<int>(MaxIconSize))
			{
				outError = fmt::format("icon '{}' is {} x {} pixels; icons may be at most {} pixels on a side", name, width, height, MaxIconSize);
				return std::nullopt;
			}
			pixels = stbi_load_from_memory(bytes->data(), size, &width, &height, &channels, 4);
		}
		if (!pixels)
		{
			outError = fmt::format("cannot decode icon '{}': {}", name, stbi_failure_reason());
			return std::nullopt;
		}
		IconImage icon;
		icon.Width = static_cast<uint32_t>(width);
		icon.Height = static_cast<uint32_t>(height);
		icon.Pixels.assign(pixels, pixels + static_cast<size_t>(icon.Width) * icon.Height * 4);
		stbi_image_free(pixels);
		return icon;
	}

	bool LoadProjectIcon(const Project& project, std::optional<IconImage>& outIcon, std::string& outError)
	{
		outIcon.reset();
		const std::string& icon = project.GetConfig().Icon;
		if (icon.empty())
			return true;
		if (AssetRegistry::IsUnresolvedKey(icon))
		{
			outError = "the project icon is an " + project.GetAssets().DescribeUnresolved(icon) + " (set it again with project.set icon)";
			return false;
		}
		outIcon = LoadIconImage(project.GetIconPath(), outError);
		return outIcon.has_value();
	}

	IconImage ResizeIcon(const IconImage& icon, uint32_t size)
	{
		BS_CORE_ASSERT(icon.IsValid(), "invalid icon image");
		BS_CORE_ASSERT(size > 0, "icon size must be positive");
		const uint32_t side = std::max(icon.Width, icon.Height);
		IconImage square;
		square.Width = side;
		square.Height = side;
		square.Pixels.assign(static_cast<size_t>(side) * side * 4, 0);
		const uint32_t left = (side - icon.Width) / 2;
		const uint32_t top = (side - icon.Height) / 2;
		for (uint32_t y = 0; y < icon.Height; ++y)
			std::memcpy(&square.Pixels[(static_cast<size_t>(top + y) * side + left) * 4], &icon.Pixels[static_cast<size_t>(y) * icon.Width * 4], static_cast<size_t>(icon.Width) * 4);
		if (side == size)
			return square;

		IconImage result;
		result.Width = size;
		result.Height = size;
		result.Pixels.resize(static_cast<size_t>(size) * size * 4);
		// sRGB and alpha-weighted, so transparent pixels never bleed their (arbitrary) color into the edges.
		if (!stbir_resize_uint8_srgb(square.Pixels.data(), static_cast<int>(side), static_cast<int>(side), 0, result.Pixels.data(), static_cast<int>(size), static_cast<int>(size), 0, STBIR_RGBA))
			return {};
		return result;
	}

	std::vector<uint8_t> EncodeIconPng(const IconImage& icon)
	{
		std::vector<uint8_t> png;
		if (!icon.IsValid())
			return png;
		if (!stbi_write_png_to_func([](void* context, void* data, int size) {
				auto& bytes = *static_cast<std::vector<uint8_t>*>(context);
				const auto* begin = static_cast<const uint8_t*>(data);
				bytes.insert(bytes.end(), begin, begin + size);
			},
									&png, static_cast<int>(icon.Width), static_cast<int>(icon.Height), 4, icon.Pixels.data(), static_cast<int>(icon.Width * 4)))
			png.clear();
		return png;
	}

	IconImage MakeDefaultIcon(uint32_t size)
	{
		// The top of a basalt column: a dark hexagon with a lighter inner face, 4x4 supersampled for smooth edges.
		BS_CORE_ASSERT(size > 0, "icon size must be positive");
		IconImage icon;
		icon.Width = size;
		icon.Height = size;
		icon.Pixels.resize(static_cast<size_t>(size) * size * 4);
		const auto insideHexagon = [](float x, float y, float radius) {
			// A pointy-top regular hexagon centered on the origin.
			const float ax = std::abs(x);
			const float ay = std::abs(y);
			return ax <= radius * 0.8660254f && ay <= radius && ax * 0.5f + ay * 0.8660254f <= radius * 0.8660254f;
		};
		constexpr int Samples = 4;
		for (uint32_t py = 0; py < size; ++py)
		{
			for (uint32_t px = 0; px < size; ++px)
			{
				float coverage = 0.0f;
				float inner = 0.0f;
				for (int sy = 0; sy < Samples; ++sy)
				{
					for (int sx = 0; sx < Samples; ++sx)
					{
						const float x = (static_cast<float>(px) + (static_cast<float>(sx) + 0.5f) / Samples) / static_cast<float>(size) - 0.5f;
						const float y = (static_cast<float>(py) + (static_cast<float>(sy) + 0.5f) / Samples) / static_cast<float>(size) - 0.5f;
						coverage += insideHexagon(x, y, 0.46f) ? 1.0f : 0.0f;
						inner += insideHexagon(x, y, 0.30f) ? 1.0f : 0.0f;
					}
				}
				coverage /= Samples * Samples;
				inner /= Samples * Samples;
				uint8_t* pixel = &icon.Pixels[(static_cast<size_t>(py) * size + px) * 4];
				const float shade = coverage > 0.0f ? inner / coverage : 0.0f;
				pixel[0] = static_cast<uint8_t>(std::lround(58.0f + 52.0f * shade));
				pixel[1] = static_cast<uint8_t>(std::lround(64.0f + 56.0f * shade));
				pixel[2] = static_cast<uint8_t>(std::lround(76.0f + 60.0f * shade));
				pixel[3] = static_cast<uint8_t>(std::lround(255.0f * coverage));
			}
		}
		return icon;
	}

	std::string MakeBundleIdentifier(const std::string& company, const std::string& name)
	{
		const std::string companyPart = IdentifierPart(company);
		const std::string namePart = IdentifierPart(name);
		return "com." + (companyPart.empty() ? std::string("basalt") : companyPart) + "." + (namePart.empty() ? std::string("game") : namePart);
	}

	std::optional<PackageInfo> MakePackageInfo(const ProjectConfig& config, const std::string& executableName, std::string& outError)
	{
		const auto numbers = ParseProjectVersion(config.Version);
		if (!numbers)
		{
			outError = fmt::format("the project Version '{}' must be {}", config.Version, VersionFormat);
			return std::nullopt;
		}
		// The version resource's 16-bit lengths depend on it.
		if (!ValidateProjectText(config, outError))
			return std::nullopt;
		PackageInfo info;
		info.Name = config.Name;
		info.ExecutableName = executableName;
		info.Version = config.Version;
		info.VersionNumbers = *numbers;
		info.Company = config.Company;
		info.BundleIdentifier = MakeBundleIdentifier(config.Company, config.Name);
		return info;
	}

	std::string BuildInfoPlist(const PackageInfo& info)
	{
		// Both version keys are documented as at most three integers, so a BUILD number stays out of them.
		const std::string shortVersion = fmt::format("{}.{}.{}", info.VersionNumbers[0], info.VersionNumbers[1], info.VersionNumbers[2]);
		std::string plist = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
							"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
							"<plist version=\"1.0\">\n<dict>\n";
		const auto add = [&](std::string_view key, std::string_view value) { plist += fmt::format("\t<key>{}</key>\n\t<string>{}</string>\n", key, XmlEscape(value)); };
		add("CFBundleDevelopmentRegion", "en");
		add("CFBundleDisplayName", info.Name);
		add("CFBundleExecutable", info.ExecutableName);
		if (!info.IconFileName.empty())
			add("CFBundleIconFile", info.IconFileName);
		add("CFBundleIdentifier", info.BundleIdentifier);
		add("CFBundleInfoDictionaryVersion", "6.0");
		add("CFBundleName", info.Name);
		add("CFBundlePackageType", "APPL");
		add("CFBundleShortVersionString", shortVersion);
		add("CFBundleVersion", shortVersion);
		add("LSApplicationCategoryType", "public.app-category.games");
		if (!info.Company.empty())
			add("NSHumanReadableCopyright", CopyrightNotice(info.Company));
		plist += "\t<key>NSHighResolutionCapable</key>\n\t<true/>\n";
		plist += "</dict>\n</plist>\n";
		return plist;
	}

	std::optional<std::vector<uint8_t>> BuildIcns(const IconImage& icon)
	{
		// PNG element types, smallest pixel size first; @2x types repeat a size. The set iconutil writes: icp6 is left out
		// because its size is documented inconsistently (iconutil reads it as 48 px).
		struct Element
		{
			const char* Type;
			uint32_t Size;
		};
		constexpr Element Elements[] = { { "icp4", 16 }, { "icp5", 32 }, { "ic11", 32 }, { "ic12", 64 }, { "ic07", 128 }, { "ic08", 256 }, { "ic13", 256 }, { "ic09", 512 }, { "ic14", 512 }, { "ic10", 1024 } };
		const std::vector<uint32_t> sizes = IconSizes(icon, { 16, 32, 64, 128, 256, 512, 1024 });

		std::vector<uint8_t> body;
		uint32_t encodedSize = 0;
		std::vector<uint8_t> png;
		for (const Element& element : Elements)
		{
			if (std::find(sizes.begin(), sizes.end(), element.Size) == sizes.end())
				continue;
			if (encodedSize != element.Size)
			{
				png = EncodeIconPng(ResizeIcon(icon, element.Size));
				encodedSize = element.Size;
				if (png.empty())
					return std::nullopt;
			}
			body.insert(body.end(), element.Type, element.Type + 4);
			BigEndian32(body, static_cast<uint32_t>(png.size() + 8));
			body.insert(body.end(), png.begin(), png.end());
		}
		std::vector<uint8_t> icns = { 'i', 'c', 'n', 's' };
		BigEndian32(icns, static_cast<uint32_t>(body.size() + 8));
		icns.insert(icns.end(), body.begin(), body.end());
		return icns;
	}

	std::string BuildDesktopEntry(const PackageInfo& info)
	{
		return fmt::format("[Desktop Entry]\nType=Application\nName={}\nExec={}\nIcon={}\nCategories=Game;\nTerminal=false\nX-AppImage-Version={}\n", DesktopEscape(info.Name), info.ExecutableName, info.ExecutableName, DesktopEscape(info.Version));
	}

	std::optional<WindowsIconResources> BuildWindowsIcon(const IconImage& icon)
	{
		WindowsIconResources resources;
		const std::vector<uint32_t> sizes = IconSizes(icon, { 16, 24, 32, 48, 64, 128, 256 });
		ResourceWriter directory;
		// GRPICONDIR: reserved, type 1 (icon), count; then one 14-byte GRPICONDIRENTRY per image.
		directory.Word(0);
		directory.Word(1);
		directory.Word(static_cast<uint16_t>(sizes.size()));
		for (size_t i = 0; i < sizes.size(); ++i)
		{
			// Windows Vista and later read PNG-compressed icon images at every size.
			resources.Images.push_back(EncodeIconPng(ResizeIcon(icon, sizes[i])));
			if (resources.Images.back().empty())
				return std::nullopt;
			const auto dimension = static_cast<uint8_t>(sizes[i] >= 256 ? 0 : sizes[i]);
			directory.Byte(dimension);
			directory.Byte(dimension);
			directory.Byte(0);
			directory.Byte(0);
			directory.Word(1);
			directory.Word(32);
			directory.DWord(static_cast<uint32_t>(resources.Images.back().size()));
			directory.Word(static_cast<uint16_t>(WindowsIconResources::FirstIconID + i));
		}
		resources.GroupDirectory = directory.Take();
		return resources;
	}

	std::vector<uint8_t> BuildVersionInfo(const PackageInfo& info)
	{
		const auto& v = info.VersionNumbers;
		const uint32_t versionMS = (static_cast<uint32_t>(v[0]) << 16) | v[1];
		const uint32_t versionLS = (static_cast<uint32_t>(v[2]) << 16) | v[3];
		std::vector<std::pair<std::u16string, std::u16string>> strings;
		if (!info.Company.empty())
			strings.emplace_back(u"CompanyName", Utf8ToUtf16(info.Company));
		strings.emplace_back(u"FileDescription", Utf8ToUtf16(info.Name));
		strings.emplace_back(u"FileVersion", Utf8ToUtf16(info.Version));
		strings.emplace_back(u"InternalName", Utf8ToUtf16(info.ExecutableName));
		if (!info.Company.empty())
			strings.emplace_back(u"LegalCopyright", Utf8ToUtf16(CopyrightNotice(info.Company)));
		strings.emplace_back(u"OriginalFilename", Utf8ToUtf16(info.ExecutableName + ".exe"));
		strings.emplace_back(u"ProductName", Utf8ToUtf16(info.Name));
		strings.emplace_back(u"ProductVersion", Utf8ToUtf16(info.Version));

		constexpr uint16_t FixedFileInfoSize = 52;
		ResourceWriter writer;
		writer.Block(u"VS_VERSION_INFO", FixedFileInfoSize, false, [&] {
			// VS_FIXEDFILEINFO.
			writer.DWord(0xFEEF04BD);
			writer.DWord(0x00010000);
			writer.DWord(versionMS);
			writer.DWord(versionLS);
			writer.DWord(versionMS);
			writer.DWord(versionLS);
			writer.DWord(0x3F);       // VS_FFI_FILEFLAGSMASK
			writer.DWord(0);          // no flags (release build)
			writer.DWord(0x00040004); // VOS_NT_WINDOWS32
			writer.DWord(0x1);        // VFT_APP
			writer.DWord(0);
			writer.DWord(0);
			writer.DWord(0);
			writer.Block(u"StringFileInfo", 0, true, [&] {
				// U.S. English (0x0409), Unicode code page (1200 = 0x04B0).
				writer.Block(u"040904B0", 0, true, [&] {
					for (const auto& [key, value] : strings)
						writer.Block(key, static_cast<uint16_t>(value.size() + 1), true, [&] { writer.String(value); });
				});
			});
			writer.Block(u"VarFileInfo", 0, true, [&] {
				writer.Block(u"Translation", 4, false, [&] {
					writer.Word(0x0409);
					writer.Word(1200);
				});
			});
		});
		return writer.Take();
	}

	bool WriteWindowsResources(const std::filesystem::path& executable, const PackageInfo& info, const IconImage* icon, std::string& outError)
	{
#if defined(BS_PLATFORM_WINDOWS)
		std::optional<WindowsIconResources> built = icon ? BuildWindowsIcon(*icon) : WindowsIconResources{};
		if (!built)
		{
			outError = "cannot encode the icon for '" + FileSystem::PathToUtf8(executable) + "'";
			return false;
		}
		WindowsIconResources& iconResources = *built;
		std::vector<uint8_t> version = BuildVersionInfo(info);
		const WORD language = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
		// RT_ICON, RT_GROUP_ICON and RT_VERSION expand to narrow strings unless UNICODE is defined (Basalt does not
		// define it); the wide functions need wide integer resources.
		const LPCWSTR iconType = MAKEINTRESOURCEW(3);
		const LPCWSTR groupIconType = MAKEINTRESOURCEW(14);
		const LPCWSTR versionType = MAKEINTRESOURCEW(16);
		const auto describe = [](const char* step) {
			const DWORD code = GetLastError();
			char* message = nullptr;
			FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<char*>(&message), 0, nullptr);
			std::string text = fmt::format("{} failed (error {}{}{})", step, code, message ? ": " : "", message ? message : "");
			LocalFree(message);
			while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
				text.pop_back();
			return text;
		};
		// A virus scanner may still hold the freshly copied file: retry briefly before giving up.
		std::string reason;
		for (int attempt = 0; attempt < 5; ++attempt)
		{
			if (attempt > 0)
				Sleep(200);
			HANDLE update = BeginUpdateResourceW(executable.wstring().c_str(), FALSE);
			if (!update)
			{
				reason = describe("BeginUpdateResource");
				continue;
			}
			bool ok = true;
			for (size_t i = 0; ok && i < iconResources.Images.size(); ++i)
			{
				std::vector<uint8_t>& image = iconResources.Images[i];
				ok = UpdateResourceW(update, iconType, MAKEINTRESOURCEW(WindowsIconResources::FirstIconID + i), language, image.data(), static_cast<DWORD>(image.size()));
			}
			if (ok && icon)
				ok = UpdateResourceW(update, groupIconType, L"GLFW_ICON", language, iconResources.GroupDirectory.data(), static_cast<DWORD>(iconResources.GroupDirectory.size()));
			if (ok)
				ok = UpdateResourceW(update, versionType, MAKEINTRESOURCEW(1), language, version.data(), static_cast<DWORD>(version.size()));
			if (!ok)
			{
				reason = describe("UpdateResource");
				EndUpdateResourceW(update, TRUE);
				break;
			}
			if (EndUpdateResourceW(update, FALSE))
				return true;
			reason = describe("EndUpdateResource");
		}
		outError = "cannot write the icon and version resources of '" + FileSystem::PathToUtf8(executable) + "': " + reason;
		return false;
#else
		(void)info;
		(void)icon;
		outError = "cannot write the Windows resources of '" + FileSystem::PathToUtf8(executable) + "': only Windows can edit executable resources";
		return false;
#endif
	}

}
