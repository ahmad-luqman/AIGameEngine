#include "Basalt/Animation/AnimationSystem.h"

#include "Basalt/Animation/AnimationPose.h"
#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <algorithm>
#include <cmath>

namespace Basalt {

	namespace {
		int32_t FindClip(const MeshSource& source, const std::string& name)
		{
			if (source.Animations.empty())
				return -1;
			if (name.empty())
				return 0;
			for (size_t i = 0; i < source.Animations.size(); i++)
				if (source.Animations[i].Name == name)
					return static_cast<int32_t>(i);
			return -1;
		}
	}

	AnimationSystem::AnimationSystem(Scene& scene)
		: m_Scene(scene)
	{
		m_Scene.GetRegistry().on_destroy<AnimatorComponent>().connect<&AnimationSystem::OnDestroy>(*this);
		Update(0);
	}

	AnimationSystem::~AnimationSystem()
	{
		m_Scene.GetRegistry().on_destroy<AnimatorComponent>().disconnect<&AnimationSystem::OnDestroy>(*this);
	}

	void AnimationSystem::OnDestroy(entt::registry&, entt::entity handle)
	{
		m_EmptyMeshes.erase(handle);
		std::erase_if(m_Playback, [handle](const auto& entry) { return entry.second.Handle == handle; });
	}

	AnimationSystem::Playback* AnimationSystem::Ensure(Entity entity)
	{
		const auto* component = entity ? entity.TryGetComponent<AnimatorComponent>() : nullptr;
		if (!component)
			return nullptr;
		if (component->Mesh.empty())
		{
			if (m_EmptyMeshes.emplace(entity, true).second)
				BS_CORE_WARN("Animator '{}': Mesh is empty", entity.GetName());
			m_Playback.erase(entity.GetUUID());
			return nullptr;
		}
		m_EmptyMeshes.erase(entity);
		auto source = AssetManager::GetMesh(component->Mesh);
		if (!source || source->AnimationUnsupported)
		{
			m_Playback.erase(entity.GetUUID());
			return nullptr;
		}
		auto [it, inserted] = m_Playback.try_emplace(entity.GetUUID());
		auto& playback = it->second;
		if (inserted || playback.Handle != static_cast<entt::entity>(entity) || playback.Source != source || playback.ConfiguredClip != component->Clip)
		{
			playback = {};
			playback.Handle = entity;
			playback.EntityName = entity.GetName();
			playback.Source = source;
			playback.ConfiguredClip = component->Clip;
			playback.Clip = FindClip(*source, component->Clip);
			if (playback.Clip < 0 && !component->Clip.empty())
				BS_CORE_ERROR("Animator '{}': clip '{}' not found in '{}'", entity.GetName(), component->Clip, component->Mesh);
			playback.Playing = component->PlayOnStart && playback.Clip >= 0;
			Evaluate(playback);
		}
		return &playback;
	}

	void AnimationSystem::Evaluate(Playback& playback)
	{
		playback.Local = SampleAnimation(*playback.Source, playback.Rest || playback.Clip < 0 ? nullptr : &playback.Source->Animations[playback.Clip], static_cast<float>(playback.Time));
		if (playback.BlendWeight < 1 && !playback.Rest)
		{
			const auto from = playback.Outgoing >= 0 ? SampleAnimation(*playback.Source, &playback.Source->Animations[playback.Outgoing], static_cast<float>(playback.OutgoingTime)) : playback.Snapshot;
			BlendAnimationPose(playback.Local, from, playback.BlendWeight);
		}
		bool valid = ComposeAnimationPose(*playback.Source, playback.Local, playback.Globals);
		if (valid)
			for (const auto& skin : playback.Source->Skins)
				valid = valid && !BuildSkinPalette(skin, playback.Globals).empty();
		if (!valid)
		{
			if (!playback.InvalidPose)
				BS_CORE_ERROR("Animator '{}': non-finite evaluated pose; displaying rest pose", playback.EntityName);
			playback.Local = SampleAnimation(*playback.Source, nullptr, 0);
			ComposeAnimationPose(*playback.Source, playback.Local, playback.Globals);
		}
		playback.InvalidPose = !valid;
	}

	void AnimationSystem::Update(Timestep timestep)
	{
		const float seconds = timestep.GetSeconds();
		if (!std::isfinite(seconds) || seconds < 0)
			return;
		for (auto handle : m_Scene.GetAllEntitiesWith<AnimatorComponent>())
		{
			Entity entity(handle, &m_Scene);
			auto* playback = Ensure(entity);
			if (!playback || !playback->Playing || playback->Clip < 0)
				continue;
			const auto& component = entity.GetComponent<AnimatorComponent>();
			const bool invalidSpeed = !std::isfinite(component.Speed) || component.Speed < 0 || component.Speed > MaxAnimatorSpeed;
			if (invalidSpeed && !playback->InvalidSpeed)
				BS_CORE_WARN("Animator '{}': invalid Speed {}; clamping to [0, {}]", entity.GetName(), component.Speed, MaxAnimatorSpeed);
			playback->InvalidSpeed = invalidSpeed;
			const float speed = std::isfinite(component.Speed) ? std::clamp(component.Speed, 0.0f, MaxAnimatorSpeed) : 0;
			const double advance = static_cast<double>(seconds) * speed;
			auto advanceClock = [&](double& time, const AnimationClip& clip) {
				time += advance;
				if (clip.Duration > 0 && component.Looping)
					time = std::fmod(time, clip.Duration);
				else
					time = std::min(time, static_cast<double>(clip.Duration));
			};
			advanceClock(playback->Time, playback->Source->Animations[playback->Clip]);
			if (playback->Outgoing >= 0)
				advanceClock(playback->OutgoingTime, playback->Source->Animations[playback->Outgoing]);
			if (playback->FadeDuration > 0)
			{
				playback->FadeTime += seconds;
				playback->BlendWeight = static_cast<float>(std::min(playback->FadeTime / playback->FadeDuration, 1.0));
				if (playback->BlendWeight >= 1)
				{
					ClearTransition(*playback);
				}
			}
			if (!component.Looping && playback->Time >= playback->Source->Animations[playback->Clip].Duration && playback->FadeDuration == 0 &&
				(playback->Outgoing < 0 || playback->OutgoingTime >= playback->Source->Animations[playback->Outgoing].Duration))
				playback->Playing = false;
			Evaluate(*playback);
		}
	}

	void AnimationSystem::ClearTransition(Playback& playback)
	{
		playback.Outgoing = -1;
		playback.OutgoingTime = 0;
		playback.Snapshot = {};
		playback.FadeTime = 0;
		playback.FadeDuration = 0;
		playback.BlendWeight = 1;
	}

	bool AnimationSystem::Play(Entity entity, const std::string& clip)
	{
		auto* playback = Ensure(entity);
		if (!playback)
			return false;
		const int32_t selected = FindClip(*playback->Source, clip);
		if (selected < 0)
		{
			if (playback->Source->Animations.empty())
				BS_CORE_ERROR("Animator '{}': model '{}' has no animation clips", entity.GetName(), playback->Source->Key);
			else
				BS_CORE_ERROR("Animator '{}': clip '{}' not found", entity.GetName(), clip);
			return false;
		}
		ClearTransition(*playback);
		playback->Clip = selected;
		playback->Time = 0;
		playback->Playing = true;
		playback->Rest = false;
		Evaluate(*playback);
		return true;
	}

	bool AnimationSystem::CrossFade(Entity entity, const std::string& clip, float seconds)
	{
		if (!std::isfinite(seconds) || seconds < 0)
		{
			BS_CORE_ERROR("Animator '{}': animation seconds must be finite and nonnegative, got {}", entity.GetName(), seconds);
			return false;
		}
		if (seconds == 0)
			return Play(entity, clip);
		auto* playback = Ensure(entity);
		const int32_t selected = playback ? FindClip(*playback->Source, clip) : -1;
		if (selected < 0)
		{
			if (playback)
				BS_CORE_ERROR("Animator '{}': crossfade clip '{}' not found", entity.GetName(), clip);
			return false;
		}
		playback->Snapshot = playback->Local;
		playback->Outgoing = playback->BlendWeight >= 1 && !playback->Rest ? playback->Clip : -1;
		playback->OutgoingTime = playback->Time;
		playback->Clip = selected;
		playback->Time = 0;
		playback->FadeTime = 0;
		playback->FadeDuration = seconds;
		playback->BlendWeight = 0;
		playback->Playing = true;
		playback->Rest = false;
		Evaluate(*playback);
		return true;
	}

	bool AnimationSystem::Blend(Entity entity, const std::string& first, const std::string& second, float weight)
	{
		if (!std::isfinite(weight) || weight < 0 || weight > 1)
		{
			BS_CORE_ERROR("Animator '{}': blend weight must be finite and in [0, 1], got {}", entity.GetName(), weight);
			return false;
		}
		auto* playback = Ensure(entity);
		const int32_t a = playback ? FindClip(*playback->Source, first) : -1;
		const int32_t b = playback ? FindClip(*playback->Source, second) : -1;
		if (a < 0 || b < 0)
		{
			if (playback)
				BS_CORE_ERROR("Animator '{}': blend clip '{}' not found", entity.GetName(), a < 0 ? first : second);
			return false;
		}
		// Updating a locomotion blend's weight must not restart either clock.
		if (playback->Outgoing != a || playback->Clip != b || playback->FadeDuration > 0)
			playback->Time = playback->OutgoingTime = 0;
		playback->Outgoing = a;
		playback->Clip = b;
		playback->BlendWeight = weight;
		playback->FadeDuration = 0;
		playback->Snapshot = {};
		playback->Playing = true;
		playback->Rest = false;
		Evaluate(*playback);
		return true;
	}

	void AnimationSystem::Pause(Entity entity, bool paused)
	{
		if (auto* playback = Ensure(entity))
			playback->Playing = !paused && playback->Clip >= 0 && !playback->Rest;
	}

	void AnimationSystem::Stop(Entity entity)
	{
		if (auto* playback = Ensure(entity))
		{
			playback->Playing = false;
			playback->Time = 0;
			playback->Rest = true;
			ClearTransition(*playback);
			Evaluate(*playback);
		}
	}

	bool AnimationSystem::Seek(Entity entity, float seconds)
	{
		if (!std::isfinite(seconds) || seconds < 0)
		{
			BS_CORE_ERROR("Animator '{}': animation seconds must be finite and nonnegative, got {}", entity.GetName(), seconds);
			return false;
		}
		auto* playback = Ensure(entity);
		if (!playback || playback->Clip < 0)
			return false;
		playback->Time = std::min(seconds, playback->Source->Animations[playback->Clip].Duration);
		if (playback->Outgoing >= 0)
			playback->OutgoingTime = std::min(seconds, playback->Source->Animations[playback->Outgoing].Duration);
		playback->Rest = false;
		Evaluate(*playback);
		return true;
	}

	float AnimationSystem::GetTime(Entity entity)
	{
		auto* playback = Ensure(entity);
		return playback ? static_cast<float>(playback->Time) : 0;
	}

	float AnimationSystem::GetDuration(Entity entity)
	{
		auto* playback = Ensure(entity);
		return playback && playback->Clip >= 0 ? playback->Source->Animations[playback->Clip].Duration : 0;
	}

	bool AnimationSystem::IsPlaying(Entity entity)
	{
		auto* playback = Ensure(entity);
		return playback && playback->Playing;
	}

	std::vector<std::string> AnimationSystem::GetClips(Entity entity)
	{
		std::vector<std::string> names;
		if (auto* playback = Ensure(entity))
			for (const auto& clip : playback->Source->Animations)
				names.push_back(clip.Name);
		return names;
	}

	const std::vector<glm::mat4>* AnimationSystem::GetPose(Entity entity)
	{
		auto* playback = Ensure(entity);
		return playback ? &playback->Globals : nullptr;
	}
}
