#include "Basalt/Core/FileWatcher.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Log.h"

#include <algorithm>
#include <system_error>

namespace Basalt {

	namespace {

		std::optional<FileWatcher::Stamp> ReadStamp(const std::filesystem::path& path)
		{
			std::error_code error;
			if (!std::filesystem::is_regular_file(path, error))
				return std::nullopt;
			const uintmax_t size = std::filesystem::file_size(path, error);
			if (error)
				return std::nullopt;
			const auto time = std::filesystem::last_write_time(path, error);
			if (error)
				return std::nullopt;
			return FileWatcher::Stamp{ size, time };
		}

		const FileWatcher::Stamp* Find(const FileWatcher::Snapshot& snapshot, const std::string& path)
		{
			const auto it = snapshot.find(path);
			return it != snapshot.end() ? &it->second : nullptr;
		}

		bool SameStamp(const FileWatcher::Stamp* a, const FileWatcher::Stamp* b)
		{
			return a && b ? *a == *b : a == b;
		}

		// The path is not there (a missing folder is simply not watched; a file removed while it was listed is absent).
		bool IsMissing(const std::error_code& error)
		{
			return error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory;
		}

	}

	const char* FileChangeTypeName(FileChangeType type)
	{
		switch (type)
		{
			case FileChangeType::Added:
				return "Added";
			case FileChangeType::Modified:
				return "Modified";
			case FileChangeType::Removed:
				return "Removed";
		}
		return "Modified";
	}

	FileWatcher::FileWatcher()
		: m_Owner(std::this_thread::get_id())
	{
	}

	FileWatcher::~FileWatcher()
	{
		StopWorker();
	}

	FileWatcher::Scan FileWatcher::ScanDirectory(const std::filesystem::path& root, const std::string& subdirectory)
	{
		Scan result;
		const std::filesystem::path base = FileSystem::PathFromUtf8(subdirectory);
		const std::filesystem::path folder = root / base;
		const auto fail = [&](const std::filesystem::path& path, const std::error_code& error) {
			result.Error = "cannot read '" + FileSystem::PathToUtf8(path) + "': " + error.message();
			return result;
		};
		std::error_code error;
		const std::filesystem::file_status status = std::filesystem::status(folder, error);
		if (error && !IsMissing(error))
			return fail(folder, error);
		if (!std::filesystem::is_directory(status))
			return result;
		// Unreadable folders are errors, not skipped: their files would look removed.
		for (auto it = std::filesystem::recursive_directory_iterator(folder, error); it != std::filesystem::recursive_directory_iterator(); it.increment(error))
		{
			if (error)
				break;
			std::error_code entryError;
			const bool regular = it->is_regular_file(entryError);
			uintmax_t size = 0;
			std::filesystem::file_time_type time;
			if (!entryError && regular)
				size = it->file_size(entryError);
			if (!entryError && regular)
				time = it->last_write_time(entryError);
			// A file removed between listing and reading is simply absent from this scan.
			if (entryError && IsMissing(entryError))
				continue;
			if (entryError)
				return fail(it->path(), entryError);
			if (regular)
				result.Files.emplace(FileSystem::PathToGenericUtf8(base / it->path().lexically_relative(folder)), Stamp{ size, time });
		}
		if (error)
			return fail(folder, error);
		return result;
	}

	void FileWatcher::Watch(const std::filesystem::path& root, const std::string& subdirectory)
	{
		BS_CORE_ASSERT(IsOwnerThread(), "FileWatcher is used from the thread that created it");
		StopWorker();
		m_Root = root;
		m_Subdirectory = subdirectory;
		m_Reported.clear();
		m_LastScan.clear();
		m_Acknowledged.clear();
		m_LastError.clear();
		m_BaselinePending = false;
		if (!IsWatching())
			return;
		const uint64_t sequence = BeginScan();
		const Scan baseline = ScanDirectory(m_Root, m_Subdirectory);
		m_AppliedSequence = sequence;
		if (!baseline.Error.empty())
		{
			BS_CORE_WARN("FileWatcher: {}; changes are reported once the folder can be read", baseline.Error);
			m_LastError = baseline.Error;
			m_BaselinePending = true;
		}
		else
		{
			m_Reported = baseline.Files;
			m_LastScan = baseline.Files;
		}
		if (IsPolling())
			StartWorker();
	}

	std::vector<FileChange> FileWatcher::Refresh()
	{
		if (!IsWatching())
			return {};
		const uint64_t sequence = BeginScan();
		return Apply(ScanDirectory(m_Root, m_Subdirectory), sequence, false);
	}

	bool FileWatcher::IsWatchedPath(const std::string& path) const
	{
		return m_Subdirectory.empty() || path.starts_with(m_Subdirectory + "/");
	}

	std::optional<FileChangeType> FileWatcher::Acknowledge(const std::string& relativePath)
	{
		BS_CORE_ASSERT(IsOwnerThread(), "FileWatcher is used from the thread that created it");
		const std::string path = FileSystem::PathToGenericUtf8(FileSystem::PathFromUtf8(relativePath).lexically_normal());
		// Files outside the watched folder are never scanned: recording them would report them as removed.
		if (!IsWatching() || !IsWatchedPath(path))
			return std::nullopt;
		const auto stamp = ReadStamp(m_Root / FileSystem::PathFromUtf8(path));
		const auto previous = m_Reported.find(path);
		std::optional<FileChangeType> change;
		if (stamp && previous == m_Reported.end())
			change = FileChangeType::Added;
		else if (!stamp && previous != m_Reported.end())
			change = FileChangeType::Removed;
		else if (stamp && !(previous->second == *stamp))
			change = FileChangeType::Modified;
		if (stamp)
		{
			m_Reported[path] = *stamp;
			m_LastScan[path] = *stamp;
		}
		else
		{
			m_Reported.erase(path);
			m_LastScan.erase(path);
		}
		std::lock_guard lock(m_Mutex);
		m_Acknowledged[path] = m_NextSequence;
		return change;
	}

	uint64_t FileWatcher::BeginScan()
	{
		std::lock_guard lock(m_Mutex);
		return m_NextSequence++;
	}

	std::vector<FileChange> FileWatcher::Apply(const Scan& scan, uint64_t sequence, bool settle)
	{
		BS_CORE_ASSERT(IsOwnerThread(), "FileWatcher is used from the thread that created it");
		if (!IsWatching())
			return {};
		if (!scan.Error.empty())
		{
			// Logged once per problem, not on every scan while it lasts.
			if (scan.Error != m_LastError)
				BS_CORE_WARN("FileWatcher: {}; changes are reported once the folder can be read again", scan.Error);
			m_LastError = scan.Error;
			return {};
		}
		if (!m_LastError.empty())
		{
			BS_CORE_INFO("FileWatcher: '{}' can be read again", FileSystem::PathToUtf8(m_Root / FileSystem::PathFromUtf8(m_Subdirectory)));
			m_LastError.clear();
		}
		// A background scan that started before the last Refresh knows less than what was already reported.
		if (sequence <= m_AppliedSequence)
			return {};
		m_AppliedSequence = sequence;
		std::erase_if(m_Acknowledged, [sequence](const auto& entry) { return entry.second <= sequence; });
		if (m_BaselinePending)
		{
			m_Reported = scan.Files;
			m_LastScan = scan.Files;
			m_BaselinePending = false;
			return {};
		}

		std::vector<FileChange> changes;
		Snapshot nextScan = scan.Files;
		const auto visit = [&](const std::string& path) {
			// The host acknowledged this file after the scan started: the scan may show it before that write.
			if (m_Acknowledged.contains(path))
			{
				if (const Stamp* previous = Find(m_LastScan, path))
					nextScan[path] = *previous;
				else
					nextScan.erase(path);
				return;
			}
			const Stamp* current = Find(scan.Files, path);
			const Stamp* reported = Find(m_Reported, path);
			if (SameStamp(current, reported))
				return;
			// Still changing since the previous scan: report it once it holds still.
			if (settle && !SameStamp(current, Find(m_LastScan, path)))
				return;
			if (!current)
			{
				changes.push_back({ path, FileChangeType::Removed });
				m_Reported.erase(path);
				return;
			}
			changes.push_back({ path, reported ? FileChangeType::Modified : FileChangeType::Added });
			m_Reported[path] = *current;
		};
		for (const auto& [path, stamp] : scan.Files)
			visit(path);
		std::vector<std::string> gone;
		for (const auto& [path, stamp] : m_Reported)
		{
			if (!scan.Files.contains(path))
				gone.push_back(path);
		}
		for (const std::string& path : gone)
			visit(path);
		m_LastScan = std::move(nextScan);
		std::sort(changes.begin(), changes.end(), [](const FileChange& a, const FileChange& b) { return a.Path < b.Path; });
		return changes;
	}

	bool FileWatcher::StartPolling(std::chrono::milliseconds interval)
	{
		BS_CORE_ASSERT(IsOwnerThread(), "FileWatcher is used from the thread that created it");
		StopWorker();
		m_RequestedInterval = std::max(interval, std::chrono::milliseconds(1));
		if (!IsWatching())
			return true;
		return StartWorker();
	}

	void FileWatcher::StopPolling()
	{
		m_RequestedInterval = std::chrono::milliseconds(0);
		StopWorker();
	}

	bool FileWatcher::StartWorker()
	{
		{
			std::lock_guard lock(m_Mutex);
			m_Stop = false;
			m_Pending.reset();
		}
		try
		{
			m_Worker = std::thread(&FileWatcher::RunWorker, this, m_Root, m_Subdirectory, m_RequestedInterval);
		}
		catch (const std::system_error& e)
		{
			BS_CORE_ERROR("FileWatcher: cannot start the polling thread ({}); changed files are only found by a refresh", e.what());
			m_RequestedInterval = std::chrono::milliseconds(0);
			return false;
		}
		return true;
	}

	void FileWatcher::StopWorker()
	{
		if (!m_Worker.joinable())
			return;
		{
			std::lock_guard lock(m_Mutex);
			m_Stop = true;
		}
		m_Wake.notify_all();
		m_Worker.join();
		std::lock_guard lock(m_Mutex);
		m_Pending.reset();
	}

	std::vector<FileChange> FileWatcher::Update()
	{
		std::optional<std::pair<Scan, uint64_t>> pending;
		{
			std::lock_guard lock(m_Mutex);
			pending.swap(m_Pending);
		}
		if (!pending)
			return {};
		return Apply(pending->first, pending->second, true);
	}

	void FileWatcher::RunWorker(const std::filesystem::path& root, const std::string& subdirectory, std::chrono::milliseconds interval)
	{
		// Only the scan runs here: no logging, no project access (Apply reports errors on the owner's thread).
		std::unique_lock lock(m_Mutex);
		while (!m_Wake.wait_for(lock, interval, [this]() { return m_Stop; }))
		{
			const uint64_t sequence = m_NextSequence++;
			lock.unlock();
			Scan scan;
			try
			{
				scan = ScanDirectory(root, subdirectory);
			}
			catch (const std::exception& e)
			{
				scan = Scan{ {}, std::string("scan failed: ") + e.what() };
			}
			lock.lock();
			// An unread older scan is replaced: the newest one knows everything it did.
			m_Pending.emplace(std::move(scan), sequence);
		}
	}

}
