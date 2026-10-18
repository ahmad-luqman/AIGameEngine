#include "Basalt/Animation/AnimationSystem.h"

#include "Basalt/Animation/AnimationPose.h"
#include "Basalt/Animation/BlendSpace.h"
#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_set>
#include <utility>

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

		int32_t FindNode(const MeshSource& source, const std::string& name)
		{
			for (size_t i = 0; i < source.Nodes.size(); i++)
				if (source.Nodes[i].Name == name)
					return static_cast<int32_t>(i);
			return -1;
		}

		const AnimationBlendSpace* FindSpace(const AnimatorComponent& component, const std::string& name)
		{
			for (const AnimationBlendSpace& space : component.BlendSpaces)
				if (space.Name == name)
					return &space;
			return nullptr;
		}

		// A blend space resolved against a model at one parameter.
		struct ResolvedSpace
		{
			std::vector<int32_t> Clips;
			std::vector<float> Weights;
			// Blended duration: what one cycle of the synchronized clips lasts.
			double Duration = 0;
			// The heaviest sample's clip.
			int32_t Dominant = -1;
		};

		// Weights of the samples whose clips exist, renormalized; false with the reason when none can play.
		bool ResolveBlendSpace(const MeshSource& source, const AnimationBlendSpace& space, glm::vec2 parameter, ResolvedSpace& out, std::string& outError)
		{
			out.Weights = ComputeBlendSpaceWeights(space, parameter);
			out.Clips.assign(space.Samples.size(), -1);
			float total = 0;
			float heaviest = 0;
			for (size_t i = 0; i < space.Samples.size(); i++)
			{
				out.Clips[i] = FindClip(source, space.Samples[i].Clip);
				if (out.Clips[i] < 0)
				{
					outError = fmt::format("blend space '{}' sample clip '{}' not found", space.Name, space.Samples[i].Clip);
					out.Weights[i] = 0;
				}
				total += out.Weights[i];
			}
			if (total <= 0)
			{
				if (outError.empty())
					outError = fmt::format("blend space '{}' has no samples", space.Name);
				return false;
			}
			out.Duration = 0;
			for (size_t i = 0; i < out.Weights.size(); i++)
			{
				out.Weights[i] /= total;
				if (out.Clips[i] < 0)
					continue;
				out.Duration += static_cast<double>(out.Weights[i]) * source.Animations[out.Clips[i]].Duration;
				if (out.Weights[i] > heaviest)
				{
					heaviest = out.Weights[i];
					out.Dominant = out.Clips[i];
				}
			}
			return true;
		}

		bool IsFinite(glm::vec2 value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y);
		}

		// Smoothing time constant of MatchVelocity's measured speed (s): long enough to hide fixed-step jitter
		// in where physics leaves a character, short enough to follow a start or stop within a few frames.
		constexpr double VelocitySmoothingSeconds = 0.1;
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
			{
				// Clip may name a blend space, played at parameter (0, 0).
				if (FindSpace(*component, component->Clip))
				{
					playback.Space = component->Clip;
					if (!ResolveSpace(playback, *component))
						playback.Space.clear();
				}
				else
					BS_CORE_ERROR("Animator '{}': clip '{}' not found in '{}'", entity.GetName(), component->Clip, component->Mesh);
			}
			playback.Playing = component->PlayOnStart && playback.Clip >= 0;
			Evaluate(playback);
		}
		return &playback;
	}

	bool AnimationSystem::ResolveSpace(Playback& playback, const AnimatorComponent& component)
	{
		const AnimationBlendSpace* space = FindSpace(component, playback.Space);
		ResolvedSpace resolved;
		std::string error;
		const bool playable = space && ResolveBlendSpace(*playback.Source, *space, playback.Parameter, resolved, error);
		if (!space)
			error = fmt::format("blend space '{}' not found", playback.Space);
		if (!error.empty() && !playback.SpaceProblemLogged)
		{
			BS_CORE_ERROR("Animator '{}': {}", playback.EntityName, error);
			playback.SpaceProblemLogged = true;
		}
		if (!playable)
			return false;
		playback.SpaceClips = std::move(resolved.Clips);
		playback.SpaceWeights = std::move(resolved.Weights);
		playback.SpaceDuration = resolved.Duration;
		playback.Clip = resolved.Dominant;
		playback.Time = playback.Phase * playback.SpaceDuration;
		return true;
	}

	void AnimationSystem::Evaluate(Playback& playback)
	{
		if (!playback.Space.empty() && !playback.Rest)
		{
			// Every clip at the same phase of its own duration, weighted by the sample weights.
			float total = 0;
			for (size_t i = 0; i < playback.SpaceClips.size(); i++)
			{
				const float weight = playback.SpaceWeights[i];
				if (playback.SpaceClips[i] < 0 || weight <= 0)
					continue;
				const AnimationClip& clip = playback.Source->Animations[playback.SpaceClips[i]];
				AnimationLocalPose pose = SampleAnimation(*playback.Source, &clip, static_cast<float>(playback.Phase * clip.Duration));
				total += weight;
				if (total > weight)
					BlendAnimationPose(pose, playback.Local, weight / total);
				playback.Local = std::move(pose);
			}
			if (total <= 0)
				playback.Local = SampleAnimation(*playback.Source, nullptr, 0);
		}
		else
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
			if (!playback)
				continue;
			const auto& component = entity.GetComponent<AnimatorComponent>();
			if (component.MatchVelocity && seconds > 0)
			{
				// Measured while paused too, so resuming does not see the whole pause's movement at once.
				const glm::vec3 position = m_Scene.GetWorldTransform(entity)[3];
				if (playback->HasLastPosition)
				{
					const glm::vec3 delta = position - playback->LastPosition;
					const double horizontal = glm::length(glm::vec2(delta.x, delta.z)) / seconds;
					const double blend = 1.0 - std::exp(-static_cast<double>(seconds) / VelocitySmoothingSeconds);
					if (std::isfinite(horizontal))
						playback->MeasuredSpeed += (horizontal - playback->MeasuredSpeed) * blend;
				}
				playback->LastPosition = position;
				playback->HasLastPosition = true;
			}
			if (!playback->Playing || playback->Clip < 0)
				continue;
			const bool inSpace = !playback->Space.empty();
			if (inSpace && !ResolveSpace(*playback, component))
			{
				// Holds the last pose until the space is fixed or another animation plays.
				playback->Playing = false;
				continue;
			}
			const bool invalidSpeed = !std::isfinite(component.Speed) || component.Speed < 0 || component.Speed > MaxAnimatorSpeed;
			if (invalidSpeed && !playback->InvalidSpeed)
				BS_CORE_WARN("Animator '{}': invalid Speed {}; clamping to [0, {}]", entity.GetName(), component.Speed, MaxAnimatorSpeed);
			playback->InvalidSpeed = invalidSpeed;
			double speed = std::isfinite(component.Speed) ? std::clamp(component.Speed, 0.0f, MaxAnimatorSpeed) : 0;
			if (inSpace && component.MatchVelocity)
			{
				// Play the clips as fast as the entity really moves; a sample of unknown speed (0) turns it off.
				double blendedSpeed = 0;
				bool known = true;
				const AnimationBlendSpace& space = *FindSpace(component, playback->Space);
				for (size_t i = 0; i < space.Samples.size(); i++)
				{
					if (playback->SpaceWeights[i] <= 0)
						continue;
					known = known && space.Samples[i].Speed > 0;
					blendedSpeed += static_cast<double>(playback->SpaceWeights[i]) * space.Samples[i].Speed;
				}
				if (known && blendedSpeed > 0)
					speed = std::clamp(speed * playback->MeasuredSpeed / blendedSpeed, 0.0, static_cast<double>(MaxAnimatorSpeed));
			}
			const double advance = static_cast<double>(seconds) * speed;
			const UUID id = entity.GetUUID();
			playback->EventsThisFrame = 0;
			auto advanceClock = [&](double& time, const AnimationClip& clip) {
				time += advance;
				if (clip.Duration > 0 && component.Looping)
					time = std::fmod(time, clip.Duration);
				else
					time = std::min(time, static_cast<double>(clip.Duration));
			};
			if (inSpace)
				AdvanceSpace(*playback, component, advance, id);
			else
			{
				CollectEvents(*playback, component, playback->Clip, playback->Time, advance, id);
				advanceClock(playback->Time, playback->Source->Animations[playback->Clip]);
			}
			if (playback->Outgoing >= 0)
			{
				if (playback->BlendWeight < 1)
					CollectEvents(*playback, component, playback->Outgoing, playback->OutgoingTime, advance, id);
				advanceClock(playback->OutgoingTime, playback->Source->Animations[playback->Outgoing]);
			}
			if (playback->FadeDuration > 0)
			{
				playback->FadeTime += seconds;
				playback->BlendWeight = static_cast<float>(std::min(playback->FadeTime / playback->FadeDuration, 1.0));
				if (playback->BlendWeight >= 1)
				{
					ClearTransition(*playback);
				}
			}
			const bool finished = inSpace ? playback->Phase >= 1 : playback->Time >= playback->Source->Animations[playback->Clip].Duration;
			if (!component.Looping && finished && playback->FadeDuration == 0 &&
				(playback->Outgoing < 0 || playback->OutgoingTime >= playback->Source->Animations[playback->Outgoing].Duration))
				playback->Playing = false;
			Evaluate(*playback);
		}
		UpdateAttachments();
	}

	void AnimationSystem::AdvanceSpace(Playback& playback, const AnimatorComponent& component, double advance, UUID id)
	{
		if (playback.SpaceDuration <= 0)
			return;
		// The heaviest sample alone fires events: the clips are in step, so each would fire its own copy.
		const double phaseAdvance = advance / playback.SpaceDuration;
		const double dominantDuration = playback.Source->Animations[playback.Clip].Duration;
		CollectEvents(playback, component, playback.Clip, playback.Phase * dominantDuration, phaseAdvance * dominantDuration, id);
		playback.Phase += phaseAdvance;
		playback.Phase = component.Looping ? std::fmod(playback.Phase, 1.0) : std::min(playback.Phase, 1.0);
		playback.Time = playback.Phase * playback.SpaceDuration;
	}

	void AnimationSystem::CollectEvents(Playback& playback, const AnimatorComponent& component, int32_t clip, double from, double advance, UUID id)
	{
		if (advance <= 0 || component.Events.empty())
			return;
		const AnimationClip& animation = playback.Source->Animations[clip];
		const double duration = animation.Duration;
		std::vector<const AnimationEvent*> markers;
		for (const AnimationEvent& event : component.Events)
			if ((event.Clip.empty() || event.Clip == animation.Name) && event.Time <= duration)
				markers.push_back(&event);
		if (markers.empty())
			return;
		std::ranges::stable_sort(markers, {}, &AnimationEvent::Time);
		// Fires the markers in [start, end), or [start, end] when `closed` (the clip's end is reached).
		auto fire = [&](double start, double end, bool closed) {
			for (const AnimationEvent* marker : markers)
			{
				if (marker->Time < start || marker->Time > end || (!closed && marker->Time == end))
					continue;
				if (playback.EventsThisFrame >= MaxEventsPerFrame)
				{
					if (!playback.EventLimitLogged)
						BS_CORE_WARN("Animator '{}': more than {} animation events in one frame; dropping the rest", playback.EntityName, MaxEventsPerFrame);
					playback.EventLimitLogged = true;
					return false;
				}
				playback.EventsThisFrame++;
				m_PendingEvents.push_back({ id, marker->Name, animation.Name });
			}
			return true;
		};
		if (component.Looping && duration > 0)
		{
			// Every wrap the step covers fires the clip's tail (end included) and restarts at 0. Each full loop
			// fires at least one marker, so the event cap also bounds this loop.
			double start = from;
			double remaining = advance;
			while (remaining > 0)
			{
				const double room = duration - start;
				if (remaining < room)
				{
					fire(start, start + remaining, false);
					break;
				}
				if (!fire(start, duration, true))
					break;
				remaining -= room;
				start = 0;
			}
		}
		else if (from < duration)
		{
			const double to = from + advance;
			fire(from, std::min(to, duration), to >= duration);
		}
	}

	void AnimationSystem::UpdateAttachments()
	{
		std::vector<Entity> attached;
		for (auto handle : m_Scene.GetAllEntitiesWith<BoneAttachmentComponent>())
			attached.emplace_back(handle, &m_Scene);
		if (attached.empty())
		{
			m_AttachmentProblems.clear();
			return;
		}
		auto targetOf = [this](Entity entity) {
			const UUID target = entity.GetComponent<BoneAttachmentComponent>().Target;
			return target != 0 ? m_Scene.GetEntityByUUID(target) : entity.GetParent();
		};
		// An attachment moves after every attachment that moves its target or one of its own or the target's
		// ancestors, so chains (a hat on a rider on a horse's back) settle in one frame. Attachments that depend
		// on each other in a cycle get -1 and stay put; ones that only depend on a cycle follow it.
		std::unordered_map<entt::entity, int32_t> depths;
		std::vector<entt::entity> visiting;
		std::unordered_set<entt::entity> cyclic;
		std::function<int32_t(Entity)> depthOf = [&](Entity entity) -> int32_t {
			if (auto it = depths.find(entity); it != depths.end())
				return it->second;
			if (auto on = std::ranges::find(visiting, static_cast<entt::entity>(entity)); on != visiting.end())
			{
				cyclic.insert(on, visiting.end());
				return -1;
			}
			visiting.push_back(entity);
			int32_t depth = 0;
			auto visit = [&](Entity dependency) {
				for (; dependency; dependency = dependency.GetParent())
				{
					if (dependency == entity || !dependency.HasComponent<BoneAttachmentComponent>())
						continue;
					if (const int32_t other = depthOf(dependency); other >= 0)
						depth = std::max(depth, other + 1);
				}
			};
			visit(targetOf(entity));
			visit(entity.GetParent());
			visiting.pop_back();
			return depths[entity] = cyclic.contains(entity) ? -1 : depth;
		};
		std::vector<std::pair<int32_t, Entity>> ordered;
		ordered.reserve(attached.size());
		for (Entity entity : attached)
			ordered.emplace_back(depthOf(entity), entity);
		std::ranges::stable_sort(ordered, {}, &std::pair<int32_t, Entity>::first);

		std::unordered_map<UUID, std::string> problems;
		for (const auto& [depth, entity] : ordered)
		{
			const auto& attachment = entity.GetComponent<BoneAttachmentComponent>();
			const Entity target = targetOf(entity);
			std::string problem;
			glm::mat4 bone(1.0f);
			if (depth < 0)
				problem = "its attachments form a cycle";
			else if (!target)
				problem = attachment.Target != 0 ? "Target entity not found" : "no Target and no parent";
			else if (target == entity)
				problem = "it cannot attach to itself";
			else if (!GetBoneTransform(target, attachment.Joint, bone, problem))
				problem = fmt::format("Target '{}': {}", target.GetName(), problem);
			if (!problem.empty())
			{
				auto previous = m_AttachmentProblems.find(entity.GetUUID());
				if (previous == m_AttachmentProblems.end() || previous->second != problem)
					BS_CORE_ERROR("BoneAttachment '{}': {}", entity.GetName(), problem);
				problems[entity.GetUUID()] = std::move(problem);
				continue;
			}
			const glm::mat4 offset = glm::translate(glm::mat4(1.0f), attachment.Translation) * glm::mat4_cast(attachment.Rotation) * glm::scale(glm::mat4(1.0f), attachment.Scale);
			m_Scene.SetWorldTransform(entity, m_Scene.GetWorldTransform(target) * bone * offset);
		}
		m_AttachmentProblems = std::move(problems);
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
		playback->Space.clear();
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
		playback->Outgoing = playback->BlendWeight >= 1 && !playback->Rest && playback->Space.empty() ? playback->Clip : -1;
		playback->OutgoingTime = playback->Time;
		playback->Space.clear();
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
		if (playback->Outgoing != a || playback->Clip != b || playback->FadeDuration > 0 || !playback->Space.empty())
			playback->Time = playback->OutgoingTime = 0;
		playback->Space.clear();
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
			playback->Space.clear();
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
		if (!playback->Space.empty())
		{
			playback->Phase = playback->SpaceDuration > 0 ? std::min(seconds / playback->SpaceDuration, 1.0) : 0.0;
			playback->Time = playback->Phase * playback->SpaceDuration;
		}
		else
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
		if (playback && !playback->Space.empty())
			return static_cast<float>(playback->SpaceDuration);
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

	bool AnimationSystem::PlayBlendSpace(Entity entity, const std::string& space, glm::vec2 parameter, float fadeSeconds)
	{
		if (!IsFinite(parameter))
		{
			BS_CORE_ERROR("Animator '{}': blend space parameter must be finite, got ({}, {})", entity.GetName(), parameter.x, parameter.y);
			return false;
		}
		if (!std::isfinite(fadeSeconds) || fadeSeconds < 0)
		{
			BS_CORE_ERROR("Animator '{}': animation seconds must be finite and nonnegative, got {}", entity.GetName(), fadeSeconds);
			return false;
		}
		auto* playback = Ensure(entity);
		if (!playback)
			return false;
		const auto& component = entity.GetComponent<AnimatorComponent>();
		if (playback->Space == space && !playback->Rest)
		{
			if (!SetBlendSpaceParameter(entity, parameter))
				return false;
			playback->Playing = true;
			return true;
		}
		const AnimationBlendSpace* definition = FindSpace(component, space);
		ResolvedSpace resolved;
		std::string error;
		if (!definition)
			error = fmt::format("blend space '{}' not found", space);
		if (!definition || !ResolveBlendSpace(*playback->Source, *definition, parameter, resolved, error))
		{
			BS_CORE_ERROR("Animator '{}': {}", entity.GetName(), error);
			return false;
		}
		if (fadeSeconds > 0)
		{
			playback->Snapshot = playback->Local;
			playback->Outgoing = playback->BlendWeight >= 1 && !playback->Rest && playback->Space.empty() ? playback->Clip : -1;
			playback->OutgoingTime = playback->Time;
			playback->FadeTime = 0;
			playback->FadeDuration = fadeSeconds;
			playback->BlendWeight = 0;
		}
		else
			ClearTransition(*playback);
		playback->Space = space;
		playback->Parameter = parameter;
		playback->Phase = 0;
		playback->SpaceProblemLogged = false;
		ResolveSpace(*playback, component);
		playback->Playing = true;
		playback->Rest = false;
		Evaluate(*playback);
		return true;
	}

	bool AnimationSystem::SetBlendSpaceParameter(Entity entity, glm::vec2 parameter)
	{
		if (!IsFinite(parameter))
		{
			BS_CORE_ERROR("Animator '{}': blend space parameter must be finite, got ({}, {})", entity.GetName(), parameter.x, parameter.y);
			return false;
		}
		auto* playback = Ensure(entity);
		if (!playback)
			return false;
		if (playback->Space.empty() || playback->Rest)
		{
			BS_CORE_ERROR("Animator '{}': no blend space is playing", entity.GetName());
			return false;
		}
		playback->Parameter = parameter;
		if (ResolveSpace(*playback, entity.GetComponent<AnimatorComponent>()))
			Evaluate(*playback);
		return true;
	}

	std::vector<std::pair<std::string, float>> AnimationSystem::GetBlendSpaceWeights(Entity entity)
	{
		std::vector<std::pair<std::string, float>> weights;
		auto* playback = Ensure(entity);
		if (!playback || playback->Space.empty() || playback->Rest)
			return weights;
		for (size_t i = 0; i < playback->SpaceClips.size(); i++)
		{
			if (playback->SpaceClips[i] < 0)
				continue;
			const std::string& name = playback->Source->Animations[playback->SpaceClips[i]].Name;
			auto it = std::ranges::find(weights, name, &std::pair<std::string, float>::first);
			if (it == weights.end())
				weights.emplace_back(name, playback->SpaceWeights[i]);
			else
				it->second += playback->SpaceWeights[i];
		}
		return weights;
	}

	bool AnimationSystem::GetBoneTransform(Entity entity, const std::string& joint, glm::mat4& outTransform, std::string& outError)
	{
		auto* playback = Ensure(entity);
		if (!playback)
		{
			outError = "no playable Animator";
			return false;
		}
		const int32_t node = FindNode(*playback->Source, joint);
		if (node < 0)
		{
			outError = fmt::format("joint '{}' not found in '{}'", joint, playback->Source->Key);
			return false;
		}
		outTransform = playback->Globals[static_cast<size_t>(node)];
		return true;
	}

	std::vector<AnimationEventRecord> AnimationSystem::TakeEvents()
	{
		return std::exchange(m_PendingEvents, {});
	}
}
