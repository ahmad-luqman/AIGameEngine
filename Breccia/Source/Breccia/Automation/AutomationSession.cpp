#include "Breccia/Automation/AutomationSession.h"

#include "Breccia/Automation/AutomationEvents.h"
#include "Breccia/Asset/AssetManager.h"
#include "Breccia/Core/FileSystem.h"
#include "Breccia/Core/Input.h"
#include "Breccia/Core/Log.h"
#include "Breccia/Network/EnetTransport.h"
#include "Breccia/Network/NetworkSystem.h"
#include "Breccia/Network/SimulatedNetwork.h"
#include "Breccia/Project/Project.h"
#include "Breccia/Scene/Entity.h"
#include "Breccia/Scene/Scene.h"
#include "Breccia/Scene/Prefab.h"
#include "Breccia/Scene/SceneSerializer.h"
#include "Breccia/Scene/SceneEventListener.h"
#include "Breccia/Scripting/ScriptEngine.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>

namespace Breccia {

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

		void OnAnimationEvent(Scene& scene, Entity entity, const std::string& name, const std::string& clip, const std::string& layer) override
		{
			if (m_Hub.Wants(AutomationEventType::Animation))
				m_Hub.Publish(AutomationEventType::Animation, { { "name", name }, { "clip", clip }, { "layer", layer.empty() ? nlohmann::json() : nlohmann::json(layer) }, { "entity", EntityRef(entity) }, { "frame", scene.GetFrameCount() } });
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

	const std::string& AutomationSession::GetPrefabPath() const
	{
		static const std::string s_None;
		return m_PrefabEditing ? m_PrefabEditing->Path : s_None;
	}

	bool AutomationSession::OpenPrefab(const std::string& prefabPath, std::string& outError, std::vector<std::string>& outWarnings)
	{
		const std::string path = Prefab::NormalizePath(prefabPath);
		if (IsPlaying())
		{
			outError = "stop Play or Simulate before editing a prefab";
			return false;
		}
		if (IsEditingPrefab())
		{
			outError = "close '" + GetPrefabPath() + "' (prefab.close) before opening another prefab";
			return false;
		}
		const auto absolute = Project::ResolvePath(path);
		// The text read here is what a later save compares the file with, to detect external edits.
		const auto text = FileSystem::ReadTextFile(absolute);
		if (!text)
		{
			outError = "cannot read prefab '" + path + "'";
			return false;
		}
		nlohmann::json entities;
		Prefab::ReadReport report;
		if (!Prefab::ReadSource(absolute, entities, outError, &report))
			return false;
		auto scene = CreateRef<Scene>(FileSystem::PathToUtf8(FileSystem::PathFromUtf8(path).stem()));
		if (!SceneSerializer::DeserializeScene(*scene, SceneSerializer::SceneDocument(std::move(entities)), outError))
			return false;
		// In here nested instances are instances like a scene's: they list overrides and take their prefabs' edits.
		outWarnings = std::move(report.Warnings);
		Prefab::TrackNestedInstances(*scene, outWarnings);
		for (const std::string& warning : outWarnings)
			BR_CORE_WARN("Prefab: {}", warning);
		m_PrefabEditing = PrefabEditing{ path, text, m_EditScene, m_ScenePath, m_Selection, m_Dirty };
		SetEditScene(scene, "");
		return true;
	}

	bool AutomationSession::SavePrefab(bool force, std::string& outError, AssetReloadReport& outReport, std::vector<std::string>& outWarnings)
	{
		if (!m_PrefabEditing)
		{
			outError = "no prefab is open (prefab.open)";
			return false;
		}
		PrefabEditing& editing = *m_PrefabEditing;
		if (IsPlaying())
		{
			outError = "stop Play or Simulate before saving the prefab";
			return false;
		}
		const auto absolute = Project::ResolvePath(editing.Path);
		if (const auto onDisk = FileSystem::ReadTextFile(absolute); !force && (!onDisk || !editing.FileText || *onDisk != *editing.FileText))
		{
			const char* what = !onDisk ? "was deleted or cannot be read" : "changed on disk since it was opened or last saved";
			outError = fmt::format("'{}' {}; save with force=true (Overwrite Prefab File in the editor) to write it anyway, or close with discard=true and open it again", editing.Path, what);
			return false;
		}
		const auto roots = m_EditScene->GetRootEntities();
		if (roots.size() != 1)
		{
			outError = "a prefab needs exactly one root entity, found " + std::to_string(roots.size());
			return false;
		}
		const PrefabFileRoot rootKind = Prefab::FileRootOf(roots.front());
		// What a nested instance has not merged yet would otherwise save as overrides of what its prefab says now.
		bool merged = false;
		const bool current = Prefab::MergeOutdatedNested(roots.front(), editing.Path, rootKind, merged, outWarnings, outError);
		if (merged)
			MarkDirty();
		if (!current || !Prefab::SaveTree(roots.front(), absolute, rootKind, outWarnings, outError))
			return false;
		ClearDirty();
		// The file is written; failing to read it back only loses the conflict check's reference, so the next save needs
		// force.
		editing.FileText = FileSystem::ReadTextFile(absolute);
		if (!editing.FileText)
		{
			outWarnings.push_back("'" + editing.Path + "' was saved but could not be read back; the next save needs force=true");
			BR_CORE_WARN("Prefab: {}", outWarnings.back());
		}
		outReport = OnFileWritten(editing.Path);
		return true;
	}

	bool AutomationSession::ClosePrefab(bool discard, std::string& outError)
	{
		if (!m_PrefabEditing)
		{
			outError = "no prefab is open";
			return false;
		}
		PrefabEditing& open = *m_PrefabEditing;
		if (IsPlaying())
		{
			outError = "stop Play or Simulate before closing the prefab";
			return false;
		}
		if (IsDirty() && !discard)
		{
			outError = "'" + open.Path + "' has unsaved edits: prefab.save, or close with discard=true";
			return false;
		}
		// Editing ends before the scene changes, so OnSceneChanged observers already see the waiting scene's state.
		PrefabEditing editing = std::move(open);
		m_PrefabEditing.reset();
		SetEditScene(editing.WaitingScene, editing.WaitingPath);
		SetSelections(editing.WaitingSelection);
		m_Dirty = editing.WaitingDirty;
		return true;
	}

	void AutomationSession::MergePrefabInstances(const std::optional<std::string>& changedPrefab, AssetReloadReport& report)
	{
		const std::optional<std::string> changedFile = changedPrefab ? std::optional(Prefab::FileKeyOf(*changedPrefab)) : std::nullopt;
		const auto addWarnings = [&](const std::vector<std::string>& warnings) {
			for (const std::string& warning : warnings)
				if (std::find(report.PrefabWarnings.begin(), report.PrefabWarnings.end(), warning) == report.PrefabWarnings.end())
				{
					report.PrefabWarnings.push_back(warning);
					BR_CORE_WARN("Prefab: {}", warning);
				}
		};
		Prefab::SourceCache sources;
		// The edit scenes only: a play copy is discarded when play stops, and its edit scene merges meanwhile.
		const Ref<Scene> waiting = m_PrefabEditing ? m_PrefabEditing->WaitingScene : nullptr;
		for (const Ref<Scene>& scene : { m_EditScene, waiting })
		{
			if (!scene)
				continue;
			std::vector<std::pair<UUID, std::string>> roots;
			for (auto [handle, link] : scene->GetAllEntitiesWith<PrefabComponent>().each())
			{
				const Entity entity(handle, scene.get());
				const std::string path = Prefab::NormalizePath(link.Prefab);
				// A nested instance changes with the instance holding it, whose source is expanded from the same file.
				if (path.empty() || Prefab::EnclosingInstance(entity))
					continue;
				const Prefab::Source& source = sources.Read(path);
				if (changedFile)
				{
					// A changed file reaches its own prefab's instances and the tracked instances of prefabs nesting or based
					// on it. Adopting an untracked one here would freeze this very edit as its overrides.
					if (source.Key != *changedFile && (!source.Report.Files.contains(*changedFile) || !Prefab::IsTracked(link)))
						continue;
				}
				// Without one this catches up what was saved since a scene was: leaving untracked instances alone, as adopting
				// them would change scenes nobody edited.
				else if (!Prefab::IsTracked(link) || (source.Readable && !Prefab::IsOutdated(link, source.Records)))
					continue;
				roots.emplace_back(entity.GetUUID(), path);
			}
			size_t changed = 0;
			for (const auto& [id, path] : roots)
			{
				Entity root = scene->GetEntityByUUID(id);
				if (!root || !root.HasComponent<PrefabComponent>())
					continue;
				const Prefab::Source& source = sources.Read(path);
				addWarnings(source.Report.Warnings);
				std::string error = source.Error;
				std::vector<std::string> moved;
				const auto result = source.Readable ? Prefab::Refresh(root, source.Records, Prefab::RefreshMode::Merge, error, &moved) : Prefab::RefreshResult::Rejected;
				addWarnings(moved);
				if (result == Prefab::RefreshResult::Merged)
					++report.PrefabInstances;
				if (Prefab::ChangedScene(result))
					++changed;
				if (Prefab::Failed(result))
				{
					const char* outcome = result == Prefab::RefreshResult::Partial ? "was only partly merged (merge it again with prefab.refresh)" : "kept its previous state";
					report.PrefabErrors.push_back(fmt::format("instance '{}' ({}) of {} {}: {}", root.GetName(), static_cast<uint64_t>(id), path, outcome, error));
					BR_CORE_WARN("Prefab: {}", report.PrefabErrors.back());
				}
			}
			if (changed == 0)
				continue;
			if (scene == m_EditScene)
			{
				report.PrefabEditSceneChanges += changed;
				MarkDirty();
			}
			else if (m_PrefabEditing)
				m_PrefabEditing->WaitingDirty = true;
		}
	}

	AssetReloadReport AutomationSession::MergeOutdatedPrefabInstances()
	{
		AssetReloadReport report;
		MergePrefabInstances(std::nullopt, report);
		return report;
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
		if (const auto& project = Project::GetActive())
		{
			std::string error;
			if (!project->GetAssets().Refresh(error))
				BR_CORE_ERROR("Asset metadata refresh: {}", error);
		}
		std::vector<std::string> paths;
		std::vector<std::string> scripts;
		std::vector<std::string> prefabs;
		for (const FileChange& change : changes)
		{
			// AssetManager keys and script paths are normalized project-relative paths.
			const std::filesystem::path normal = FileSystem::PathFromUtf8(change.Path).lexically_normal();
			const std::string path = FileSystem::PathToGenericUtf8(normal);
			report.Changed.emplace_back(path, change.Type);
			paths.push_back(path);
			const std::string extension = FileSystem::GetLowercaseExtension(normal);
			// A removed script has nothing to load: the running version stays until play stops.
			if (change.Type != FileChangeType::Removed && extension == ".lua")
				scripts.push_back(path);
			// A removed prefab is reported per instance (they keep their state).
			if (extension == ".bprefab")
				prefabs.push_back(path);
		}
		report.Assets = AssetManager::ReloadChanged(paths);
		for (const std::string& path : prefabs)
			MergePrefabInstances(path, report);
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
			BR_CORE_INFO("Hot reload: {} changed file(s); reloaded {} asset(s) and {} script(s) ({} instance(s)){}", changes.size(), report.Assets.size(), report.Scripts.Reloaded.size(), report.Scripts.Instances, failures);
		}
		if (OnAssetsReloaded)
			OnAssetsReloaded(report);
		return report;
	}

	void AutomationSession::AcknowledgeAssetMetadata()
	{
		// Metadata encoded by a save belongs to the same host write, not the next external refresh.
		if (Project::GetActive())
			for (const auto& written : Project::GetActive()->GetAssets().TakeWrittenPaths())
				AcknowledgeFile(written);
	}

	AssetReloadReport AutomationSession::OnFileWritten(const std::string& relativePath)
	{
		AcknowledgeAssetMetadata();
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
		if (IsEditingPrefab())
		{
			outError = "close the open prefab (prefab.close) before Play or Simulate: a prefab plays in the scenes that place it";
			return false;
		}
		m_EditScene->ResetParticlePreview();
		m_EditScene->ResetAnimationPreview();
		m_Scene = Scene::Copy(m_EditScene);
		m_Scene->SetNetworkTransports(ScriptsUseUdp ? EnetTransport::CreateFactory() : GetSimulatedNetwork().CreateFactory());
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
		StopNetworkClients();
		if (m_Scene->GetState() == SceneState::Simulate)
			m_Scene->OnSimulationStop();
		else
			m_Scene->OnRuntimeStop();
		m_Network.reset();
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
			UpdatePlayScene(frameTime);
			Input::EndFrame();
		}
	}

	void AutomationSession::UpdatePlayScene(float frameTime)
	{
		if (!IsPlaying())
			return;
		if (m_Network)
			m_Network->Advance(frameTime);
		for (size_t i = 0; i < m_NetworkClients.size(); i++)
		{
			const Ref<Scene> client = m_NetworkClients[i];
			if (!client)
				continue;
			{
				const ScopedGameMute mute(m_InputPeer != i + 1);
				client->OnUpdate(frameTime);
			}
			// A client's Game.Quit ends that client.
			if (client->IsQuitRequested())
			{
				BR_CORE_INFO("Network: in-process client {} quit", i + 1);
				EndNetworkClient(i + 1);
			}
		}
		const ScopedGameMute mute(m_InputPeer != 0);
		m_Scene->OnUpdate(frameTime);
	}

	bool AutomationSession::SetInputPeer(size_t peer)
	{
		if (peer != 0 && !GetNetworkClient(peer))
			return false;
		m_InputPeer = peer;
		return true;
	}

	void AutomationSession::EndNetworkClient(size_t index)
	{
		const Ref<Scene> client = GetNetworkClient(index);
		if (!client)
			return;
		client->OnRuntimeStop();
		m_NetworkClients[index - 1].reset();
		if (m_InputPeer == index)
			m_InputPeer = 0;
	}

	bool AutomationSession::CanStartNetwork(std::string& outError) const
	{
		if (!IsPlaying() || IsSimulating())
		{
			outError = "not playing (use play.start)";
			return false;
		}
		if (m_Scene->GetNetworkSystem() && m_Scene->GetNetworkSystem()->GetState() != NetConnectionState::Disconnected)
		{
			outError = "the play scene is already in a network session (net.disconnect ends it)";
			return false;
		}
		return true;
	}

	AutomationSession::SceneOverride::SceneOverride(AutomationSession& session, Ref<Scene> scene)
		: m_Session(session)
		, m_Previous(session.m_Scene)
	{
		BR_CORE_ASSERT(scene && scene != session.m_EditScene, "a scene override names a playing peer's scene");
		m_Session.m_Scene = std::move(scene);
	}

	AutomationSession::SceneOverride::~SceneOverride()
	{
		m_Session.m_Scene = std::move(m_Previous);
	}

	SimulatedNetwork& AutomationSession::GetSimulatedNetwork()
	{
		if (!m_Network)
			m_Network = CreateScope<SimulatedNetwork>();
		return *m_Network;
	}

	bool AutomationSession::HostNetwork(bool udp, uint16_t port, std::string& outError)
	{
		if (!CanStartNetwork(outError))
			return false;
		const NetworkSettings settings = NetworkSettings::FromActiveProject();
		const Ref<NetTransportFactory> transports = udp ? EnetTransport::CreateFactory() : GetSimulatedNetwork().CreateFactory();
		Scope<NetTransport> transport = transports->Listen(port, settings.MaxClients + NetworkSystem::SpareTransportSlots, settings.TimeoutSeconds, outError);
		if (!transport)
			return false;
		BR_CORE_INFO("Network: hosting on {} port {} (up to {} clients)", transports->GetName(), transport->GetListenPort(), settings.MaxClients);
		m_Scene->StartNetwork(CreateScope<NetworkSystem>(*m_Scene, NetworkRole::Host, std::move(transport), settings, NetworkSystem::Identity::FromActiveProject()));
		return true;
	}

	bool AutomationSession::JoinNetwork(const std::string& address, uint16_t port, std::string& outError)
	{
		if (!CanStartNetwork(outError))
			return false;
		const NetworkSettings settings = NetworkSettings::FromActiveProject();
		Scope<NetTransport> transport = EnetTransport::CreateFactory()->Connect(address, port, settings.TimeoutSeconds, outError);
		if (!transport)
			return false;
		BR_CORE_INFO("Network: connecting to {}:{}", address, port);
		m_Scene->StartNetwork(CreateScope<NetworkSystem>(*m_Scene, NetworkRole::Client, std::move(transport), settings, NetworkSystem::Identity::FromActiveProject()));
		return true;
	}

	void AutomationSession::LeaveNetwork()
	{
		if (IsPlaying())
			m_Scene->StopNetwork();
	}

	std::optional<size_t> AutomationSession::AddNetworkClient(std::string& outError)
	{
		const NetworkSystem* host = IsPlaying() ? m_Scene->GetNetworkSystem() : nullptr;
		if (!host || host->GetRole() != NetworkRole::Host)
		{
			outError = "the play scene is not hosting (net.host first)";
			return std::nullopt;
		}
		if (!m_Network || !m_Network->IsListening(host->GetListenPort()))
		{
			outError = "the play scene hosts over UDP: in-process clients join an in-process host (net.host without udp)";
			return std::nullopt;
		}
		if (std::ranges::count_if(m_NetworkClients, [](const Ref<Scene>& client) { return client != nullptr; }) >= static_cast<std::ptrdiff_t>(MaxNetworkClients))
		{
			outError = fmt::format("at most {} in-process clients", MaxNetworkClients);
			return std::nullopt;
		}
		const NetworkSettings settings = NetworkSettings::FromActiveProject();
		Ref<Scene> client = Scene::Copy(m_EditScene);
		client->SetNetworkTransports(m_Network->CreateFactory());
		client->StartNetwork(CreateScope<NetworkSystem>(*client, NetworkRole::Client, m_Network->Connect(host->GetListenPort()), settings, NetworkSystem::Identity::FromActiveProject()));
		// Its scripts start without the input (net.focus routes it).
		{
			const ScopedGameMute mute(true);
			client->OnRuntimeStart();
		}
		const size_t index = m_NetworkClients.size() + 1;
		m_NetworkClients.push_back(client);
		return index;
	}

	bool AutomationSession::RemoveNetworkClient(size_t index, std::string& outError)
	{
		if (!GetNetworkClient(index))
		{
			outError = fmt::format("no in-process client {}", index);
			return false;
		}
		EndNetworkClient(index);
		return true;
	}

	Ref<Scene> AutomationSession::GetNetworkClient(size_t index) const
	{
		return index >= 1 && index <= m_NetworkClients.size() ? m_NetworkClients[index - 1] : nullptr;
	}

	void AutomationSession::StopNetworkClients()
	{
		for (Ref<Scene>& client : m_NetworkClients)
			if (client)
				client->OnRuntimeStop();
		m_NetworkClients.clear();
		m_InputPeer = 0;
	}

}
