#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Core/UUID.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <string>

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
		void MarkDirty() { m_Dirty = true; }
		void ClearDirty() { m_Dirty = false; }

		UUID GetSelection() const { return m_Selection; }
		void SetSelection(UUID selection) { m_Selection = selection; }

		// --- Play mode ------------------------------------------------------------------------------
		// Plays a copy of the edit scene (edits made while playing are discarded on stop). With simulate,
		// only physics runs (no scripts or audio).
		bool StartPlay(std::string& outError, bool simulate = false);
		void StopPlay();
		bool IsPlaying() const;
		bool IsSimulating() const;
		// Advances the play scene by whole fixed frames (deterministic).
		void Step(uint32_t frames, float frameTime);

		// --- Input recording (replays) ---------------------------------------------------------------
		// While recording, input commands are stored with the play frame they arrived on, so replaying
		// them on the same frames of a fresh play session reproduces the run.
		void StartRecording() { m_Recording = nlohmann::json::array(); }
		bool IsRecording() const { return m_Recording.is_array(); }
		void RecordInput(const std::string& command, const nlohmann::json& params);
		// Returns the recorded events and stops recording.
		nlohmann::json StopRecording();

		// --- Host capabilities ----------------------------------------------------------------------
		// Renders the current scene to a PNG. Unset in headless hosts.
		std::function<bool(const std::filesystem::path& path, uint32_t width, uint32_t height, std::string& outError)> Screenshot;
		// Called after the edit scene is replaced (editor resets panels, camera...).
		std::function<void()> OnSceneChanged;
		// Name of the host for diagnostics ("cli", "editor").
		std::string HostName = "cli";

	private:
		Ref<Scene> m_EditScene;
		// Equals m_EditScene while editing, the running copy while playing.
		Ref<Scene> m_Scene;
		std::string m_ScenePath;
		UUID m_Selection = 0;
		bool m_Dirty = false;
		// Array of { "frame", "command", "params" } while recording, null otherwise.
		nlohmann::json m_Recording;
	};

}
