#include "Basalt/Automation/AutomationSession.h"

#include "Basalt/Core/Input.h"
#include "Basalt/Scene/Scene.h"

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
		m_Selection = 0;
		m_Dirty = false;
		if (OnSceneChanged)
			OnSceneChanged();
	}

	bool AutomationSession::StartPlay(std::string& outError, bool simulate)
	{
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
		Input::Reset();
	}

	bool AutomationSession::IsPlaying() const
	{
		return m_Scene && m_Scene != m_EditScene && m_Scene->IsRunning();
	}

	bool AutomationSession::IsSimulating() const
	{
		return IsPlaying() && m_Scene->GetState() == SceneState::Simulate;
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
