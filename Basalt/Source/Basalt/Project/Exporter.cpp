#include "Basalt/Project/Exporter.h"

#include "Basalt/Asset/AssetRegistry.h"
#include "Basalt/Core/Base.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Core/Platform.h"
#include "Basalt/Core/Process.h"
#include "Basalt/Project/Packaging.h"
#include "Basalt/Project/Project.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/ranges.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace Basalt {

	namespace {

		std::string SanitizeFileName(const std::string& name)
		{
			std::string result;
			for (char c : name)
			{
				if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')
					result += c;
				else if (c == ' ')
					result += '_';
			}
			return result.empty() ? std::string("Game") : result;
		}

		bool IsInside(const std::filesystem::path& path, const std::filesystem::path& directory)
		{
			const std::filesystem::path relative = path.lexically_normal().lexically_relative(directory.lexically_normal());
			return !relative.empty() && *relative.begin() != "..";
		}

		uint32_t CountFiles(const std::filesystem::path& directory)
		{
			uint32_t count = 0;
			std::error_code error;
			for (auto it = std::filesystem::recursive_directory_iterator(directory, error); !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
			{
				if (it->is_regular_file(error))
					count++;
			}
			return count;
		}

		// Where a package keeps the project: Contents/Resources of a macOS bundle, the AppDir itself on Linux.
		std::filesystem::path PackagedProjectDirectory(const std::filesystem::path& package)
		{
			return package.extension() == ".app" ? package / "Contents" / "Resources" : package;
		}

		// The executable's file name on this platform.
		std::string ExecutableFileName(const std::string& executableName)
		{
#if defined(BS_PLATFORM_WINDOWS)
			return executableName + ".exe";
#else
			return executableName;
#endif
		}

		// A folder export of this game holds the project file and its executable; a package export holds this game's
		// package. Other games' exports do not count: a folder of downloaded games is not a previous export of this one.
		bool IsPreviousExport(const std::filesystem::path& output, const std::string& executableName)
		{
			if (FileSystem::Exists(output / Project::FileName) && FileSystem::Exists(output / ExecutableFileName(executableName)))
				return true;
			for (const char* extension : { ".app", ".AppDir" })
				if (FileSystem::Exists(PackagedProjectDirectory(output / (executableName + extension)) / Project::FileName))
					return true;
			return false;
		}

		struct ArchiveFile
		{
			std::string Key;
			std::filesystem::path Source;
			uint64_t AssetID = 0;
		};

		// What an archive export wrote.
		struct WrittenArchive
		{
			PackArchiveWriter::Statistics Statistics;
			// Sorted: the omitted original images.
			std::vector<std::string> Omitted;
		};

		constexpr std::string_view ReplaceableImageExtensions[] = { ".png", ".jpg", ".jpeg", ".tga", ".bmp" };
		// Files that cannot name another asset, besides those images.
		constexpr std::string_view OtherMediaExtensions[] = { ".hdr", ".ktx2", ".wav", ".ogg", ".mp3", ".flac", ".opus", ".ttf", ".otf", ".bin" };
		// Stored without trying zstd, which saves 0.1 to 5% of them and would stop a long clip being read in part
		// (PackArchive::ReadStored). The engine plays these, and WAV.
		constexpr std::string_view EncodedAudioExtensions[] = { ".mp3", ".flac" };

		bool IsListed(std::span<const std::string_view> extensions, std::string_view extension)
		{
			return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
		}

		std::optional<uint32_t> ParseHex(std::string_view text)
		{
			uint32_t value = 0;
			const auto [parsed, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
			return !text.empty() && error == std::errc() && parsed == text.data() + text.size() ? std::optional(value) : std::nullopt;
		}

		void AppendUtf8(std::string& out, uint32_t code)
		{
			if (code < 0x80)
				out += static_cast<char>(code);
			else if (code < 0x800)
				out.append({ static_cast<char>(0xC0 | (code >> 6)), static_cast<char>(0x80 | (code & 63)) });
			else if (code < 0x10000)
				out.append({ static_cast<char>(0xE0 | (code >> 12)), static_cast<char>(0x80 | ((code >> 6) & 63)), static_cast<char>(0x80 | (code & 63)) });
			else
				out.append({ static_cast<char>(0xF0 | (code >> 18)), static_cast<char>(0x80 | ((code >> 12) & 63)), static_cast<char>(0x80 | ((code >> 6) & 63)), static_cast<char>(0x80 | (code & 63)) });
		}

		// Undoes the backslash escape text begins with into out; the characters it took, or 0 when it is none. text is not
		// empty. The escapes of punctuation need none of this: comparisons skip punctuation (KeepNameCharacters).
		size_t DecodeStringEscape(std::string_view text, std::string& out)
		{
			const auto hexAt = [&](size_t at, size_t count) { return at + count <= text.size() ? ParseHex(text.substr(at, count)) : std::nullopt; };
			if (text[0] != '\\' || text.size() < 2)
				return 0;
			// Control characters, and Lua's \z with the spaces it skips, are no part of a name worth comparing.
			if (std::string_view("abfnrtvz").find(text[1]) != std::string_view::npos)
				return 2;
			if (text[1] == '\\')
			{
				// An escaped backslash, so what follows it is no escape.
				out += '\\';
				return 2;
			}
			if (text[1] == 'x')
			{
				const auto byte = hexAt(2, 2);
				if (byte)
					out += static_cast<char>(*byte);
				return byte ? 4 : 0;
			}
			if (text.starts_with("\\u{"))
			{
				// Lua allows any number of leading zeros, and a stray \u{ must not scan the rest of the file.
				constexpr size_t MaxBraceDigits = 32;
				const std::string_view braced = text.substr(3, MaxBraceDigits + 1);
				const size_t close = braced.find('}');
				const auto code = close != std::string_view::npos ? ParseHex(braced.substr(0, close)) : std::nullopt;
				if (!code || *code > 0x10FFFF)
					return 0;
				AppendUtf8(out, *code);
				return 3 + close + 1;
			}
			if (text[1] == 'u')
			{
				auto code = hexAt(2, 4);
				if (!code)
					return 0;
				size_t length = 6;
				if (*code >= 0xD800 && *code < 0xDC00 && text.substr(6, 2) == "\\u")
				{
					const auto low = hexAt(8, 4);
					if (low && *low >= 0xDC00 && *low < 0xE000)
					{
						code = 0x10000 + ((*code - 0xD800) << 10) + (*low - 0xDC00);
						length = 12;
					}
				}
				AppendUtf8(out, *code);
				return length;
			}
			const std::string_view digits = text.substr(1, 3);
			uint32_t byte = 0;
			const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), byte);
			if (error != std::errc() || byte > 255)
				return 0;
			out += static_cast<char>(byte);
			return 1 + static_cast<size_t>(end - digits.data());
		}

		// JSON's and Lua's backslash escapes undone. Anything else stays as it is, so it works on any file, binary ones too.
		std::string DecodeStringEscapes(std::string_view text)
		{
			std::string decoded;
			decoded.reserve(text.size());
			for (size_t i = 0; i < text.size();)
			{
				const size_t taken = text[i] == '\\' ? DecodeStringEscape(text.substr(i), decoded) : 0;
				if (taken == 0)
					decoded += text[i++];
				i += taken;
			}
			return decoded;
		}

		// A URI's %XX undone, in either case, however little the writer escaped.
		std::string DecodePercentEscapes(std::string_view text)
		{
			std::string decoded;
			decoded.reserve(text.size());
			for (size_t i = 0; i < text.size(); i++)
			{
				const auto byte = text[i] == '%' && i + 2 < text.size() ? ParseHex(text.substr(i + 1, 2)) : std::nullopt;
				decoded += byte ? static_cast<char>(*byte) : text[i];
				i += byte ? 2 : 0;
			}
			return decoded;
		}

		// What two spellings of a name share: its letters, digits and non-ASCII bytes. Punctuation goes because it is what
		// escapes and joins differ in (\' for ', a space for %20, "Wall" .. ".png").
		std::string KeepNameCharacters(std::string_view text)
		{
			std::string kept;
			kept.reserve(text.size());
			for (const char c : text)
				if (const auto byte = static_cast<unsigned char>(c); byte >= 0x80 || std::isalnum(byte) != 0)
					kept += c;
			return kept;
		}

		// The images that compressed replacements make unnecessary: an image with a .ktx2 of the same name beside it (the
		// usual asset.compress_texture output; KTX2 files do not record their source) whose file name and UUID neither the
		// project file nor any file but media contains. A search of the bytes rather than a walk of the declared asset fields,
		// because a script's string, a script property or a glTF URI can name a file too, and a loose one, ignoring punctuation
		// on each side after undoing escapes: dropping a file a game uses is the failure to avoid, and a false match only
		// keeps one.
		std::vector<std::string> FindReplacedOriginals(const Project& project, const std::vector<ArchiveFile>& files, std::vector<std::string>& outWarnings)
		{
			std::unordered_set<std::string> keys;
			for (const ArchiveFile& file : files)
				keys.insert(file.Key);
			// Key -> what would mention it. Ordered, so the result comes out sorted.
			std::map<std::string, std::vector<std::string>> candidates;
			for (const ArchiveFile& file : files)
			{
				const std::filesystem::path path = FileSystem::PathFromUtf8(file.Key);
				const std::string extension = FileSystem::GetLowercaseExtension(path);
				if (!IsListed(ReplaceableImageExtensions, extension) || !keys.contains(file.Key.substr(0, file.Key.size() - extension.size()) + ".ktx2"))
					continue;
				std::vector<std::string>& mentions = candidates[file.Key];
				mentions.push_back(KeepNameCharacters(FileSystem::PathToUtf8(path.filename())));
				if (file.AssetID != 0)
					mentions.push_back(std::to_string(file.AssetID));
			}
			if (candidates.empty())
				return {};

			std::vector<std::filesystem::path> searched = { project.GetProjectFilePath() };
			for (const ArchiveFile& file : files)
			{
				const std::string extension = FileSystem::GetLowercaseExtension(file.Source);
				if (!IsListed(ReplaceableImageExtensions, extension) && !IsListed(OtherMediaExtensions, extension))
					searched.push_back(file.Source);
			}
			for (const std::filesystem::path& file : searched)
			{
				std::string readError;
				const auto bytes = FileSystem::ReadBinaryFile(file, &readError);
				if (!bytes)
				{
					outWarnings.push_back("every original texture stays in the archive: cannot search '" + FileSystem::PathToUtf8(file) + "' for references to them (" + readError + ")");
					return {};
				}
				const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
				// As written (a name that holds %41 itself), without and with URI escapes, which may be JSON-escaped in turn.
				const std::string strings = DecodeStringEscapes(text);
				const std::string spellings[] = { KeepNameCharacters(text), KeepNameCharacters(strings), KeepNameCharacters(DecodePercentEscapes(strings)) };
				std::erase_if(candidates, [&](const auto& candidate) {
					return std::any_of(candidate.second.begin(), candidate.second.end(), [&](const std::string& mention) {
						return std::any_of(std::begin(spellings), std::end(spellings), [&](const std::string& spelling) { return spelling.find(mention) != std::string::npos; });
					});
				});
				if (candidates.empty())
					return {};
			}
			std::vector<std::string> replaced;
			replaced.reserve(candidates.size());
			for (const auto& [key, mentions] : candidates)
				replaced.push_back(key);
			return replaced;
		}

		// Tells whoever exports what is gone: a script that builds one of these names at run time will not find it.
		std::string DescribeOmitted(const std::vector<std::string>& omitted)
		{
			constexpr size_t MaxNamed = 5;
			std::string text = "left out of the archive, each replaced by the .ktx2 beside it (keepOriginals keeps them):";
			for (size_t i = 0; i < std::min(omitted.size(), MaxNamed); i++)
				text.append(" '").append(omitted[i]).append("'");
			if (omitted.size() > MaxNamed)
				text.append(" and ").append(std::to_string(omitted.size() - MaxNamed)).append(" more");
			return text;
		}

		// Packs the project's Assets into archive: every file except the .meta sidecars, whose UUIDs go into the index,
		// entries no key can name (reported in outWarnings), and (unless keepOriginals) the originals of compressed textures
		// nothing else mentions.
		// Assets whose sidecar is broken go in without a UUID, so references to them will not resolve: outWarnings says so
		// (project.validate reports them as errors, so only an export with validation off gets here).
		std::optional<WrittenArchive> WriteAssetArchive(const Project& project, const std::filesystem::path& archive, bool keepOriginals, std::vector<std::string>& outWarnings, std::string& outError)
		{
			// A fresh scan: the project's own registry may predate files written since it loaded.
			AssetRegistry registry(project.GetDirectory());
			if (!registry.Refresh(outError))
				return std::nullopt;
			for (const auto& [asset, problem] : registry.GetProblems())
			{
				std::string warning = "'" + asset + "' is archived without its UUID (";
				warning.append(asset).append(".meta: ").append(problem).append("), so references to it will not resolve");
				outWarnings.push_back(std::move(warning));
			}
			std::vector<ArchiveFile> files;
			std::set<std::string> skipped;
			const std::filesystem::path assets = project.GetDirectory() / Project::AssetDirectoryName;
			std::error_code error;
			for (auto it = std::filesystem::recursive_directory_iterator(assets, error); !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
			{
				if (FileSystem::SkipNonUtf8Entry(it, project.GetDirectory(), skipped))
					continue;
				const std::string key = FileSystem::PathToGenericUtf8(it->path().lexically_relative(project.GetDirectory()));
				const auto inspectFailed = [&]() {
					outError = "cannot inspect '" + key + "': " + FileSystem::DescribeError(error);
					return std::nullopt;
				};
				// The iterator does not enter linked folders (a folder export's copy does): unless the link's own name is one no key
				// can hold (skipped above), refuse rather than ship a game that silently lacks their files. Linked files are read
				// through, as the folder export copies them, and a link to nothing is refused rather than left out.
				const bool link = it->is_symlink(error);
				if (error)
					return inspectFailed();
				if (link)
				{
					std::error_code targetError;
					const std::filesystem::file_status target = std::filesystem::status(it->path(), targetError);
					if (target.type() == std::filesystem::file_type::directory)
					{
						outError = "'" + key + "' is a linked folder, which an archive cannot hold (replace the link with the folder)";
						return std::nullopt;
					}
					if (target.type() != std::filesystem::file_type::regular)
					{
						outError = "'" + key + "' is a broken link (it names no file)";
						return std::nullopt;
					}
				}
				const bool regular = it->is_regular_file(error);
				if (error)
					return inspectFailed();
				if (!regular || FileSystem::GetLowercaseExtension(it->path()) == ".meta")
					continue;
				if (!PackArchive::IsValidPath(key))
				{
					outError = "'" + key + "' cannot be stored in an archive (a name with ':' or '\\', or a path over " + std::to_string(PackArchive::MaxPathLength) + " bytes)";
					return std::nullopt;
				}
				const std::optional<UUID> id = registry.FindID(key);
				files.push_back({ key, it->path(), id ? static_cast<uint64_t>(*id) : 0 });
			}
			if (error)
			{
				outError = "cannot list '" + FileSystem::PathToUtf8(assets) + "': " + FileSystem::DescribeError(error);
				return std::nullopt;
			}
			// No scene, prefab or material can name them.
			for (const std::string& entry : skipped)
				outWarnings.push_back("'" + entry + "' is left out of the archive: " + FileSystem::NonUtf8NameProblem);
#if !defined(BS_PLATFORM_WINDOWS)
			// Here raw bytes still open such a file from a folder, so a game could depend on one without any document saying so.
			if (!skipped.empty())
				outWarnings.emplace_back("a Lua string or glTF URI that spells such a name in raw bytes or percent escapes works from a folder, but will not find the file in the archive");
#endif
			WrittenArchive written;
			if (!keepOriginals)
				written.Omitted = FindReplacedOriginals(project, files, outWarnings);
			if (!written.Omitted.empty())
				outWarnings.push_back(DescribeOmitted(written.Omitted));
			PackArchiveWriter writer;
			for (ArchiveFile& file : files)
			{
				if (std::binary_search(written.Omitted.begin(), written.Omitted.end(), file.Key))
					continue;
				const auto storage = IsListed(EncodedAudioExtensions, FileSystem::GetLowercaseExtension(file.Source)) ? PackArchiveWriter::Storage::Streamable : PackArchiveWriter::Storage::Smallest;
				if (!writer.AddFile(std::move(file.Key), std::move(file.Source), file.AssetID, outError, storage))
					return std::nullopt;
			}
			std::optional<PackArchiveWriter::Statistics> statistics = writer.Write(archive, outError);
			if (!statistics)
				return std::nullopt;
			written.Statistics = *statistics;
			return written;
		}

		// Copies the project file and its input actions into directory with its Assets (a folder, or with options.Archive an
		// Assets.bpak that outArchive reports), replacing a previous export's.
		bool CopyProjectFiles(const Project& project, const std::filesystem::path& directory, const ExportOptions& options, std::optional<WrittenArchive>& outArchive, std::vector<std::string>& outWarnings, std::string& outError)
		{
			std::error_code error;
			// Assets the project no longer has must not survive from the previous export, in either form: the runtime would
			// read an archive left beside a new Assets folder.
			for (const char* previous : { Project::AssetDirectoryName, Project::ArchiveFileName })
			{
				std::filesystem::remove_all(directory / previous, error);
				if (error)
				{
					outError = "cannot remove the previous export's '" + FileSystem::PathToUtf8(directory / previous) + "': " + FileSystem::DescribeError(error);
					return false;
				}
			}
			std::filesystem::create_directories(directory, error);
			if (error)
			{
				outError = "cannot create '" + FileSystem::PathToUtf8(directory) + "': " + FileSystem::DescribeError(error);
				return false;
			}
			if (options.Archive)
			{
				outArchive = WriteAssetArchive(project, directory / Project::ArchiveFileName, options.KeepOriginals, outWarnings, outError);
				if (!outArchive)
				{
					outError = "cannot write " + std::string(Project::ArchiveFileName) + ": " + outError;
					return false;
				}
			}
			else
				std::filesystem::copy(project.GetDirectory() / Project::AssetDirectoryName, directory / Project::AssetDirectoryName, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, error);
			if (!error)
				std::filesystem::copy_file(project.GetProjectFilePath(), directory / Project::FileName, std::filesystem::copy_options::overwrite_existing, error);
			if (error)
			{
				outError = "cannot copy project files: " + FileSystem::DescribeError(error);
				return false;
			}
			// The input actions are optional; a previous export's copy must not outlive the project's file. A file that
			// cannot even be checked fails the export rather than shipping the game without its controls.
			const std::filesystem::file_status input = std::filesystem::status(project.GetInputFilePath(), error);
			if (input.type() == std::filesystem::file_type::not_found)
			{
				error.clear();
				std::filesystem::remove(directory / Project::InputFileName, error);
			}
			else if (!error)
				std::filesystem::copy_file(project.GetInputFilePath(), directory / Project::InputFileName, std::filesystem::copy_options::overwrite_existing, error);
			if (error)
			{
				outError = "cannot copy '" + FileSystem::PathToUtf8(project.GetInputFilePath()) + "': " + FileSystem::DescribeError(error);
				return false;
			}
			return true;
		}

#if defined(BS_PLATFORM_MACOS)
		constexpr const char* LoaderFileName = "libvulkan.1.dylib";
		constexpr const char* MoltenVKFileName = "libMoltenVK.dylib";

		struct VulkanLibraries
		{
			std::filesystem::path Loader;
			std::filesystem::path MoltenVK;
		};

		// The Vulkan loader and MoltenVK to ship. The runtime's own directory comes first: the tools archive holds both
		// libraries next to the runtime, built for the same architectures, so an export from it needs no Vulkan SDK and
		// keeps them whatever VULKAN_SDK says (a build tree has none there). scripts/package_release.py, which packages
		// that archive, searches the other places in the same order.
		bool FindVulkanLibraries(const std::filesystem::path& runtimeDirectory, VulkanLibraries& outLibraries, std::string& outError)
		{
			std::vector<std::filesystem::path> searchDirectories{ runtimeDirectory };
			if (const auto sdk = Platform::GetEnvVar("VULKAN_SDK"))
				searchDirectories.emplace_back(FileSystem::PathFromUtf8(*sdk) / "lib");
			searchDirectories.emplace_back("/opt/homebrew/lib");
			searchDirectories.emplace_back("/usr/local/lib");
			auto find = [&](const char* fileName) -> std::filesystem::path {
				for (const auto& directory : searchDirectories)
				{
					if (FileSystem::Exists(directory / fileName))
						return directory / fileName;
				}
				return {};
			};
			outLibraries = { find(LoaderFileName), find(MoltenVKFileName) };
			if (outLibraries.Loader.empty() || outLibraries.MoltenVK.empty())
			{
				outError = "cannot find libvulkan.1.dylib and libMoltenVK.dylib next to the runtime or in the Vulkan SDK (install it, or Homebrew vulkan-loader + molten-vk)";
				return false;
			}
			return true;
		}

		// Copies the Vulkan loader and MoltenVK into libraryDirectory, and writes the ICD manifest that names MoltenVK into
		// <manifestBase>/vulkan/icd.d, where VulkanLoader looks for it, so the game runs on Macs without the Vulkan SDK.
		bool BundleMoltenVK(const VulkanLibraries& libraries, const std::filesystem::path& libraryDirectory, const std::filesystem::path& manifestBase, std::string& outError)
		{
			std::error_code error;
			std::filesystem::create_directories(libraryDirectory, error);
			if (error)
			{
				outError = "cannot create '" + FileSystem::PathToUtf8(libraryDirectory) + "': " + FileSystem::DescribeError(error);
				return false;
			}
			for (const auto& [source, name] : { std::pair{ &libraries.Loader, LoaderFileName }, std::pair{ &libraries.MoltenVK, MoltenVKFileName } })
			{
				// Package managers install versioned files behind symlinks; ship the file itself.
				const std::filesystem::path file = std::filesystem::canonical(*source, error);
				if (!error)
					std::filesystem::copy_file(file, libraryDirectory / name, std::filesystem::copy_options::overwrite_existing, error);
				// Package-manager files are often read-only; keep exports overwritable by later exports.
				if (!error)
					std::filesystem::permissions(libraryDirectory / name, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read | std::filesystem::perms::others_read, std::filesystem::perm_options::replace, error);
				if (error)
				{
					outError = "cannot copy '" + FileSystem::PathToUtf8(*source) + "' to '" + FileSystem::PathToUtf8(libraryDirectory / name) + "': " + FileSystem::DescribeError(error);
					return false;
				}
			}

			// The manifest names the library relative to the manifest file itself.
			const std::filesystem::path manifestDirectory = manifestBase / "vulkan" / "icd.d";
			const std::string library = FileSystem::PathToGenericUtf8((libraryDirectory / MoltenVKFileName).lexically_relative(manifestDirectory));
			const std::string manifest = R"({ "file_format_version": "1.0.0", "ICD": { "library_path": ")" + library + R"(", "api_version": "1.3.0", "is_portability_driver": true } })";
			if (!FileSystem::WriteTextFile(manifestDirectory / "MoltenVK_icd.json", manifest, outError))
			{
				outError = "cannot write the MoltenVK ICD manifest: " + outError;
				return false;
			}
			return true;
		}

		// Warns when a bundled library is Mach-O code that lacks an architecture the runtime has: the game would not start
		// on such Macs (a universal runtime with Homebrew's single-architecture libraries). A runtime or library that is no
		// Mach-O code at all (a test's stand-in, a damaged file) cannot be compared: that is only logged, since the export's
		// smoke test runs headless and never loads the libraries.
		void CheckLibraryArchitectures(const std::filesystem::path& runtime, const VulkanLibraries& libraries, std::vector<std::string>& outWarnings)
		{
			const auto architectures = [](const std::filesystem::path& path) {
				std::error_code error;
				const std::optional<uint64_t> size = FileSystem::GetRegularFileSize(path, error);
				const auto header = size ? FileSystem::ReadBinaryFilePrefix(path, std::min<uint64_t>(*size, MachOHeaderBytes)) : std::nullopt;
				std::vector<std::string> found = header ? MachOArchitectures(*header) : std::vector<std::string>{};
				if (found.empty())
					BS_CORE_WARN("Exporter: '{}' is no 64-bit Mach-O code, so its architectures cannot be checked", FileSystem::PathToUtf8(path));
				return found;
			};
			const std::vector<std::string> wanted = architectures(runtime);
			if (wanted.empty())
				return;
			for (const std::filesystem::path* library : { &libraries.Loader, &libraries.MoltenVK })
			{
				const std::vector<std::string> found = architectures(*library);
				if (found.empty() || std::includes(found.begin(), found.end(), wanted.begin(), wanted.end()))
					continue;
				outWarnings.push_back(fmt::format("'{}' holds {}, the runtime {}: the game will not start on Macs of the missing kind (use the libraries next to a release's runtime, or a universal Vulkan SDK)",
												  FileSystem::PathToUtf8(*library), fmt::join(found, ", "), fmt::join(wanted, ", ")));
			}
		}

		// Signs an export ad hoc: the Vulkan libraries BundleMoltenVK put in libraryDirectory first, then sealingCode, the
		// executable or the bundle that seals them. A release's runtime, once signed with Basalt's Developer ID, has the hardened runtime,
		// whose library validation refuses libraries signed by anyone else (the SDK's, Homebrew's), and would name Basalt's
		// publisher as the game's. An ad hoc signature names no one and checks no libraries; scripts/sign-macos.sh
		// replaces it with the developer's.
		bool SignAdHoc(const std::filesystem::path& libraryDirectory, const std::filesystem::path& sealingCode, std::string& outError)
		{
			for (const std::filesystem::path& path : { libraryDirectory / LoaderFileName, libraryDirectory / MoltenVKFileName, sealingCode })
			{
				ProcessSpec spec;
				// Part of macOS itself, not of the developer tools.
				spec.Executable = "/usr/bin/codesign";
				spec.Arguments = { "--force", "--sign", "-", FileSystem::PathToUtf8(path) };
				// A bundle's signature hashes every resource, Assets.bpak included, which takes a while for a large game.
				spec.Timeout = std::chrono::minutes(10);
				const ProcessResult run = RunProcess(spec);
				if (!run.Succeeded())
				{
					outError = "codesign could not sign '" + FileSystem::PathToUtf8(path) + "' (" + run.DescribeFailure() + "): " + run.Output;
					return false;
				}
			}
			return true;
		}

		// Info.plist, PkgInfo and the icon of a bundle whose executable and resources are already in place.
		bool FinishAppBundle(const std::filesystem::path& bundle, PackageInfo info, const std::optional<IconImage>& icon, std::string& outError)
		{
			const std::filesystem::path contents = bundle / "Contents";
			if (icon)
			{
				const auto icns = BuildIcns(*icon);
				if (!icns)
				{
					outError = "cannot encode the icon as .icns";
					return false;
				}
				info.IconFileName = info.ExecutableName + ".icns";
				if (!FileSystem::WriteBinaryFile(contents / "Resources" / info.IconFileName, icns->data(), icns->size(), outError))
					return false;
			}
			return FileSystem::WriteTextFile(contents / "Info.plist", BuildInfoPlist(info), outError) && FileSystem::WriteTextFile(contents / "PkgInfo", "APPL????", outError);
		}
#endif

#if defined(BS_PLATFORM_LINUX)
		std::filesystem::path FindAppImageTool(const std::filesystem::path& configured)
		{
			if (!configured.empty())
				return configured;
			if (const auto variable = Platform::GetEnvVar("APPIMAGETOOL"); variable && !variable->empty())
				return *variable;
			const std::string path = Platform::GetEnvVar("PATH").value_or("");
			for (size_t start = 0; start <= path.size();)
			{
				const size_t end = std::min(path.find(':', start), path.size());
				std::error_code error;
				const std::filesystem::path candidate = std::filesystem::path(path.substr(start, end - start)) / "appimagetool";
				if (end > start && std::filesystem::is_regular_file(candidate, error))
					return candidate;
				start = end + 1;
			}
			return {};
		}

		// Removes the AppImages an earlier export of this game wrote (<Name>-<version>-<arch>.AppImage, any version), so a
		// new AppImage never sits beside an outdated one.
		bool RemoveOldAppImages(const std::filesystem::path& output, const std::string& executableName, std::string& outError)
		{
			const std::string prefix = executableName + "-";
			std::error_code error;
			std::vector<std::filesystem::path> old;
			for (auto it = std::filesystem::directory_iterator(output, error); !error && it != std::filesystem::directory_iterator(); it.increment(error))
			{
				const std::string name = FileSystem::PathToUtf8(it->path().filename());
				if (!name.starts_with(prefix))
					continue;
				// What follows the name is "<version>-<arch>.AppImage". Sanitized names have no dots and a version has two or
				// three, so another game's name ("<Name>-Demo", "<Name>-2") never passes for one.
				const std::string rest = name.substr(prefix.size());
				const size_t dash = rest.find('-');
				if (dash != std::string::npos && ParseProjectVersion(std::string_view(rest).substr(0, dash)) && (rest.substr(dash + 1) == "x86_64.AppImage" || rest.substr(dash + 1) == "aarch64.AppImage"))
					old.push_back(it->path());
			}
			if (error)
			{
				outError = "cannot list '" + FileSystem::PathToUtf8(output) + "': " + FileSystem::DescribeError(error);
				return false;
			}
			for (const auto& path : old)
			{
				if (!std::filesystem::remove(path, error) && error)
				{
					outError = "cannot remove the earlier AppImage '" + FileSystem::PathToUtf8(path) + "': " + FileSystem::DescribeError(error);
					return false;
				}
				BS_CORE_INFO("Exporter: removed the earlier AppImage '{}'", FileSystem::PathToUtf8(path));
			}
			return true;
		}

		// The AppDir's desktop entry, icon and AppRun around an executable and project already in place, then the
		// .AppImage file when appimagetool is available. outPackage is the AppImage, or the AppDir without appimagetool.
		bool FinishAppImage(const std::filesystem::path& appDir, const std::filesystem::path& output, const PackageInfo& info, const std::optional<IconImage>& icon, const std::filesystem::path& configuredTool, std::filesystem::path& outPackage, ExportResult& result)
		{
			const std::string iconFile = info.ExecutableName + ".png";
			const std::vector<uint8_t> png = EncodeIconPng(icon ? ResizeIcon(*icon, std::min<uint32_t>(512, std::max(icon->Width, icon->Height))) : MakeDefaultIcon(256));
			if (png.empty())
			{
				result.Error = "cannot encode the AppImage icon";
				return false;
			}
			if (!FileSystem::WriteBinaryFile(appDir / iconFile, png.data(), png.size(), result.Error) || !FileSystem::WriteTextFile(appDir / (info.ExecutableName + ".desktop"), BuildDesktopEntry(info), result.Error))
				return false;
			std::vector<std::pair<std::string, std::string>> links = { { ".DirIcon", iconFile } };
			// A game named AppRun already is the entry point; a link would replace it with a link to itself.
			if (info.ExecutableName != "AppRun")
				links.emplace_back("AppRun", info.ExecutableName);
			for (const auto& [link, target] : links)
			{
				std::error_code error;
				std::filesystem::create_symlink(target, appDir / link, error);
				if (error)
				{
					result.Error = "cannot create '" + FileSystem::PathToUtf8(appDir / link) + "': " + FileSystem::DescribeError(error);
					return false;
				}
			}

			const std::filesystem::path tool = FindAppImageTool(configuredTool);
			if (tool.empty())
			{
				// Earlier AppImages stay: without a new one, the newest of them is still the last working package.
				result.Warnings.push_back("appimagetool was not found (set APPIMAGETOOL or add it to PATH): the package is the AppDir '" + FileSystem::PathToUtf8(appDir) + "'");
				outPackage = appDir;
				return true;
			}
	#if defined(__aarch64__)
			const char* architecture = "aarch64";
	#else
			const char* architecture = "x86_64";
	#endif
			if (!RemoveOldAppImages(output, info.ExecutableName, result.Error))
				return false;
			const std::filesystem::path appImage = output / (info.ExecutableName + "-" + info.Version + "-" + architecture + ".AppImage");
			ProcessSpec spec;
			spec.Executable = tool;
			spec.Arguments = { "--no-appstream", FileSystem::PathToUtf8(appDir), FileSystem::PathToUtf8(appImage) };
			// Without FUSE (containers, CI) an AppImage, including appimagetool itself, can still run extracted.
			spec.Environment = { { "ARCH", architecture }, { "APPIMAGE_EXTRACT_AND_RUN", "1" } };
			spec.Timeout = std::chrono::minutes(10);
			const ProcessResult run = RunProcess(spec);
			if (!run.Succeeded())
			{
				result.Error = "appimagetool failed (" + run.DescribeFailure() + "): " + run.Output;
				return false;
			}
			outPackage = appImage;
			return true;
		}
#endif

	}

	std::filesystem::path Exporter::GetDefaultRuntimePath()
	{
#if defined(BS_PLATFORM_WINDOWS)
		return FileSystem::GetExecutableDirectory() / "BasaltRuntime.exe";
#else
		return FileSystem::GetExecutableDirectory() / "BasaltRuntime";
#endif
	}

	std::filesystem::path Exporter::GetExportedProjectDirectory(const std::filesystem::path& executableDirectory)
	{
		std::filesystem::path resources = executableDirectory.parent_path() / "Resources";
		if (!FileSystem::Exists(executableDirectory / Project::FileName) && executableDirectory.filename() == "MacOS" && FileSystem::Exists(resources / Project::FileName))
			return resources;
		return executableDirectory;
	}

	bool Exporter::MountExportedArchive(const std::filesystem::path& projectDirectory, std::string& outError)
	{
		const std::filesystem::path file = projectDirectory / Project::ArchiveFileName;
		std::error_code error;
		const bool exists = std::filesystem::exists(file, error);
		if (!exists && !error)
			return true;
		const Ref<PackArchive> archive = PackArchive::Open(file, outError);
		if (!archive)
			return false;
		// Exports never leave both forms. A folder copied in later is shadowed wherever the archive has the same file, and its
		// sidecars are ignored (the asset registry takes UUIDs from the index only), so UUID references to its files fail.
		if (FileSystem::Exists(projectDirectory / Project::AssetDirectoryName))
			BS_CORE_WARN("Exporter: both {} and an Assets folder are in '{}'; files in the archive take precedence", Project::ArchiveFileName, FileSystem::PathToUtf8(projectDirectory));
		FileSystem::Mount(projectDirectory, archive);
		BS_CORE_INFO("Exporter: reading assets from '{}' ({} files)", FileSystem::PathToUtf8(file), archive->GetEntries().size());
		return true;
	}

	ExportResult Exporter::Export(const Project& project, const std::filesystem::path& outputDirectory, const ExportOptions& options)
	{
		ExportResult result;
		std::error_code error;

		if (options.KeepOriginals && !options.Archive)
		{
			result.Error = "keepOriginals needs archive: true (a folder export copies every file)";
			return result;
		}
		const ProjectConfig& config = project.GetConfig();
		if (config.StartScene.empty())
		{
			result.Error = "the project has no StartScene (set it with project.set)";
			return result;
		}
		if (!FileSystem::Exists(project.GetDirectory() / FileSystem::PathFromUtf8(config.StartScene)))
		{
			result.Error = "the start scene '" + config.StartScene + "' does not exist";
			return result;
		}

		// The runtime refuses a broken input actions file: say so now (the file may have been edited since the project
		// loaded) rather than ship a game that does not start.
		if (std::string inputError; !Project::ReadInputActions(project.GetInputFilePath(), inputError))
		{
			result.Error = inputError;
			return result;
		}

		const std::filesystem::path runtime = options.RuntimeExecutable.empty() ? GetDefaultRuntimePath() : options.RuntimeExecutable;
		if (!FileSystem::Exists(runtime))
		{
			result.Error = "runtime executable not found at '" + FileSystem::PathToUtf8(runtime) + "' (build the BasaltRuntime target)";
			return result;
		}

		const std::filesystem::path output = std::filesystem::absolute(outputDirectory, error);
		if (error || output.empty())
		{
			result.Error = "invalid output directory '" + FileSystem::PathToUtf8(outputDirectory) + "'";
			return result;
		}
		if (IsInside(output, project.GetDirectory()) || output.lexically_normal() == project.GetDirectory().lexically_normal())
		{
			result.Error = "the output directory must be outside the project directory";
			return result;
		}
		const std::string executableName = SanitizeFileName(config.Name);
		// Only ever replace a previous export, never an arbitrary non-empty directory.
		const bool outputExists = std::filesystem::exists(output, error);
		const bool outputEmpty = !error && (!outputExists || std::filesystem::is_empty(output, error));
		if (error)
		{
			result.Error = "cannot inspect the output directory '" + FileSystem::PathToUtf8(output) + "': " + FileSystem::DescribeError(error);
			return result;
		}
		if (!outputEmpty && !IsPreviousExport(output, executableName))
		{
			result.Error = "the output directory is not empty and is not a previous export of this game";
			return result;
		}

		// Everything that can be checked is read before anything is written, so a bad version, icon or missing library
		// leaves a previous export untouched.
		std::optional<PackageInfo> info;
		std::optional<IconImage> icon;
		if (options.Package)
		{
			info = MakePackageInfo(config, executableName, result.Error);
			if (!info || !LoadProjectIcon(project, icon, result.Error))
				return result;
#if defined(BS_PLATFORM_LINUX)
			// An AppImage must have an icon.
			const char* fallback = "the AppImage uses Basalt's default icon";
#else
			const char* fallback = "the package shows the platform's generic application icon";
#endif
			if (!icon)
				result.Warnings.push_back(std::string("the project has no icon: ") + fallback + " (set one with project.set icon)");
		}
#if defined(BS_PLATFORM_MACOS)
		VulkanLibraries vulkan;
		if (!FindVulkanLibraries(runtime.parent_path(), vulkan, result.Error))
			return result;
#endif

		// Folder exports (and Windows packages) keep everything in the output directory; macOS and Linux packages are a
		// directory of their own inside it, replaced whole on every export (a Linux AppImage file sits beside its AppDir).
		std::filesystem::path executableDirectory = output;
		std::filesystem::path package;
#if defined(BS_PLATFORM_MACOS)
		if (options.Package)
		{
			package = output / (executableName + ".app");
			executableDirectory = package / "Contents" / "MacOS";
		}
#elif defined(BS_PLATFORM_LINUX)
		if (options.Package)
		{
			package = output / (executableName + ".AppDir");
			executableDirectory = package;
		}
#endif
		const std::filesystem::path projectDirectory = package.empty() ? output : PackagedProjectDirectory(package);
		if (!package.empty())
		{
			std::filesystem::remove_all(package, error);
			if (error)
			{
				result.Error = "cannot replace '" + FileSystem::PathToUtf8(package) + "': " + FileSystem::DescribeError(error);
				return result;
			}
		}

		// From here on a failure leaves a partial package (the previous one is gone): say so.
		const auto fail = [&]() {
			if (!package.empty())
				result.Error += " (the incomplete package '" + FileSystem::PathToUtf8(package) + "' remains; export again once this is fixed)";
			else
				result.Error += " (the export in '" + FileSystem::PathToUtf8(output) + "' is incomplete; export again once this is fixed)";
			return result;
		};
		std::optional<WrittenArchive> archive;
		if (!CopyProjectFiles(project, projectDirectory, options, archive, result.Warnings, result.Error))
			return fail();

		std::filesystem::path executable = executableDirectory / ExecutableFileName(executableName);
		std::filesystem::create_directories(executableDirectory, error);
		// A run of the previous export that just exited can keep its executable locked briefly (antivirus);
		// CopyFileReplacing retries that.
		if (!error)
			FileSystem::CopyFileReplacing(runtime, executable, error);
		if (!error)
			std::filesystem::permissions(executable, std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec, std::filesystem::perm_options::add, error);
		if (error)
		{
			result.Error = "cannot copy the runtime: " + FileSystem::DescribeError(error);
			return fail();
		}

		std::filesystem::path packagePath = output;
#if defined(BS_PLATFORM_MACOS)
		if (options.Package)
		{
			const std::filesystem::path frameworks = package / "Contents" / "Frameworks";
			const bool bundled = BundleMoltenVK(vulkan, frameworks, projectDirectory, result.Error) && FinishAppBundle(package, *info, icon, result.Error);
			if (!bundled || !SignAdHoc(frameworks, package, result.Error))
				return fail();
			packagePath = package;
		}
		else if (!BundleMoltenVK(vulkan, output, output, result.Error) || !SignAdHoc(output, executable, result.Error))
			return fail();
		CheckLibraryArchitectures(executable, vulkan, result.Warnings);
#elif defined(BS_PLATFORM_WINDOWS)
		// Every export, folder or package: the game carries no signature but its developer's, and a package's resource
		// rewrite (WriteWindowsResources, next) would break one anyway.
		const std::optional<SignatureRemoval> removal = RemoveAuthenticodeSignature(executable, result.Error);
		if (!removal)
			return fail();
		// Not a Windows executable (a test's stand-in, or a damaged runtime): there is no signature to remove. Only the smoke
		// test can tell whether the game starts, so say so where it does not run.
		if (*removal == SignatureRemoval::NotExecutable)
			BS_CORE_WARN("Exporter: the runtime '{}' is not a Windows executable: the exported game will not start", FileSystem::PathToUtf8(runtime));
		if (options.Package && !WriteWindowsResources(executable, *info, icon ? &*icon : nullptr, result.Error))
			return fail();
#elif defined(BS_PLATFORM_LINUX)
		if (options.Package && !FinishAppImage(package, output, *info, icon, options.AppImageTool, packagePath, result))
			return fail();
#endif

		result.Success = true;
		if (archive)
		{
			result.ArchiveStatistics = archive->Statistics;
			result.ArchiveOmitted = std::move(archive->Omitted);
		}
		result.PackagePath = std::move(packagePath);
		result.ExecutablePath = std::move(executable);
		result.FileCount = CountFiles(output);
		BS_CORE_INFO("Exporter: exported '{}' to '{}' ({} files)", config.Name, FileSystem::PathToUtf8(result.PackagePath), result.FileCount);
		for (const std::string& warning : result.Warnings)
			BS_CORE_WARN("Exporter: {}", warning);
		return result;
	}

}
