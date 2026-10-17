#include "Basalt/Core/FileWatcher.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Log.h"

#include <algorithm>

namespace Basalt {

	namespace {

		std::string ToKey(const std::filesystem::path& relative)
		{
			const std::u8string text = relative.generic_u8string();
			return std::string(text.begin(), text.end());
		}

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
			return FileWatcher::Stamp{ size, std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count() };
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

	FileWatcher::~FileWatcher()
	{
		StopPolling();
	}

	FileWatcher::Scan FileWatcher::ScanDirectory(const std::filesystem::path& root, const std::string& subdirectory)
	{
		Scan result;
		const std::filesystem::path base = FileSystem::PathFromUtf8(subdirectory);
		const std::filesystem::path folder = root / base;
		std::error_code error;
		if (!std::filesystem::is_directory(folder, error))
			return result;
		const auto options = std::filesystem::directory_options::skip_permission_denied;
		for (auto it = std::filesystem::recursive_directory_iterator(folder, options, error); !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
		{
			// A file removed between listing and reading is simply absent from this scan.
			std::error_code entryError;
			if (!it->is_regular_file(entryError))
				continue;
			const uintmax_t size = it->file_size(entryError);
			if (entryError)
				continue;
			const auto time = it->last_write_time(entryError);
			if (entryError)
				continue;
			result.Files.emplace(ToKey(base / it->path().lexically_relative(folder)), Stamp{ size, std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count() });
		}
		if (error)
			result.Error = "cannot read '" + FileSystem::PathToUtf8(folder) + "': " + error.message();
		return result;
	}

	void FileWatcher::Watch(const std::filesystem::path& root, const std::string& subdirectory)
	{
		const std::chrono::milliseconds interval = m_Interval;
		StopPolling();
		m_Root = root;
		m_Subdirectory = subdirectory;
		m_Reported.clear();
		m_LastScan.clear();
		m_Acknowledged.clear();
		m_LastError.clear();
		if (!IsWatching())
			return;
		const uint64_t sequence = BeginScan();
		const Scan baseline = ScanDirectory(m_Root, m_Subdirectory);
		if (!baseline.Error.empty())
			BS_CORE_WARN("FileWatcher: {}", baseline.Error);
		m_Reported = baseline.Files;
		m_LastScan = baseline.Files;
		m_AppliedSequence = sequence;
		if (interval.count() > 0)
			StartPolling(interval);
	}

	std::vector<FileChange> FileWatcher::Refresh()
	{
		if (!IsWatching())
			return {};
		const uint64_t sequence = BeginScan();
		return Apply(ScanDirectory(m_Root, m_Subdirectory), sequence, false);
	}

	void FileWatcher::Acknowledge(const std::string& relativePath)
	{
		const std::string path = ToKey(FileSystem::PathFromUtf8(relativePath).lexically_normal());
		// Files outside the watched folder are never scanned: recording them would report them as removed.
		if (!IsWatching() || !path.starts_with(m_Subdirectory + "/"))
			return;
		if (const auto stamp = ReadStamp(m_Root / FileSystem::PathFromUtf8(path)))
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
	}

	uint64_t FileWatcher::BeginScan()
	{
		std::lock_guard lock(m_Mutex);
		return m_NextSequence++;
	}

	std::vector<FileChange> FileWatcher::Apply(const Scan& scan, uint64_t sequence, bool settle)
	{
		if (!scan.Error.empty())
		{
			// Logged once per problem, not on every scan while it lasts.
			if (scan.Error != m_LastError)
				BS_CORE_WARN("FileWatcher: {}", scan.Error);
			m_LastError = scan.Error;
			return {};
		}
		m_LastError.clear();
		// A background scan that started before the last Refresh knows less than what was already reported.
		if (sequence <= m_AppliedSequence)
			return {};
		m_AppliedSequence = sequence;

		std::vector<FileChange> changes;
		Snapshot nextScan = scan.Files;
		const auto visit = [&](const std::string& path) {
			// The host acknowledged this file after the scan started: the scan may show it before that write.
			if (const auto it = m_Acknowledged.find(path); it != m_Acknowledged.end() && sequence < it->second)
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
		std::erase_if(m_Acknowledged, [sequence](const auto& entry) { return entry.second <= sequence; });
		std::sort(changes.begin(), changes.end(), [](const FileChange& a, const FileChange& b) { return a.Path < b.Path; });
		return changes;
	}

	void FileWatcher::StartPolling(std::chrono::milliseconds interval)
	{
		StopPolling();
		m_Interval = std::max(interval, std::chrono::milliseconds(1));
		if (!IsWatching())
			return;
		{
			std::lock_guard lock(m_Mutex);
			m_Stop = false;
			m_Pending.reset();
		}
		m_Worker = std::thread(&FileWatcher::RunWorker, this, m_Root, m_Subdirectory, m_Interval);
	}

	void FileWatcher::StopPolling()
	{
		m_Interval = std::chrono::milliseconds(0);
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
		// Only the scan runs here: no logging, no project access (Apply reports errors on the main thread).
		std::unique_lock lock(m_Mutex);
		while (!m_Wake.wait_for(lock, interval, [this]() { return m_Stop; }))
		{
			const uint64_t sequence = m_NextSequence++;
			lock.unlock();
			Scan scan = ScanDirectory(root, subdirectory);
			lock.lock();
			// An unread older scan is replaced: the newest one knows everything it did.
			m_Pending.emplace(std::move(scan), sequence);
		}
	}

}
