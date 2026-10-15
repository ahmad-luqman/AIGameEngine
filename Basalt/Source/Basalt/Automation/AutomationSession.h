#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Core/UUID.h"
#include "Basalt/UI/GameUI.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	class Scene;

	// Editing state that automation commands operate on: the open scene, its file, the selection and the
	// play state. Hosts (CLI, editor) can plug in capabilities that need more than the engine core, such
	// as rendering a screenshot or the editor's own play mode.
	class AutomationSession
	{
	public:
		AutomationSession();

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

		// --- Input recording (replays) ---------------------------------------------------------------
		// While recording, input commands are stored with the play frame they arrived on, so replaying
		// them on the same frames of a fresh play session reproduces the run.
		void StartRecording();
		bool IsRecording() const { return m_Recording.is_array(); }
		// Records an input command on the current play frame. Before it, when the host's view (GameUI::HostState: where
		// the mouse maps onto the canvas, and whether widgets take input) differs from the last one recorded, a "view"
		// event records it, so a replay maps the recorded mouse positions the same way in any host.
		void RecordInput(const std::string& command, const nlohmann::json& params);
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
		// Name of the host for diagnostics ("cli", "editor").
		std::string HostName = "cli";
		// Hosts observe validated top-level commands, including requests dispatched through MCP.
		std::function<void(const std::string&, const nlohmann::json&)> BeforeCommand;
		std::function<void(const std::string&, const nlohmann::json&)> AfterCommand;

	private:
		Ref<Scene> m_EditScene;
		// Equals m_EditScene while editing, the running copy while playing.
		Ref<Scene> m_Scene;
		std::string m_ScenePath;
		std::vector<UUID> m_Selection;
		friend class CommandRegistry;
		size_t m_CommandDepth = 0;
		bool m_Dirty = false;
		uint64_t m_EditRevision = 0;
		// Array of { "frame", "command", "params" } while recording, null otherwise.
		nlohmann::json m_Recording;
		std::optional<uint32_t> m_RecordingSeed;
		// The host view the recording last recorded.
		std::optional<GameUI::HostState> m_RecordedView;
	};

}
