#include "Basalt/Automation/AutomationSession.h"

#include "Basalt/Core/Input.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/Scene/Entity.h"

#include <algorithm>

namespace Basalt {

	AutomationSession::AutomationSession()
	{
		SetEditScene(CreateRef<Scene>("Untitled"), "");
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
		if (simulate)
			m_Scene->OnSimulationStart();
		else
			m_Scene->OnRuntimeStart();
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
		m_Scene = m_EditScene;
		PruneSelection();
		// Like held keys, injected gamepads belong to the play session.
		Input::Reset();
		Input::ForgetInjectedGamepads();
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
		m_Recording = nlohmann::json::array();
		m_RecordingSeed = IsPlaying() ? std::optional<uint32_t>(m_Scene->GetRandomSeed()) : std::nullopt;
		m_RecordedView.reset();
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
		const uint64_t frame = IsPlaying() ? m_Scene->GetFrameCount() : 0;
		if (const GameUI::HostState view = GameUI::GetHostState(); m_RecordedView != view)
		{
			m_RecordedView = view;
			const nlohmann::json viewParams = {
				{ "origin", { view.ViewportOrigin.x, view.ViewportOrigin.y } },
				{ "size", { view.ViewportSize.x, view.ViewportSize.y } },
				{ "pointerEnabled", view.PointerEnabled },
				{ "navigationEnabled", view.NavigationEnabled },
			};
			m_Recording.push_back({ { "frame", frame }, { "command", "view" }, { "params", viewParams } });
		}
		m_Recording.push_back({ { "frame", frame }, { "command", command }, { "params", params } });
	}

	nlohmann::json AutomationSession::StopRecording()
	{
		nlohmann::json events = IsRecording() ? std::move(m_Recording) : nlohmann::json::array();
		m_Recording = nullptr;
		m_RecordingSeed.reset();
		m_RecordedView.reset();
		return events;
	}

	void AutomationSession::Step(uint32_t frames, float frameTime)
	{
		if (!IsPlaying())
			return;
		for (uint32_t i = 0; i < frames; i++)
		{
			m_Scene->OnUpdate(frameTime);
			Input::EndFrame();
		}
	}

}
