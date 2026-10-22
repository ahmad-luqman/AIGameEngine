#pragma once

#include "Basalt/Automation/AutomationEvents.h"
#include "Basalt/Core/Base.h"
#include "Basalt/Core/FileWatcher.h"
#include "Basalt/Core/UUID.h"
#include "Basalt/Scripting/ScriptReload.h"
#include "Basalt/UI/GameUI.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <unordered_set>

namespace Basalt {

	class Project;
	class Scene;
	struct CommandInfo;

	// Whether changed scripts were hot reloaded.
	enum class ScriptHotReload : uint8_t
	{
		// No added or modified script among the changes.
		None,
		// The play session processed the changed scripts: Scripts says what was reloaded (possibly nothing, when no
		// running script uses them) and what failed.
		Applied,
		// Scripts reload only while the game runs.
		NotPlaying,
		// Simulate runs physics without scripts.
		Simulating,
		// A Lua error stopped the reload part way (Scripts.Error): some classes may be replaced, others not.
		Failed
	};

	// "none", "applied", "notPlaying", "simulating" or "failed".
	const char* ScriptHotReloadName(ScriptHotReload state);

	// What AutomationSession::ReloadAssets did with a set of changed files.
	struct AssetReloadReport
	{
		// The changes, with normalized paths (AssetManager keys).
		std::vector<FileChange> Changed;
		// AssetManager keys dropped; they load again from disk on their next use.
		std::vector<std::string> Assets;
		ScriptHotReload HotReload = ScriptHotReload::None;
		// The hot reload of changed scripts (empty unless HotReload is Applied).
		ScriptReloadResult Scripts;
		// Prefab instances that merged changed prefab files (edit mode), and those that could not.
		size_t PrefabInstances = 0;
		std::vector<std::string> PrefabErrors;
	};

	// One host owns command observation. Rejected BeforeCommand calls never reach the handler;
	// AfterCommand runs after successful or throwing handlers, with nesting depth already restored.
	class CommandObserver
	{
	public:
		virtual ~CommandObserver() = default;
		virtual void BeforeCommand(const CommandInfo& command, const nlohmann::json& params) = 0;
		virtual void AfterCommand(const CommandInfo& command, const nlohmann::json& params) noexcept = 0;
		virtual bool CanStartPlay(std::string& error) = 0;
	};

	// Editing state that automation commands operate on: the open scene, its file, the selection and the
	// play state. Hosts (CLI, editor) can plug in capabilities that need more than the engine core, such
	// as rendering a screenshot or the editor's own play mode.
	class AutomationSession
	{
	public:
		static constexpr size_t MaxSnapshots = 32;

		AutomationSession();
		~AutomationSession();
		AutomationSession(const AutomationSession&) = delete;
		AutomationSession& operator=(const AutomationSession&) = delete;

		const Ref<Scene>& GetScene() const { return m_Scene; }
		// The scene being edited (not the play-mode copy).
		const Ref<Scene>& GetEditScene() const { return m_EditScene; }
		void SetEditScene(const Ref<Scene>& scene, const std::string& scenePath);
		const std::string& GetScenePath() const { return m_ScenePath; }
		void SetScenePath(const std::string& path) { m_ScenePath = path; }

		// Prefab editing swaps in an isolated scene. The scene that waits keeps its path, unsaved state and
		// selection, and saving the prefab merges it into the waiting scene's instances.
		bool OpenPrefab(const std::string& path, std::string& outError);
		bool SavePrefab(std::string& outError, AssetReloadReport& outReport);
		bool ClosePrefab(bool discard, std::string& outError);
		// The open prefab's project-relative path; empty unless a prefab is being edited.
		const std::string& GetPrefabPath() const { return m_PrefabPath; }
		bool IsEditingPrefab() const { return !m_PrefabPath.empty(); }
		// Whether the scene waiting for prefab editing to end has unsaved edits.
		bool IsWaitingSceneDirty() const { return m_PrefabReturnDirty; }
		// True while a command handler runs; hosts use it to tell command edits from file-watcher edits.
		bool IsExecutingCommand() const { return m_CommandDepth > 0; }

		bool IsDirty() const { return m_Dirty; }
		void MarkDirty()
		{
			m_Dirty = true;
			++m_EditRevision;
		}
		// Counts authored edits even when already dirty; hosts can avoid capturing history for read-only calls.
		uint64_t GetEditRevision() const { return m_EditRevision; }
		void ClearDirty() { m_Dirty = false; }

		UUID GetSelection() const { return m_Selection.empty() ? UUID(0) : m_Selection.back(); }
		const std::vector<UUID>& GetSelections() const { return m_Selection; }
		void SetSelection(UUID selection);
		void SetSelections(const std::vector<UUID>& selection);
		bool IsSelected(UUID entity) const;
		void PruneSelection();

		// --- Play mode ------------------------------------------------------------------------------
		// Plays a copy of the edit scene (edits made while playing are discarded on stop). With simulate,
		// only physics runs (no scripts or audio).
		bool StartPlay(std::string& outError, bool simulate = false);
		// Also releases every key and button and forgets injected gamepads (they belong to the play session).
		void StopPlay();
		bool IsPlaying() const;
		bool IsSimulating() const;
		// Advances the play scene by whole fixed frames (deterministic).
		void Step(uint32_t frames, float frameTime);
		// Pauses or resumes the play scene (the editor's Pause button); no-op when not playing or unchanged.
		void SetPaused(bool paused);

		// --- Asset changes (hot reload) ---------------------------------------------------------------
		// The session watches the active project's Assets/ folder with a FileWatcher. Hosts poll it through the
		// functions below (the editor in the background; agents through asset.refresh); those that scan or record a file
		// first follow the active project (SyncFileWatcher).
		// Follows the active project: watches the Assets/ folder of a project that became active since the last call
		// (what is there now is the baseline), or nothing without one.
		void SyncFileWatcher();
		// Background scanning (FileWatcher::StartPolling): false (logged) when it cannot start.
		bool StartFileWatchPolling(std::chrono::milliseconds interval) { return m_FileWatcher.StartPolling(interval); }
		void StopFileWatchPolling() { m_FileWatcher.StopPolling(); }
		bool IsFileWatchPolling() const { return m_FileWatcher.IsPolling(); }
		// The settled changes the background scans found since the last call.
		std::vector<FileChange> PollFileChanges();
		// Scans now: every change since changes were last reported (by a refresh, by polling, or since the project was
		// opened, or since the folder could first be read). outError is set (and nothing reported) when the folder cannot
		// be read; outWarning when the baseline was only taken now (earlier changes were not seen).
		std::vector<FileChange> RefreshFileChanges(std::string& outError, std::string& outWarning);
		// A file the host wrote or removed itself: not reported as a change later.
		std::optional<FileChangeType> AcknowledgeFile(const std::string& relativePath);
		// The file is one the watcher knows (it existed at the last report).
		bool IsKnownFile(const std::string& relativePath);
		// Applies changed files: AssetManager drops what they affect (models, textures and what reads them), and while
		// the game runs (Play, not Simulate) added or modified scripts are hot reloaded. Logs what it did and calls
		// OnAssetsReloaded.
		AssetReloadReport ReloadAssets(const std::vector<FileChange>& changes);
		// Drains sidecars created by this host before the watcher can report them as external changes.
		void AcknowledgeAssetMetadata();
		// A file the session wrote itself (asset.write, scene.save, ...): the watcher records it, so it is not reported
		// as a change later, and it is reloaded now.
		AssetReloadReport OnFileWritten(const std::string& relativePath);
		// Scripts were hot reloaded during the current recording: a replay runs the final code from the start, so it may
		// not reproduce the run (replay.record_stop warns). False when not recording.
		bool HasReloadedScriptsWhileRecording() const { return m_ReloadedWhileRecording; }

		// --- Events and snapshots --------------------------------------------------------------------
		// Log lines, script errors, play-state changes and collisions of this session (events.subscribe). Created by
		// the session and shared with the connections that drain it from the server thread.
		const Ref<AutomationEventHub>& GetEvents() const { return m_Events; }

		// The client a request is handled for: the subscriptions it creates end with it (Owner, 0 for none: the CLI,
		// `basalt serve` and stdio MCP end with the process), and stream to it when it reads unsolicited frames.
		struct EventClient
		{
			uint64_t Owner = 0;
			// Streamed needs an Owner (its connection drains the events).
			AutomationEventDelivery Delivery = AutomationEventDelivery::Polled;
		};
		const EventClient& GetEventClient() const { return m_EventClient; }
		// Attributes the requests handled during its lifetime to a client (AutomationConnection), then restores
		// the previous one.
		class [[nodiscard]] EventClientScope
		{
		public:
			EventClientScope(AutomationSession& session, EventClient client);
			~EventClientScope();
			EventClientScope(const EventClientScope&) = delete;
			EventClientScope& operator=(const EventClientScope&) = delete;

		private:
			AutomationSession& m_Session;
			EventClient m_Previous;
		};

		// Named scene states (scene.snapshot) that scene.diff compares; they survive play stop and scene changes.
		// False (with outError) for a name ending in .bscene (scene.diff reads those as files) or when MaxSnapshots
		// other names are stored. Pointers FindSnapshot returns stay valid for the session's lifetime; replacing a
		// name changes what they point to.
		bool SetSnapshot(const std::string& name, nlohmann::json state, std::string& outError);
		// SetSnapshot's checks without a state, so callers can fail before serializing a scene.
		bool CanSetSnapshot(const std::string& name, std::string& outError) const;
		const nlohmann::json* FindSnapshot(const std::string& name) const;

		// --- Input recording (replays) ---------------------------------------------------------------
		// While recording, input commands are stored with the play frame they arrived on, so replaying
		// them on the same frames of a fresh play session reproduces the run.
		void StartRecording();
		bool IsRecording() const { return m_Recording.is_array(); }
		// Records an input command on the current play frame, after RecordViewChange.
		void RecordInput(const std::string& command, const nlohmann::json& params);
		// While recording, records the host's view (GameUI::HostState: where the mouse maps onto the canvas and the game
		// resolution, and whether the game has the pointer and the keyboard) as a "view" event when it differs from the
		// last one recorded, so a replay maps mouse positions and routes input the same way in any host. Hosts call it
		// before every scene update (Step does), as the routing can change between recorded commands.
		void RecordViewChange();
		// Seed of the first play session the recording saw (Scene::GetRandomSeed), if any: replays must run
		// with it, whatever the project's seed was changed to since.
		std::optional<uint32_t> GetRecordingSeed() const { return m_RecordingSeed; }
		// Returns the recorded events and stops recording.
		nlohmann::json StopRecording();

		// --- Host capabilities ----------------------------------------------------------------------
		// Renders the current scene to a PNG. Unset in headless hosts.
		std::function<bool(const std::filesystem::path& path, uint32_t width, uint32_t height, std::string& outError)> Screenshot;
		// Statistics of the host's scene renderer (render.stats): resolution, draw counts, GPU pass times.
		// Unset in headless hosts.
		std::function<bool(nlohmann::json& outStats, std::string& outError)> RenderStats;
		// Called after the edit scene is replaced (editor resets panels, camera...).
		std::function<void()> OnSceneChanged;
		// Called when Play starts, before the scripts' OnCreate, so the host can give the game its view first (the
		// editor's Game view geometry, resolution and routing). It must not start or stop play. replay.run clears it
		// while it runs the recorded view.
		std::function<void()> BeforeRuntimeStart;
		// Called after ReloadAssets applied changes (the editor refreshes previews).
		std::function<void(const AssetReloadReport&)> OnAssetsReloaded;
		// Called after asset.move/asset.rename moved a file (its old and new project-relative paths). The move rewrites
		// references on disk and in the open scene; hosts holding paths elsewhere (the editor's Material Editor) follow it.
		std::function<void(const std::string& from, const std::string& to)> OnAssetMoved;
		// Name of the host for diagnostics ("cli", "editor").
		std::string HostName = "cli";
		// Only the attached owner may detach itself; attaching a second observer is rejected.
		bool AttachCommandObserver(CommandObserver& observer);
		void DetachCommandObserver(CommandObserver& observer);

	private:
		// Prefab editing: the open prefab, its file contents when opened or last saved (to detect external edits),
		// and the scene waiting for it to close.
		std::string m_PrefabPath;
		std::string m_PrefabFileText;
		Ref<Scene> m_PrefabReturnScene;
		std::string m_PrefabReturnPath;
		std::vector<UUID> m_PrefabReturnSelection;
		bool m_PrefabReturnDirty = false;

		// Merges changed prefab files into the instances of the edit scene and of the scene waiting for prefab editing.
		void RefreshPrefabInstances(const std::vector<FileChange>& changes, AssetReloadReport& report);

		class SceneEvents;
		void PublishPlayState(PlayStateChange change);

		// Declared before the scenes, so the session's own scene references go first; ~AutomationSession detaches the
		// listener from a play copy someone else may still hold.
		Ref<AutomationEventHub> m_Events;
		Scope<SceneEvents> m_SceneEvents;
		EventClient m_EventClient;
		std::map<std::string, nlohmann::json> m_Snapshots;
		Ref<Scene> m_EditScene;
		// Equals m_EditScene while editing, the running copy while playing.
		Ref<Scene> m_Scene;
		std::string m_ScenePath;
		std::vector<UUID> m_Selection;
		std::unordered_set<UUID> m_SelectionSet;
		CommandObserver* m_CommandObserver = nullptr;
		friend class CommandRegistry;
		// Nested implementation commands keep the outer command's undo label and capture boundary.
		size_t m_CommandDepth = 0;
		bool m_Dirty = false;
		uint64_t m_EditRevision = 0;
		// Array of { "frame", "command", "params" } while recording, null otherwise.
		nlohmann::json m_Recording;
		std::optional<uint32_t> m_RecordingSeed;
		// The host view the recording last recorded.
		std::optional<GameUI::HostState> m_RecordedView;
		bool m_ReloadedWhileRecording = false;
		FileWatcher m_FileWatcher;
		// The project m_FileWatcher watches. weak_ptr, not a raw pointer: a new project allocated where a freed one was
		// must not look like the watched one (an expired pointer locks to null).
		std::weak_ptr<Project> m_WatchedProject;
	};

}
