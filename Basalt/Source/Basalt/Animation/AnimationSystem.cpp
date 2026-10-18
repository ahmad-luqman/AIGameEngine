#include "Basalt/Animation/AnimationSystem.h"

#include "Basalt/Animation/AnimatedModel.h"
#include "Basalt/Animation/AnimationPose.h"
#include "Basalt/Animation/BlendSpace.h"
#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <spdlog/fmt/ranges.h>

#include <algorithm>
#include <cmath>
#include <functional>
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

		bool CheckSeconds(Entity entity, float seconds, const char* what)
		{
			if (std::isfinite(seconds) && seconds >= 0)
				return true;
			BS_CORE_ERROR("Animator '{}': {} must be finite and nonnegative, got {}", entity.GetName(), what, seconds);
			return false;
		}

		bool CheckParameter(Entity entity, glm::vec2 parameter)
		{
			if (std::isfinite(parameter.x) && std::isfinite(parameter.y))
				return true;
			BS_CORE_ERROR("Animator '{}': blend space parameter must be finite, got ({}, {})", entity.GetName(), parameter.x, parameter.y);
			return false;
		}

		// Advances a clip clock (or a blend space's phase, of length 1): wraps when looping, holds at the end otherwise.
		double WrapClock(double time, double length, bool looping)
		{
			return looping && length > 0 ? std::fmod(time, length) : std::min(time, length);
		}

		// Smoothing time constant of MatchVelocity's measured speed (s): long enough to hide fixed-step jitter
		// in where physics leaves a character, short enough to follow a start or stop within a few frames.
		constexpr double VelocitySmoothingSeconds = 0.1;
		// A horizontal move faster than this (units/s) between two updates is a teleport (respawn, Scene.Load, a script
		// placing the entity), not movement, so it does not spike MatchVelocity. Physics moves bodies once per fixed
		// step, so a move is measured over at least one step: at high frame rates most frames see none.
		constexpr double TeleportSpeed = 100.0;
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

	void AnimationSystem::OnDestroy(entt::registry& registry, entt::entity handle)
	{
		m_EmptyMeshes.erase(handle);
		std::erase_if(m_Playback, [handle](const auto& entry) { return entry.second.Handle == handle; });
		if (const auto* id = registry.try_get<IDComponent>(handle))
		{
			m_UnplayableReported.erase(id->ID);
			std::erase_if(m_BoneProblems, [id](const auto& entry) { return entry.first.first == id->ID; });
		}
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
				// Clip may name a blend space, played at parameter (0, 0); one that cannot play yet is retried every
				// update.
				if (FindSpace(*component, component->Clip))
					ResolveSpace(playback, playback.Space.emplace(SpaceState{ .Name = component->Clip }), *component);
				else
				{
					BS_CORE_ERROR("Animator '{}': clip '{}' not found in '{}'", entity.GetName(), component->Clip, component->Mesh);
					playback.PendingStart = component->Clip;
				}
			}
			playback.Playing = component->PlayOnStart && (playback.Clip >= 0 || playback.Space);
			Evaluate(playback);
		}
		return &playback;
	}

	AnimationSystem::Playback* AnimationSystem::EnsurePlayable(Entity entity)
	{
		if (auto* playback = Ensure(entity))
		{
			m_UnplayableReported.erase(entity.GetUUID());
			return playback;
		}
		if (!entity)
			return nullptr;
		std::string reason = DescribeUnplayable(entity);
		std::string& reported = m_UnplayableReported[entity.GetUUID()];
		if (reported != reason)
		{
			BS_CORE_ERROR("Animator '{}': cannot animate: {}", entity.GetName(), reason);
			reported = std::move(reason);
		}
		return nullptr;
	}

	std::string AnimationSystem::DescribeUnplayable(Entity entity)
	{
		const auto* component = entity ? entity.TryGetComponent<AnimatorComponent>() : nullptr;
		if (!component)
			return "it has no Animator";
		if (component->Mesh.empty())
			return "its Animator Mesh is empty";
		if (!AssetManager::GetMesh(component->Mesh))
			return fmt::format("model '{}' could not be loaded: {}", component->Mesh, AssetManager::GetError(component->Mesh));
		return fmt::format("model '{}' has extra influence sets the Animator cannot animate", component->Mesh);
	}

	int32_t AnimationSystem::ResolveSpaceState(const MeshSource& source, const AnimatorComponent& component, SpaceState& state, std::string& outProblem)
	{
		outProblem.clear();
		state.Resolved = false;
		const AnimationBlendSpace* space = FindSpace(component, state.Name);
		if (!space)
		{
			outProblem = fmt::format("blend space '{}' not found", state.Name);
			return -1;
		}
		if (space->Samples.empty())
		{
			outProblem = fmt::format("blend space '{}' has no samples", space->Name);
			return -1;
		}
		ComputeBlendSpaceWeights(*space, state.Parameter, m_Weights);
		state.Samples.resize(space->Samples.size());
		std::vector<std::string> missing;
		float total = 0;
		for (size_t i = 0; i < space->Samples.size(); i++)
		{
			const AnimationBlendSample& sample = space->Samples[i];
			SpaceSample& resolved = state.Samples[i];
			// C++ can write a non-finite Speed (the registry rejects it); it counts as unknown.
			resolved = { FindClip(source, sample.Clip), m_Weights[i], std::isfinite(sample.Speed) && sample.Speed > 0 ? sample.Speed : 0.0f };
			if (resolved.Clip < 0)
			{
				const std::string name = sample.Clip.empty() ? "(first clip: the model has none)" : fmt::format("'{}'", sample.Clip);
				if (std::ranges::find(missing, name) == missing.end())
					missing.push_back(name);
				resolved.Weight = 0;
			}
			total += resolved.Weight;
		}
		if (!missing.empty())
			outProblem = fmt::format("blend space '{}': sample clips not found: {}", space->Name, fmt::join(missing, ", "));
		if (!(total > 0))
		{
			if (outProblem.empty())
				outProblem = fmt::format("blend space '{}' has no weight at ({}, {})", space->Name, state.Parameter.x, state.Parameter.y);
			return -1;
		}
		state.Duration = 0;
		float heaviest = 0;
		int32_t dominant = -1;
		for (SpaceSample& sample : state.Samples)
		{
			sample.Weight /= total;
			if (sample.Clip < 0)
				continue;
			state.Duration += static_cast<double>(sample.Weight) * source.Animations[sample.Clip].Duration;
			if (sample.Weight > heaviest)
			{
				heaviest = sample.Weight;
				dominant = sample.Clip;
			}
		}
		state.Resolved = true;
		return dominant;
	}

	bool AnimationSystem::ResolveSpace(Playback& playback, SpaceState& state, const AnimatorComponent& component)
	{
		std::string problem;
		const int32_t dominant = ResolveSpaceState(*playback.Source, component, state, problem);
		if (problem != state.Problem)
		{
			if (!problem.empty())
				BS_CORE_ERROR("Animator '{}': {}", playback.EntityName, problem);
			state.Problem = std::move(problem);
		}
		if (!state.Resolved)
			return false;
		playback.Clip = dominant;
		playback.Time = state.Phase * state.Duration;
		return true;
	}

	void AnimationSystem::Evaluate(Playback& playback)
	{
		if (playback.Space && !playback.Rest)
		{
			// Every clip at the same phase of its own duration, weighted by the sample weights.
			const SpaceState& state = *playback.Space;
			float total = 0;
			if (state.Resolved)
			{
				for (const SpaceSample& sample : state.Samples)
				{
					if (sample.Clip < 0 || sample.Weight <= 0)
						continue;
					const AnimationClip& clip = playback.Source->Animations[sample.Clip];
					AnimationLocalPose pose = SampleAnimation(*playback.Source, &clip, static_cast<float>(state.Phase * clip.Duration));
					total += sample.Weight;
					if (total > sample.Weight)
						BlendAnimationPose(pose, playback.Local, sample.Weight / total);
					playback.Local = std::move(pose);
				}
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
		{
			BS_CORE_ERROR("AnimationSystem: ignoring a time step of {} s (must be nonnegative and finite)", seconds);
			return;
		}
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
				if (playback->LastPosition)
				{
					const glm::vec3 delta = position - *playback->LastPosition;
					const double distance = glm::length(glm::vec2(delta.x, delta.z));
					const double blend = 1.0 - std::exp(-static_cast<double>(seconds) / VelocitySmoothingSeconds);
					const double window = std::max(static_cast<double>(seconds), static_cast<double>(m_Scene.GetFixedTimestep()));
					if (std::isfinite(distance) && distance <= TeleportSpeed * window)
						playback->MeasuredSpeed += (distance / seconds - playback->MeasuredSpeed) * blend;
				}
				playback->LastPosition = position;
			}
			// Editing BlendSpaces does not reset playback, so a start space added after the Clip was set is looked
			// for until the Animator plays something else.
			if (!playback->PendingStart.empty() && playback->Clip < 0 && !playback->Space && !playback->Rest)
			{
				if (FindSpace(component, playback->PendingStart))
				{
					ResolveSpace(*playback, playback->Space.emplace(SpaceState{ .Name = playback->PendingStart }), component);
					playback->Playing = component.PlayOnStart;
					playback->PendingStart.clear();
				}
			}
			if (!playback->Playing)
				continue;
			if (playback->Space)
			{
				// Holds the pose while the space cannot play, and resumes once it can.
				if (!ResolveSpace(*playback, *playback->Space, component))
					continue;
			}
			else if (playback->Clip < 0)
				continue;
			const bool invalidSpeed = !std::isfinite(component.Speed) || component.Speed < 0 || component.Speed > MaxAnimatorSpeed;
			if (invalidSpeed && !playback->InvalidSpeed)
				BS_CORE_WARN("Animator '{}': invalid Speed {}; clamping to [0, {}]", entity.GetName(), component.Speed, MaxAnimatorSpeed);
			playback->InvalidSpeed = invalidSpeed;
			const double plainSpeed = std::isfinite(component.Speed) ? std::clamp(component.Speed, 0.0f, MaxAnimatorSpeed) : 0;
			double speed = plainSpeed;
			if (playback->Space && component.MatchVelocity)
			{
				// One rate for the whole space: the moving samples' share (Speed > 0) plays as fast as the entity
				// really moves relative to their weighted Speed, the rest (idle, unknown) at the plain Speed, so the
				// rate eases in from a standstill.
				double moving = 0;
				double weightedSpeed = 0;
				for (const SpaceSample& sample : playback->Space->Samples)
				{
					if (sample.Speed <= 0 || sample.Weight <= 0)
						continue;
					moving += sample.Weight;
					weightedSpeed += static_cast<double>(sample.Weight) * sample.Speed;
				}
				if (moving > 0)
				{
					const double sampleSpeed = weightedSpeed / moving;
					speed = std::clamp(plainSpeed * ((1.0 - moving) + moving * playback->MeasuredSpeed / sampleSpeed), 0.0, static_cast<double>(MaxAnimatorSpeed));
				}
			}
			const double advance = static_cast<double>(seconds) * speed;
			const UUID id = entity.GetUUID();
			size_t budget = MaxEventsPerFrame;
			if (playback->Space)
				AdvanceSpace(*playback, *playback->Space, component, advance, id, budget);
			else
			{
				// A two-clip blend's second clip fires only while it shows; a crossfade's target always does, from its
				// first frame.
				if (playback->BlendWeight > 0 || playback->FadeDuration > 0)
					CollectEvents(*playback, component, playback->Clip, playback->Time, advance, id, budget);
				playback->Time = WrapClock(playback->Time + advance, playback->Source->Animations[playback->Clip].Duration, component.Looping);
			}
			if (playback->Outgoing >= 0)
			{
				// The outgoing clip is never a blend space sample, so it keeps the plain Speed. It stops firing where the
				// fade ends, even within this update.
				const double outgoingRate = playback->Space ? plainSpeed : speed;
				const double outgoingAdvance = static_cast<double>(seconds) * outgoingRate;
				const double shown = playback->FadeDuration > 0 ? std::min(static_cast<double>(seconds), playback->FadeDuration - playback->FadeTime) * outgoingRate : outgoingAdvance;
				if (playback->BlendWeight < 1)
					CollectEvents(*playback, component, playback->Outgoing, playback->OutgoingTime, shown, id, budget);
				playback->OutgoingTime = WrapClock(playback->OutgoingTime + outgoingAdvance, playback->Source->Animations[playback->Outgoing].Duration, component.Looping);
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
			const bool finished = playback->Space ? playback->Space->Phase >= 1 : playback->Time >= playback->Source->Animations[playback->Clip].Duration;
			if (!component.Looping && finished && playback->FadeDuration == 0 &&
				(playback->Outgoing < 0 || playback->OutgoingTime >= playback->Source->Animations[playback->Outgoing].Duration))
				playback->Playing = false;
			Evaluate(*playback);
		}
		UpdateAttachments();
	}

	void AnimationSystem::AdvanceSpace(Playback& playback, SpaceState& state, const AnimatorComponent& component, double advance, UUID id, size_t& budget)
	{
		if (state.Duration <= 0)
		{
			// Static poses have no cycle: a non-looping space of them is done at once.
			if (!component.Looping && advance > 0)
				state.Phase = 1;
			return;
		}
		// The heaviest sample alone fires events: the clips are in step, so each would fire its own copy.
		const double phaseAdvance = advance / state.Duration;
		const double dominantDuration = playback.Source->Animations[playback.Clip].Duration;
		CollectEvents(playback, component, playback.Clip, state.Phase * dominantDuration, phaseAdvance * dominantDuration, id, budget);
		state.Phase = WrapClock(state.Phase + phaseAdvance, 1.0, component.Looping);
		playback.Time = state.Phase * state.Duration;
	}

	void AnimationSystem::CollectEvents(Playback& playback, const AnimatorComponent& component, int32_t clip, double from, double advance, UUID id, size_t& budget)
	{
		if (advance <= 0 || component.Events.empty())
			return;
		const AnimationClip& animation = playback.Source->Animations[clip];
		const double duration = animation.Duration;
		m_Markers.clear();
		for (const AnimationEvent& event : component.Events)
			if ((event.Clip.empty() || event.Clip == animation.Name) && event.Time >= 0 && event.Time <= duration)
				m_Markers.push_back(&event);
		if (m_Markers.empty())
			return;
		std::ranges::stable_sort(m_Markers, {}, &AnimationEvent::Time);
		// Fires the markers in [start, end), or [start, end] when `closed` (the clip's end is reached); false once
		// the budget is spent.
		auto fire = [&](double start, double end, bool closed) {
			for (const AnimationEvent* marker : m_Markers)
			{
				if (marker->Time < start || marker->Time > end || (!closed && marker->Time == end))
					continue;
				if (budget == 0)
				{
					if (!playback.EventLimitLogged)
						BS_CORE_WARN("Animator '{}': more than {} animation events in one frame; dropping the rest", playback.EntityName, MaxEventsPerFrame);
					playback.EventLimitLogged = true;
					return false;
				}
				budget--;
				m_PendingEvents.push_back({ id, marker->Name, animation.Name });
			}
			return true;
		};
		if (component.Looping && duration > 0)
		{
			// The same arithmetic as the clock (fmod of from + advance), so the windows meet where the clock lands
			// (for a blend space, whose clock is its phase, up to rounding).
			// Every wrap fires the clip's tail (end included) and restarts at 0. Each full loop fires every marker,
			// so the budget also bounds the loop over many wraps.
			const double end = from + advance;
			if (end < duration)
			{
				fire(from, end, false);
				return;
			}
			if (!fire(from, duration, true))
				return;
			// Loops past the budget cannot fire anything.
			const auto loops = static_cast<size_t>(std::min(std::floor(end / duration) - 1, static_cast<double>(MaxEventsPerFrame)));
			for (size_t loop = 0; loop < loops; loop++)
				if (!fire(0, duration, true))
					return;
			fire(0, std::fmod(end, duration), false);
		}
		else if (from < duration)
		{
			const double to = from + advance;
			fire(from, std::min(to, duration), to >= duration);
		}
	}

	void AnimationSystem::UpdateAttachments()
	{
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
			visit(GetBoneAttachmentTarget(entity));
			visit(entity.GetParent());
			visiting.pop_back();
			return depths[entity] = cyclic.contains(entity) ? -1 : depth;
		};
		std::vector<std::pair<int32_t, Entity>> ordered;
		for (auto handle : m_Scene.GetAllEntitiesWith<BoneAttachmentComponent>())
		{
			const Entity entity(handle, &m_Scene);
			ordered.emplace_back(depthOf(entity), entity);
		}
		std::ranges::stable_sort(ordered, {}, &std::pair<int32_t, Entity>::first);

		std::unordered_map<UUID, std::string> problems;
		for (const auto& [depth, entity] : ordered)
		{
			const auto& attachment = entity.GetComponent<BoneAttachmentComponent>();
			const Entity target = GetBoneAttachmentTarget(entity);
			std::string problem;
			glm::mat4 bone(1.0f);
			if (depth < 0)
				problem = "its attachments form a cycle";
			else if (!target)
				problem = attachment.Target != 0 ? "Target entity not found" : "no Target and no parent";
			else if (target == entity)
				problem = "it cannot attach to itself";
			// The target would move with the entity, so every frame would add the offset again.
			else if (m_Scene.IsDescendantOf(target, entity))
				problem = fmt::format("its Target '{}' is one of its descendants", target.GetName());
			else if (!GetBoneTransform(target, attachment.Joint, bone, problem))
				problem = fmt::format("Target '{}': {}", target.GetName(), problem);
			// SetWorldTransform inverts the parent's world transform and decomposes the result: a zero scale above the
			// entity, or in the target, joint or offset, has no Transform.
			glm::mat4 world(1.0f);
			const auto singular = [](const glm::mat4& matrix) {
				const glm::mat3 basis(matrix);
				const float determinant = glm::determinant(basis);
				return !std::isfinite(determinant) || !(glm::length(basis[0]) > 1e-6f && glm::length(basis[1]) > 1e-6f && glm::length(basis[2]) > 1e-6f) || determinant == 0.0f;
			};
			if (problem.empty())
			{
				world = m_Scene.GetWorldTransform(target) * bone * Math::ComposeTransform(attachment.Translation, attachment.Rotation, attachment.Scale);
				if (const Entity parent = entity.GetParent(); parent && singular(m_Scene.GetWorldTransform(parent)))
					problem = fmt::format("its parent '{}' has a zero (or non-finite) world scale", parent.GetName());
				else if (singular(world))
					problem = "its world transform would have a zero (or non-finite) scale";
			}
			if (!problem.empty())
			{
				auto previous = m_AttachmentProblems.find(entity.GetUUID());
				if (previous == m_AttachmentProblems.end() || previous->second != problem)
					BS_CORE_ERROR("BoneAttachment '{}': {}", entity.GetName(), problem);
				problems[entity.GetUUID()] = std::move(problem);
				continue;
			}
			m_Scene.SetWorldTransform(entity, world);
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

	void AnimationSystem::BeginTransition(Playback& playback, float seconds)
	{
		playback.Snapshot = playback.Local;
		playback.Outgoing = playback.BlendWeight >= 1 && !playback.Rest && !playback.Space ? playback.Clip : -1;
		playback.OutgoingTime = playback.Time;
		playback.FadeTime = 0;
		playback.FadeDuration = seconds;
		playback.BlendWeight = 0;
	}

	void AnimationSystem::Start(Playback& playback, int32_t clip)
	{
		playback.Clip = clip;
		playback.Time = 0;
		playback.Playing = true;
		playback.Rest = false;
		playback.EventLimitLogged = false;
		Evaluate(playback);
	}

	bool AnimationSystem::Play(Entity entity, const std::string& clip)
	{
		auto* playback = EnsurePlayable(entity);
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
		playback->Space.reset();
		Start(*playback, selected);
		return true;
	}

	bool AnimationSystem::CrossFade(Entity entity, const std::string& clip, float seconds)
	{
		if (!CheckSeconds(entity, seconds, "animation seconds"))
			return false;
		if (seconds == 0)
			return Play(entity, clip);
		auto* playback = EnsurePlayable(entity);
		if (!playback)
			return false;
		const int32_t selected = FindClip(*playback->Source, clip);
		if (selected < 0)
		{
			BS_CORE_ERROR("Animator '{}': crossfade clip '{}' not found", entity.GetName(), clip);
			return false;
		}
		BeginTransition(*playback, seconds);
		playback->Space.reset();
		Start(*playback, selected);
		return true;
	}

	bool AnimationSystem::Blend(Entity entity, const std::string& first, const std::string& second, float weight)
	{
		if (!std::isfinite(weight) || weight < 0 || weight > 1)
		{
			BS_CORE_ERROR("Animator '{}': blend weight must be finite and in [0, 1], got {}", entity.GetName(), weight);
			return false;
		}
		auto* playback = EnsurePlayable(entity);
		if (!playback)
			return false;
		const int32_t a = FindClip(*playback->Source, first);
		const int32_t b = FindClip(*playback->Source, second);
		if (a < 0 || b < 0)
		{
			BS_CORE_ERROR("Animator '{}': blend clip '{}' not found", entity.GetName(), a < 0 ? first : second);
			return false;
		}
		// Updating a locomotion blend's weight must not restart either clock.
		if (playback->Outgoing != a || playback->Clip != b || playback->FadeDuration > 0 || playback->Space)
		{
			playback->Time = playback->OutgoingTime = 0;
			playback->EventLimitLogged = false;
		}
		playback->Space.reset();
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

	bool AnimationSystem::PlayBlendSpace(Entity entity, const std::string& space, glm::vec2 parameter, float fadeSeconds)
	{
		if (!CheckParameter(entity, parameter) || !CheckSeconds(entity, fadeSeconds, "fadeSeconds"))
			return false;
		auto* playback = EnsurePlayable(entity);
		if (!playback)
			return false;
		const auto& component = entity.GetComponent<AnimatorComponent>();
		const bool finished = playback->Space && playback->Space->Phase >= 1 && !component.Looping;
		if (playback->Space && playback->Space->Name == space && playback->Playing && !finished)
			return SetBlendSpaceParameter(entity, parameter);
		SpaceState state{ .Name = space, .Parameter = parameter };
		const int32_t dominant = ResolveSpaceState(*playback->Source, component, state, state.Problem);
		// Scripts often call this every frame; a problem is logged when it appears or changes.
		if (state.Problem != playback->PlayProblem && !state.Problem.empty())
			BS_CORE_ERROR("Animator '{}': {}", entity.GetName(), state.Problem);
		playback->PlayProblem = state.Problem;
		if (!state.Resolved)
			return false;
		if (fadeSeconds > 0)
			BeginTransition(*playback, fadeSeconds);
		else
			ClearTransition(*playback);
		playback->Space = std::move(state);
		Start(*playback, dominant);
		return true;
	}

	bool AnimationSystem::SetBlendSpaceParameter(Entity entity, glm::vec2 parameter)
	{
		if (!CheckParameter(entity, parameter))
			return false;
		auto* playback = EnsurePlayable(entity);
		if (!playback)
			return false;
		if (!playback->Space)
		{
			BS_CORE_ERROR("Animator '{}': no blend space is playing", entity.GetName());
			return false;
		}
		playback->Space->Parameter = parameter;
		if (!ResolveSpace(*playback, *playback->Space, entity.GetComponent<AnimatorComponent>()))
			return false;
		Evaluate(*playback);
		return true;
	}

	std::optional<glm::vec2> AnimationSystem::GetBlendSpaceParameter(Entity entity)
	{
		auto* playback = Ensure(entity);
		if (!playback || !playback->Space)
			return std::nullopt;
		return playback->Space->Parameter;
	}

	std::vector<std::pair<std::string, float>> AnimationSystem::GetBlendSpaceWeights(Entity entity)
	{
		std::vector<std::pair<std::string, float>> weights;
		auto* playback = Ensure(entity);
		if (!playback || !playback->Space || !playback->Space->Resolved)
			return weights;
		for (const SpaceSample& sample : playback->Space->Samples)
		{
			if (sample.Clip < 0)
				continue;
			const std::string& name = playback->Source->Animations[sample.Clip].Name;
			auto it = std::ranges::find(weights, name, &std::pair<std::string, float>::first);
			if (it == weights.end())
				weights.emplace_back(name, sample.Weight);
			else
				it->second += sample.Weight;
		}
		return weights;
	}

	void AnimationSystem::Pause(Entity entity, bool paused)
	{
		if (auto* playback = Ensure(entity))
			playback->Playing = !paused && (playback->Clip >= 0 || playback->Space) && !playback->Rest;
	}

	void AnimationSystem::Stop(Entity entity)
	{
		if (auto* playback = Ensure(entity))
		{
			playback->Playing = false;
			playback->Time = 0;
			playback->Rest = true;
			playback->Space.reset();
			ClearTransition(*playback);
			Evaluate(*playback);
		}
	}

	bool AnimationSystem::Seek(Entity entity, float seconds)
	{
		if (!CheckSeconds(entity, seconds, "animation seconds"))
			return false;
		auto* playback = EnsurePlayable(entity);
		if (!playback)
			return false;
		// Seeking a space that cannot play would evaluate it as the rest pose instead of holding the pose.
		if (playback->Space && !playback->Space->Resolved)
		{
			BS_CORE_ERROR("Animator '{}': cannot seek: {}", entity.GetName(), playback->Space->Problem);
			return false;
		}
		if (playback->Clip < 0)
		{
			BS_CORE_ERROR("Animator '{}': cannot seek: no clip is playing", entity.GetName());
			return false;
		}
		if (playback->Space)
		{
			SpaceState& state = *playback->Space;
			state.Phase = state.Duration > 0 ? std::min(seconds / state.Duration, 1.0) : 0.0;
			playback->Time = state.Phase * state.Duration;
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
		if (!playback)
			return 0;
		if (playback->Space)
			return static_cast<float>(playback->Space->Duration);
		return playback->Clip >= 0 ? playback->Source->Animations[playback->Clip].Duration : 0;
	}

	bool AnimationSystem::IsPlaying(Entity entity)
	{
		auto* playback = Ensure(entity);
		return playback && playback->Playing && (!playback->Space || playback->Space->Resolved);
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

	bool AnimationSystem::GetBoneTransform(Entity entity, const std::string& joint, glm::mat4& outTransform, std::string& outError)
	{
		auto* playback = Ensure(entity);
		if (!playback)
		{
			outError = DescribeUnplayable(entity);
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

	void AnimationSystem::ReportBoneProblem(Entity entity, const std::string& joint, const std::string& error)
	{
		const std::pair<UUID, std::string> key{ entity.GetUUID(), joint };
		if (error.empty())
		{
			m_BoneProblems.erase(key);
			return;
		}
		std::string& reported = m_BoneProblems[key];
		if (reported != error)
			BS_CORE_ERROR("GetBoneTransform on '{}': {}", entity.GetName(), error);
		reported = error;
	}

	std::vector<AnimationEventRecord> AnimationSystem::TakeEvents()
	{
		return std::exchange(m_PendingEvents, {});
	}
}
