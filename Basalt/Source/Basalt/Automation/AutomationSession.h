#pragma once

#include "Basalt/Automation/AutomationEvents.h"
#include "Basalt/Core/Base.h"
#include "Basalt/Core/FileWatcher.h"
#include "Basalt/Core/UUID.h"
#include "Basalt/Scripting/ScriptReload.h"
#include "Basalt/UI/GameUI.h"

#include <nlohmann/json.hpp>

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

	// What AutomationSession::ReloadAssets did with a set of changed files.
	struct AssetReloadReport
	{
		std::vector<FileChange> Changed;
		// AssetManager keys dropped; they load again from disk on their next use.
		std::vector<std::string> Assets;
		// The hot reload of changed scripts (empty unless the game was running).
		ScriptReloadResult Scripts;
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
		// Watches the active project's Assets/ folder. The host polls it (the editor in the background; agents through
		// asset.refresh).
		FileWatcher& GetFileWatcher() { return m_FileWatcher; }
		// Follows the active project: watches the Assets/ folder of a project that became active since the last call
		// (what is there now is the baseline), or nothing without one.
		void SyncFileWatcher();
		// Applies changed files: AssetManager drops what they affect (models, textures and what reads them), and while
		// the game runs (Play, not Simulate) added or modified scripts are hot reloaded. Logs what it did and calls
		// OnAssetsReloaded.
		AssetReloadReport ReloadAssets(const std::vector<FileChange>& changes);
		// A file the session wrote itself (asset.write, scene.save, ...): the watcher records it, so it is not reported
		// as a change later, and it is reloaded now.
		AssetReloadReport OnFileWritten(const std::string& relativePath);

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
		// Name of the host for diagnostics ("cli", "editor").
		std::string HostName = "cli";
		// Only the attached owner may detach itself; attaching a second observer is rejected.
		bool AttachCommandObserver(CommandObserver& observer);
		void DetachCommandObserver(CommandObserver& observer);

	private:
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
		FileWatcher m_FileWatcher;
		// The project m_FileWatcher watches (weak: another project at the same address is still another project).
		std::weak_ptr<Project> m_WatchedProject;
	};

}
