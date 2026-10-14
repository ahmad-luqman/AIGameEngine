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
		const AnimationClip* FindClip(const MeshSource& source, const std::string& name)
		{
			if (source.Animations.empty())
				return nullptr;
			if (name.empty())
				return &source.Animations.front();
			for (const auto& clip : source.Animations)
				if (clip.Name == name)
					return &clip;
			return nullptr;
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
		std::erase_if(m_Playback, [handle](const auto& entry) { return entry.second.Handle == handle; });
	}

	AnimationSystem::Playback* AnimationSystem::Ensure(Entity entity)
	{
		const auto* component = entity ? entity.TryGetComponent<AnimatorComponent>() : nullptr;
		if (!component)
			return nullptr;
		auto source = AssetManager::GetMesh(component->Mesh);
		if (!source)
		{
			m_Playback.erase(entity.GetUUID());
			return nullptr;
		}
		auto [it, inserted] = m_Playback.try_emplace(entity.GetUUID());
		auto& playback = it->second;
		if (inserted || playback.Source != source || playback.ConfiguredClip != component->Clip)
		{
			playback = {};
			playback.Handle = entity;
			playback.Source = source;
			playback.ConfiguredClip = component->Clip;
			playback.Clip = FindClip(*source, component->Clip);
			if (!playback.Clip && !component->Clip.empty())
				BS_CORE_ERROR("Animator '{}': clip '{}' not found in '{}'", entity.GetName(), component->Clip, component->Mesh);
			playback.Playing = component->PlayOnStart && playback.Clip;
			Evaluate(playback);
		}
		return &playback;
	}

	void AnimationSystem::Evaluate(Playback& playback)
	{
		playback.Local = SampleAnimation(*playback.Source, playback.Rest ? nullptr : playback.Clip, static_cast<float>(playback.Time));
		if (playback.BlendWeight < 1 && !playback.Rest)
		{
			const auto from = playback.Outgoing ? SampleAnimation(*playback.Source, playback.Outgoing, static_cast<float>(playback.OutgoingTime)) : playback.Snapshot;
			BlendAnimationPose(playback.Local, from, playback.BlendWeight);
		}
		bool valid = ComposeAnimationPose(*playback.Source, playback.Local, playback.Globals);
		if (valid)
			for (const auto& skin : playback.Source->Skins)
				valid = valid && !BuildSkinPalette(skin, playback.Globals).empty();
		if (!valid)
		{
			if (!playback.InvalidPose)
				BS_CORE_ERROR("Animator '{}': non-finite evaluated pose; displaying rest pose", playback.Source->Key);
			playback.Local = SampleAnimation(*playback.Source, nullptr, 0);
			ComposeAnimationPose(*playback.Source, playback.Local, playback.Globals);
		}
		playback.InvalidPose = !valid;
	}

	void AnimationSystem::Update(float seconds)
	{
		if (!std::isfinite(seconds) || seconds < 0)
			return;
		for (auto handle : m_Scene.GetAllEntitiesWith<AnimatorComponent>())
		{
			Entity entity(handle, &m_Scene);
			auto* playback = Ensure(entity);
			if (!playback || !playback->Playing || !playback->Clip)
				continue;
			const auto& component = entity.GetComponent<AnimatorComponent>();
			const float speed = std::isfinite(component.Speed) ? std::clamp(component.Speed, 0.0f, 100.0f) : 0;
			const double advance = static_cast<double>(seconds) * speed;
			auto advanceClock = [&](double& time, const AnimationClip& clip) {
				time += advance;
				if (clip.Duration > 0 && component.Looping)
					time = std::fmod(time, clip.Duration);
				else
					time = std::min(time, static_cast<double>(clip.Duration));
			};
			advanceClock(playback->Time, *playback->Clip);
			if (playback->Outgoing)
				advanceClock(playback->OutgoingTime, *playback->Outgoing);
			if (playback->FadeDuration > 0)
			{
				playback->FadeTime += seconds;
				playback->BlendWeight = static_cast<float>(std::min(playback->FadeTime / playback->FadeDuration, 1.0));
				if (playback->BlendWeight >= 1)
				{
					playback->Outgoing = nullptr;
					playback->Snapshot = {};
					playback->FadeDuration = 0;
				}
			}
			if (!component.Looping && playback->Time >= playback->Clip->Duration && playback->FadeDuration == 0 &&
				(!playback->Outgoing || playback->OutgoingTime >= playback->Outgoing->Duration))
				playback->Playing = false;
			Evaluate(*playback);
		}
	}

	bool AnimationSystem::Play(Entity entity, const std::string& clip)
	{
		auto* playback = Ensure(entity);
		if (!playback)
			return false;
		const auto* selected = FindClip(*playback->Source, clip);
		if (!selected)
		{
			BS_CORE_ERROR("Animator '{}': clip '{}' not found", entity.GetName(), clip);
			return false;
		}
		playback->Outgoing = nullptr;
		playback->Snapshot = {};
		playback->FadeDuration = 0;
		playback->BlendWeight = 1;
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
			return false;
		if (seconds == 0)
			return Play(entity, clip);
		auto* playback = Ensure(entity);
		const auto* selected = playback ? FindClip(*playback->Source, clip) : nullptr;
		if (!selected)
			return false;
		playback->Snapshot = playback->Local;
		playback->Outgoing = playback->BlendWeight >= 1 && !playback->Rest ? playback->Clip : nullptr;
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
			return false;
		auto* playback = Ensure(entity);
		const auto* a = playback ? FindClip(*playback->Source, first) : nullptr;
		const auto* b = playback ? FindClip(*playback->Source, second) : nullptr;
		if (!a || !b)
			return false;
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
			playback->Playing = !paused && playback->Clip && !playback->Rest;
	}

	void AnimationSystem::Stop(Entity entity)
	{
		if (auto* playback = Ensure(entity))
		{
			playback->Playing = false;
			playback->Time = 0;
			playback->Rest = true;
			playback->Outgoing = nullptr;
			playback->BlendWeight = 1;
			playback->FadeDuration = 0;
			playback->Snapshot = {};
			Evaluate(*playback);
		}
	}

	bool AnimationSystem::Seek(Entity entity, float seconds)
	{
		if (!std::isfinite(seconds) || seconds < 0)
			return false;
		auto* playback = Ensure(entity);
		if (!playback || !playback->Clip)
			return false;
		playback->Time = std::min(seconds, playback->Clip->Duration);
		if (playback->Outgoing)
			playback->OutgoingTime = std::min(seconds, playback->Outgoing->Duration);
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
		return playback && playback->Clip ? playback->Clip->Duration : 0;
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
