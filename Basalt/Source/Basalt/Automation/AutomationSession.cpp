#include "Basalt/Automation/AutomationSession.h"

#include "Basalt/Automation/AutomationEvents.h"
#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Input.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/Scene/SceneEventListener.h"
#include "Basalt/Scripting/ScriptEngine.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>

namespace Basalt {

	const char* ScriptHotReloadName(ScriptHotReload state)
	{
		switch (state)
		{
			case ScriptHotReload::None:
				return "none";
			case ScriptHotReload::Applied:
				return "applied";
			case ScriptHotReload::NotPlaying:
				return "notPlaying";
			case ScriptHotReload::Simulating:
				return "simulating";
			case ScriptHotReload::Failed:
				return "failed";
		}
		return "none";
	}

	namespace {

		nlohmann::json EntityRef(Entity entity)
		{
			return { { "id", static_cast<uint64_t>(entity.GetUUID()) }, { "name", entity.GetName() } };
		}

		const char* ContactTypeName(ContactEventType type)
		{
			switch (type)
			{
				case ContactEventType::CollisionBegin:
					return "collisionBegin";
				case ContactEventType::CollisionEnd:
					return "collisionEnd";
				case ContactEventType::TriggerEnter:
					return "triggerEnter";
				case ContactEventType::TriggerExit:
					return "triggerExit";
			}
			return "unknown";
		}

	}

	// Turns the play scene's contacts and script errors into automation events.
	class AutomationSession::SceneEvents final : public SceneEventListener
	{
	public:
		explicit SceneEvents(AutomationEventHub& hub)
			: m_Hub(hub)
		{
		}

		void OnContact(Scene& scene, ContactEventType type, Entity first, Entity second, const std::optional<ContactInfo>& contact) override
		{
			if (!m_Hub.Wants(AutomationEventType::Collision))
				return;
			nlohmann::json event = { { "type", ContactTypeName(type) }, { "frame", scene.GetFrameCount() }, { "a", EntityRef(first) }, { "b", EntityRef(second) } };
			if (contact)
			{
				event["point"] = { contact->Point.x, contact->Point.y, contact->Point.z };
				event["normal"] = { contact->Normal.x, contact->Normal.y, contact->Normal.z };
				event["impulse"] = contact->Impulse;
			}
			m_Hub.Publish(AutomationEventType::Collision, std::move(event));
		}

		void OnAnimationEvent(Scene& scene, Entity entity, const std::string& name, const std::string& clip) override
		{
			if (m_Hub.Wants(AutomationEventType::Animation))
				m_Hub.Publish(AutomationEventType::Animation, { { "name", name }, { "clip", clip }, { "entity", EntityRef(entity) }, { "frame", scene.GetFrameCount() } });
		}

		void OnScriptError(Scene& scene, const std::string& message) override
		{
			if (m_Hub.Wants(AutomationEventType::ScriptError))
				m_Hub.Publish(AutomationEventType::ScriptError, { { "message", message }, { "frame", scene.GetFrameCount() } });
		}

	private:
		AutomationEventHub& m_Hub;
	};

	AutomationSession::AutomationSession()
		: m_Events(CreateRef<AutomationEventHub>())
		, m_SceneEvents(CreateScope<SceneEvents>(*m_Events))
	{
		SetEditScene(CreateRef<Scene>("Untitled"), "");
	}

	AutomationSession::~AutomationSession()
	{
		// A play copy reports to m_SceneEvents, which goes with the session; hosts may still hold the copy.
		if (m_Scene && m_Scene != m_EditScene)
			m_Scene->SetEventListener(nullptr);
	}

	AutomationSession::EventClientScope::EventClientScope(AutomationSession& session, EventClient client)
		: m_Session(session)
		, m_Previous(session.m_EventClient)
	{
		m_Session.m_EventClient = client;
	}

	AutomationSession::EventClientScope::~EventClientScope()
	{
		m_Session.m_EventClient = m_Previous;
	}

	bool AutomationSession::CanSetSnapshot(const std::string& name, std::string& outError) const
	{
		if (name.ends_with(".bscene"))
		{
			outError = "snapshot names cannot end in '.bscene' (scene.diff reads such names as scene files)";
			return false;
		}
		if (!m_Snapshots.contains(name) && m_Snapshots.size() >= MaxSnapshots)
		{
			outError = fmt::format("too many snapshots (at most {}); reuse a name", MaxSnapshots);
			return false;
		}
		return true;
	}

	bool AutomationSession::SetSnapshot(const std::string& name, nlohmann::json state, std::string& outError)
	{
		if (!CanSetSnapshot(name, outError))
			return false;
		m_Snapshots[name] = std::move(state);
		return true;
	}

	const nlohmann::json* AutomationSession::FindSnapshot(const std::string& name) const
	{
		auto it = m_Snapshots.find(name);
		return it == m_Snapshots.end() ? nullptr : &it->second;
	}

	void AutomationSession::PublishPlayState(PlayStateChange change)
	{
		if (m_Events->Wants(AutomationEventType::PlayState))
			m_Events->Publish(AutomationEventType::PlayState, { { "state", PlayStateChangeName(change) }, { "scene", m_Scene ? m_Scene->GetName() : std::string() } });
	}

	void AutomationSession::SetEditScene(const Ref<Scene>& scene, const std::string& scenePath)
	{
		StopPlay();
		m_EditScene = scene;
		m_Scene = scene;
		m_ScenePath = scenePath;
		m_Selection.clear();
		m_SelectionSet.clear();
		++m_EditRevision;
		m_Dirty = false;
		if (OnSceneChanged)
			OnSceneChanged();
	}

	void AutomationSession::SetSelection(UUID selection)
	{
		SetSelections(selection.IsValid() ? std::vector<UUID>{ selection } : std::vector<UUID>{});
	}

	void AutomationSession::SetSelections(const std::vector<UUID>& selection)
	{
		std::vector<UUID> valid;
		valid.reserve(selection.size());
		std::unordered_set<UUID> membership;
		membership.reserve(selection.size());
		for (UUID id : selection)
			if (m_Scene->GetEntityByUUID(id) && membership.insert(id).second)
				valid.push_back(id);
		m_Selection = std::move(valid);
		m_SelectionSet = std::move(membership);
	}

	bool AutomationSession::IsSelected(UUID entity) const
	{
		return m_SelectionSet.contains(entity);
	}

	void AutomationSession::PruneSelection()
	{
		std::erase_if(m_Selection, [this](UUID id) {
			if (m_Scene->GetEntityByUUID(id))
				return false;
			m_SelectionSet.erase(id);
			return true;
		});
	}

	bool AutomationSession::AttachCommandObserver(CommandObserver& observer)
	{
		if (m_CommandObserver)
			return false;
		m_CommandObserver = &observer;
		return true;
	}

	void AutomationSession::DetachCommandObserver(CommandObserver& observer)
	{
		if (m_CommandObserver == &observer)
			m_CommandObserver = nullptr;
	}

	void AutomationSession::SyncFileWatcher()
	{
		const Ref<Project>& project = Project::GetActive();
		if (project == m_WatchedProject.lock() && (project != nullptr) == m_FileWatcher.IsWatching())
			return;
		m_WatchedProject = project;
		m_FileWatcher.Watch(project ? project->GetDirectory() : std::filesystem::path(), Project::AssetDirectoryName);
	}

	std::vector<FileChange> AutomationSession::PollFileChanges()
	{
		SyncFileWatcher();
		return m_FileWatcher.Update();
	}

	std::vector<FileChange> AutomationSession::RefreshFileChanges(std::string& outError, std::string& outWarning)
	{
		SyncFileWatcher();
		std::vector<FileChange> changes = m_FileWatcher.Refresh();
		outError = m_FileWatcher.GetLastError();
		outWarning = m_FileWatcher.TakeLateBaselineNotice() ? "Assets/ could not be read until now: changes made before were not seen (pass 'paths' to reload them)" : "";
		return changes;
	}

	bool AutomationSession::IsKnownFile(const std::string& relativePath)
	{
		SyncFileWatcher();
		return m_FileWatcher.IsKnown(relativePath);
	}

	std::optional<FileChangeType> AutomationSession::AcknowledgeFile(const std::string& relativePath)
	{
		SyncFileWatcher();
		return m_FileWatcher.Acknowledge(relativePath);
	}

	AssetReloadReport AutomationSession::ReloadAssets(const std::vector<FileChange>& changes)
	{
		AssetReloadReport report;
		if (changes.empty())
			return report;
		std::vector<std::string> paths;
		std::vector<std::string> scripts;
		for (const FileChange& change : changes)
		{
			// AssetManager keys and script paths are normalized project-relative paths.
			const std::filesystem::path normal = FileSystem::PathFromUtf8(change.Path).lexically_normal();
			const std::string path = FileSystem::PathToGenericUtf8(normal);
			report.Changed.emplace_back(path, change.Type);
			paths.push_back(path);
			// A removed script has nothing to load: the running version stays until play stops.
			if (change.Type != FileChangeType::Removed && FileSystem::GetLowercaseExtension(normal) == ".lua")
				scripts.push_back(path);
		}
		report.Assets = AssetManager::ReloadChanged(paths);
		if (!scripts.empty())
		{
			ScriptEngine* engine = IsPlaying() && !IsSimulating() ? m_Scene->GetScriptEngine() : nullptr;
			if (engine)
			{
				report.Scripts = engine->ReloadScripts(scripts);
				report.HotReload = report.Scripts.Error.empty() ? ScriptHotReload::Applied : ScriptHotReload::Failed;
				if (IsRecording() && !report.Scripts.Reloaded.empty())
					m_ReloadedWhileRecording = true;
			}
			else if (IsSimulating())
				report.HotReload = ScriptHotReload::Simulating;
			else
				report.HotReload = ScriptHotReload::NotPlaying;
		}
		if (!report.Assets.empty() || report.HotReload == ScriptHotReload::Applied || report.HotReload == ScriptHotReload::Failed)
		{
			const std::string failures = report.Scripts.Failed.empty() ? std::string() : fmt::format("; {} script(s) did not load", report.Scripts.Failed.size());
			BS_CORE_INFO("Hot reload: {} changed file(s); reloaded {} asset(s) and {} script(s) ({} instance(s)){}", changes.size(), report.Assets.size(), report.Scripts.Reloaded.size(), report.Scripts.Instances, failures);
		}
		if (OnAssetsReloaded)
			OnAssetsReloaded(report);
		return report;
	}

	AssetReloadReport AutomationSession::OnFileWritten(const std::string& relativePath)
	{
		const std::optional<FileChangeType> change = AcknowledgeFile(relativePath);
		return ReloadAssets({ { relativePath, change.value_or(FileChangeType::Modified) } });
	}

	bool AutomationSession::StartPlay(std::string& outError, bool simulate)
	{
		if (m_CommandObserver && !m_CommandObserver->CanStartPlay(outError))
			return false;
		if (IsPlaying())
		{
			outError = "already playing";
			return false;
		}
		m_Scene = Scene::Copy(m_EditScene);
		// Before the systems start, so errors of the first OnCreate calls are reported too, after the play event.
		m_Scene->SetEventListener(m_SceneEvents.get());
		PublishPlayState(simulate ? PlayStateChange::Simulate : PlayStateChange::Play);
		if (simulate)
			m_Scene->OnSimulationStart();
		else
		{
			if (BeforeRuntimeStart)
				BeforeRuntimeStart();
			// A recording that began before play marks where play started, with the view the scripts start with, so
			// a replay applies what came before before it starts play (replay.run).
			if (IsRecording() && !m_RecordingSeed)
			{
				RecordViewChange();
				m_Recording.push_back({ { "frame", 0 }, { "command", "playStart" }, { "params", nlohmann::json::object() } });
			}
			m_Scene->OnRuntimeStart();
		}
		if (IsRecording() && !m_RecordingSeed)
			m_RecordingSeed = m_Scene->GetRandomSeed();
		return true;
	}

	void AutomationSession::StopPlay()
	{
		if (!IsPlaying())
			return;
		if (m_Scene->GetState() == SceneState::Simulate)
			m_Scene->OnSimulationStop();
		else
			m_Scene->OnRuntimeStop();
		// Hosts may keep the stopped copy alive a little longer (renderers); it must not report anymore.
		m_Scene->SetEventListener(nullptr);
		m_Scene = m_EditScene;
		PublishPlayState(PlayStateChange::Stop);
		PruneSelection();
		// Like held keys, injected gamepads belong to the play session.
		Input::Reset();
		Input::ForgetInjectedGamepads();
	}

	void AutomationSession::SetPaused(bool paused)
	{
		if (!IsPlaying() || m_Scene->IsPaused() == paused)
			return;
		m_Scene->SetPaused(paused);
		PublishPlayState(paused ? PlayStateChange::Pause : PlayStateChange::Resume);
	}

	bool AutomationSession::IsPlaying() const
	{
		return m_Scene && m_Scene != m_EditScene && m_Scene->IsRunning();
	}

	bool AutomationSession::IsSimulating() const
	{
		return IsPlaying() && m_Scene->GetState() == SceneState::Simulate;
	}

	void AutomationSession::StartRecording()
	{
		m_ReloadedWhileRecording = false;
		m_Recording = nlohmann::json::array();
		m_RecordingSeed = IsPlaying() ? std::optional<uint32_t>(m_Scene->GetRandomSeed()) : std::nullopt;
		m_RecordedView.reset();
		// The view first, so the mouse positions after it map the same way in the replay.
		RecordViewChange();
		// A replay starts from no input: record the mouse position and the injected gamepads already connected, with
		// what they hold.
		if (Input::HasMousePosition())
		{
			const glm::vec2 mouse = Input::GetMousePosition();
			RecordInput("input.mouse", { { "position", { mouse.x, mouse.y } } });
		}
		for (uint32_t index = 0; index < Input::MaxGamepads; index++)
		{
			if (Input::GetGamepadSource(index) != GamepadSource::Injected)
				continue;
			RecordInput("input.gamepad", { { "index", index }, { "connected", true }, { "name", Input::GetGamepadName(index) } });
			for (uint32_t b = 0; b < GamepadButtonCount; b++)
			{
				if (Input::IsGamepadButtonDown(index, static_cast<GamepadButton>(b)))
					RecordInput("input.gamepad", { { "index", index }, { "button", GamepadButtonToString(static_cast<GamepadButton>(b)) } });
			}
			for (uint32_t a = 0; a < GamepadAxisCount; a++)
			{
				if (const float value = Input::GetGamepadAxis(index, static_cast<GamepadAxis>(a)); value != 0.0f)
					RecordInput("input.gamepad", { { "index", index }, { "axis", GamepadAxisToString(static_cast<GamepadAxis>(a)) }, { "value", value } });
			}
		}
	}

	void AutomationSession::RecordInput(const std::string& command, const nlohmann::json& params)
	{
		if (!IsRecording())
			return;
		RecordViewChange();
		const uint64_t frame = IsPlaying() ? m_Scene->GetFrameCount() : 0;
		m_Recording.push_back({ { "frame", frame }, { "command", command }, { "params", params } });
	}

	void AutomationSession::RecordViewChange()
	{
		if (!IsRecording())
			return;
		const uint64_t frame = IsPlaying() ? m_Scene->GetFrameCount() : 0;
		if (const GameUI::HostState view = GameUI::GetHostState(); m_RecordedView != view)
		{
			m_RecordedView = view;
			nlohmann::json viewParams = {
				{ "origin", { view.ViewportOrigin.x, view.ViewportOrigin.y } },
				{ "size", { view.ViewportSize.x, view.ViewportSize.y } },
				{ "pointerEnabled", view.PointerEnabled },
				{ "navigationEnabled", view.NavigationEnabled },
			};
			if (view.Resolution)
				viewParams["resolution"] = { view.Resolution->x, view.Resolution->y };
			m_Recording.push_back({ { "frame", frame }, { "command", "view" }, { "params", viewParams } });
		}
	}

	nlohmann::json AutomationSession::StopRecording()
	{
		nlohmann::json events = IsRecording() ? std::move(m_Recording) : nlohmann::json::array();
		m_Recording = nullptr;
		m_RecordingSeed.reset();
		m_RecordedView.reset();
		m_ReloadedWhileRecording = false;
		return events;
	}

	void AutomationSession::Step(uint32_t frames, float frameTime)
	{
		if (!IsPlaying())
			return;
		for (uint32_t i = 0; i < frames; i++)
		{
			RecordViewChange();
			m_Scene->OnUpdate(frameTime);
			Input::EndFrame();
		}
	}

}
