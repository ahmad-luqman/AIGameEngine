#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Basalt {

	enum class FileChangeType : uint8_t
	{
		Added,
		Modified,
		Removed
	};

	// "Added", "Modified" or "Removed".
	const char* FileChangeTypeName(FileChangeType type);

	struct FileChange
	{
		// Relative to the watched root, '/'-separated UTF-8 ("Assets/Scripts/Player.lua": an AssetManager key).
		std::string Path;
		FileChangeType Type = FileChangeType::Modified;

		bool operator==(const FileChange&) const = default;
	};

	// Reports the files that were added, modified or removed under one folder of a directory (a project's Assets/).
	// It polls: each scan compares every file's size and modification time with the previous one, which works the same
	// on every platform and file system without native notification APIs (a write that keeps both the size and the
	// modification time, possible only on file systems with coarse timestamps, goes unnoticed).
	//
	// Refresh() scans on the calling thread. StartPolling() scans on a worker thread instead, and Update() (main
	// thread) reports what the newest scan found, once it has settled: a file whose stamp changed since the previous
	// scan may still be being written, so it is reported when a later scan finds it unchanged. Changes the host made
	// itself are passed to Acknowledge() and never reported. Every member function is for the owning (main) thread;
	// the worker only scans the folder it was given and hands the result over.
	class FileWatcher
	{
	public:
		struct Stamp
		{
			uintmax_t Size = 0;
			// Nanoseconds of the file clock.
			int64_t WriteTime = 0;

			bool operator==(const Stamp&) const = default;
		};
		// Watched files by relative path (FileChange::Path).
		using Snapshot = std::map<std::string, Stamp>;
		struct Scan
		{
			Snapshot Files;
			// Set when the folder could not be read completely; such a scan reports nothing (a file it missed is not
			// removed).
			std::string Error;
		};

		FileWatcher() = default;
		~FileWatcher();
		FileWatcher(const FileWatcher&) = delete;
		FileWatcher& operator=(const FileWatcher&) = delete;

		// Every regular file under root/subdirectory. A missing subdirectory is an empty scan, not an error.
		static Scan ScanDirectory(const std::filesystem::path& root, const std::string& subdirectory);

		// Watches root/subdirectory from now on: what exists now is the baseline (not reported). Keeps polling if it
		// was. An empty root watches nothing.
		void Watch(const std::filesystem::path& root, const std::string& subdirectory = "Assets");
		const std::filesystem::path& GetRoot() const { return m_Root; }
		bool IsWatching() const { return !m_Root.empty(); }

		// Scans now and reports every change since the last report, including those still settling.
		std::vector<FileChange> Refresh();
		// Records the current state of a file the host just wrote or removed (relative path), so neither this nor a
		// scan that started before it reports that change.
		void Acknowledge(const std::string& relativePath);

		// Scans every `interval` on a worker thread until StopPolling (or Watch with an empty root).
		void StartPolling(std::chrono::milliseconds interval);
		void StopPolling();
		// Polling was asked for (the worker runs while a folder is watched).
		bool IsPolling() const { return m_Interval.count() > 0; }
		// The settled changes of the newest background scan since the last call (none when no scan finished).
		std::vector<FileChange> Update();

		// Applies a scan as Update and Refresh do (`sequence` orders scans: an older one than the last applied, or than
		// an acknowledgement of a path, is ignored for it). With `settle`, changes still in progress are held back.
		// Public for tests, which feed scans without a worker.
		std::vector<FileChange> Apply(const Scan& scan, uint64_t sequence, bool settle);
		// The sequence number the next scan gets (tests pass it to Apply).
		uint64_t BeginScan();

	private:
		// std::thread keeps its own copies of root and subdirectory, which these refer to.
		void RunWorker(const std::filesystem::path& root, const std::string& subdirectory, std::chrono::milliseconds interval);

		std::filesystem::path m_Root;
		std::string m_Subdirectory;
		// What the host has been told about (or the baseline), and what the previous applied scan saw.
		Snapshot m_Reported;
		Snapshot m_LastScan;
		uint64_t m_AppliedSequence = 0;
		// Path -> the first scan sequence that started after its acknowledgement.
		std::unordered_map<std::string, uint64_t> m_Acknowledged;
		std::string m_LastError;

		// Shared with the worker.
		std::mutex m_Mutex;
		std::condition_variable m_Wake;
		bool m_Stop = false;
		uint64_t m_NextSequence = 1;
		std::optional<std::pair<Scan, uint64_t>> m_Pending;
		std::chrono::milliseconds m_Interval{ 0 };
		std::thread m_Worker;
	};

}
