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
#include <utility>
#include <vector>

namespace Basalt {

	// How a watched file changed.
	enum class FileChangeType : uint8_t
	{
		Added,
		Modified,
		Removed
	};

	// "Added", "Modified" or "Removed".
	const char* FileChangeTypeName(FileChangeType type);

	// One file a FileWatcher found added, modified or removed.
	struct FileChange
	{
		// Both required: a change without a type would silently read as Added.
		FileChange(std::string path, FileChangeType type)
			: Path(std::move(path))
			, Type(type)
		{
		}

		// Relative to the watched root, '/'-separated UTF-8 ("Assets/Scripts/Player.lua": an AssetManager key).
		std::string Path;
		FileChangeType Type;

		bool operator==(const FileChange&) const = default;
	};

	// Reports the files that were added, modified or removed under one folder of a directory (a project's Assets/).
	// It polls: each scan compares every file's size and modification time with the previous one, which works the same
	// on every platform and file system without native notification APIs. A write that keeps both the size and the
	// modification time goes unnoticed (a copy that preserves times, or a same-size write within a coarse timestamp's
	// resolution).
	//
	// Refresh() scans on the calling thread. StartPolling() scans on a worker thread instead, and Update() reports what
	// the newest scan found, once it has settled: a file whose stamp changed since the previous scan may still be being
	// written, so it is reported when a later scan finds it unchanged. A scan that could not read the whole folder
	// reports nothing (its missing files are not removals); GetLastError says why until a scan succeeds again. Changes
	// the host made itself are passed to Acknowledge() and never reported. Every member function is for the thread that
	// created the watcher; the worker only scans the folder it was given and hands the result over.
	class FileWatcher
	{
	public:
		struct Stamp
		{
			uintmax_t Size = 0;
			std::filesystem::file_time_type WriteTime{};

			bool operator==(const Stamp&) const = default;
		};
		// Watched files by relative path (FileChange::Path).
		using Snapshot = std::map<std::string, Stamp>;
		struct Scan
		{
			Snapshot Files;
			// Set when the folder could not be read completely (Files is then partial).
			std::string Error;
		};

		FileWatcher();
		~FileWatcher();
		FileWatcher(const FileWatcher&) = delete;
		FileWatcher& operator=(const FileWatcher&) = delete;

		// Every regular file under root/subdirectory. A missing subdirectory (or a file in its place) is an empty scan,
		// not an error; one that cannot be read, or holds a file or folder that cannot be read, is an error.
		static Scan ScanDirectory(const std::filesystem::path& root, const std::string& subdirectory);

		// Watches root/subdirectory from now on: what exists now is the baseline (not reported; when it cannot be read,
		// the first complete scan is, and TakeLateBaselineNotice says so once). Polling, if asked for, continues on the
		// new folder. An empty root watches nothing.
		void Watch(const std::filesystem::path& root, const std::string& subdirectory = "Assets");
		const std::filesystem::path& GetRoot() const { return m_Root; }
		bool IsWatching() const { return !m_Root.empty(); }

		// Scans now and reports every change since the last report, including those still settling.
		std::vector<FileChange> Refresh();
		// Records the current state of a file the host just wrote or removed (relative to the root), so no later scan,
		// and no scan already running, reports that change. Returns how it changed since the last report: nullopt when
		// it did not, when nothing is watched or the baseline is still pending, or when the path is outside the watched
		// folder (never recorded).
		std::optional<FileChangeType> Acknowledge(const std::string& relativePath);
		// The file (relative to the root) is in what was last reported, or the baseline.
		bool IsKnown(const std::string& relativePath) const;
		// Why the newest scan could not read the folder (empty when it could).
		const std::string& GetLastError() const { return m_LastError; }
		// True once after the baseline was taken late (Watch could not read the folder): changes made before it were not
		// seen.
		bool TakeLateBaselineNotice();

		// Asks for a scan every `interval` on a worker thread, which runs while a folder is watched, until StopPolling.
		// When the thread cannot be started (now, or later by Watch) that is logged and the request dropped: false here,
		// IsPolling false afterwards.
		bool StartPolling(std::chrono::milliseconds interval);
		void StopPolling();
		// Polling was asked for.
		bool IsPolling() const { return m_RequestedInterval.count() > 0; }
		// The settled changes of the newest background scan since the last call (none when no scan finished).
		std::vector<FileChange> Update();

	private:
		friend struct FileWatcherTestAccess;

		// Takes the next scan sequence number; call it just before scanning.
		uint64_t BeginScan();
		// Applies a scan as Update and Refresh do. A scan not newer than the last applied one is ignored entirely (its
		// error too), and a scan that began before a path's acknowledgement is ignored for that path. With `settle`,
		// changes still in progress are held back.
		std::vector<FileChange> Apply(const Scan& scan, uint64_t sequence, bool settle);
		bool StartWorker();
		void StopWorker();
		// std::thread keeps its own copies of root and subdirectory, which these refer to.
		void RunWorker(const std::filesystem::path& root, const std::string& subdirectory, std::chrono::milliseconds interval);
		bool IsWatchedPath(const std::string& path) const;
		void AssertOwnerThread() const;

		std::thread::id m_Owner;
		std::filesystem::path m_Root;
		std::string m_Subdirectory;
		// What the host has been told about (or the baseline), and what the previous applied scan saw.
		Snapshot m_Reported;
		Snapshot m_LastScan;
		// The baseline could not be read: the next complete scan becomes it (and m_LateBaseline is set).
		bool m_BaselinePending = false;
		bool m_LateBaseline = false;
		uint64_t m_AppliedSequence = 0;
		// Path -> the first scan sequence that started after its acknowledgement.
		std::unordered_map<std::string, uint64_t> m_Acknowledged;
		std::string m_LastError;
		std::chrono::milliseconds m_RequestedInterval{ 0 };

		// Shared with the worker.
		std::mutex m_Mutex;
		std::condition_variable m_Wake;
		bool m_Stop = false;
		uint64_t m_NextSequence = 1;
		std::optional<std::pair<Scan, uint64_t>> m_Pending;
		std::thread m_Worker;
	};

}
