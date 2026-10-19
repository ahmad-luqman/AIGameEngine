#include "Basalt/Animation/AnimationSystem.h"

#include "Basalt/Animation/AnimatedModel.h"
#include "Basalt/Animation/AnimationPose.h"
#include "Basalt/Animation/BlendSpace.h"
#include "Basalt/Animation/InverseKinematics.h"
#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Core/Profiler.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Physics/PhysicsWorld.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <spdlog/fmt/ranges.h>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace Basalt {

	namespace {
		const char* ParameterTypeName(AnimationParameterType type)
		{
			switch (type)
			{
				case AnimationParameterType::Float:
					return "Float";
				case AnimationParameterType::Bool:
					return "Bool";
				case AnimationParameterType::Trigger:
					return "Trigger";
			}
			return "Unknown";
		}
		bool ConditionHolds(const AnimationCondition& condition, float value)
		{
			switch (condition.Comparison)
			{
				case AnimationComparison::Greater:
					return value > condition.Value;
				case AnimationComparison::Less:
					return value < condition.Value;
				case AnimationComparison::Equal:
					return value == condition.Value;
				case AnimationComparison::NotEqual:
					return value != condition.Value;
			}
			return false;
		}
		float RootYaw(const glm::mat4& frame)
		{
			return std::atan2(frame[2].x, frame[2].z);
		}
		glm::mat4 YawTransform(glm::vec3 position, float yaw)
		{
			return Math::ComposeTransform(position, glm::vec3(0, yaw, 0), glm::vec3(1));
		}
		// Swing-twist decomposition around model Y remains stable when the authored +Z points vertically.
		// A 180 degree swing, or one very close to it, has no reliable twist; callers keep the authored pose and skip extraction then.
		bool HorizontalFrame(const glm::mat4& frame, glm::mat4& out)
		{
			glm::vec3 position, scale;
			glm::quat rotation;
			if (!Math::DecomposeTransform(frame, position, rotation, scale) || rotation.w * rotation.w + rotation.y * rotation.y < 1e-10f)
				return false;
			out = YawTransform({ position.x, 0, position.z }, 2 * std::atan2(rotation.y, rotation.w));
			return Math::IsFinite(out);
		}
		// Correction is rigid, so authored TRS roots stay representable, including zero-scale helpers.
		// Used both to preflight extraction and to commit the visible pose, keeping their validity in sync.
		bool CorrectRootPose(const MeshSource& source, AnimationLocalPose& pose, const glm::mat4& correction)
		{
			for (size_t i = 0; i < pose.Nodes.size(); i++)
				if (source.Nodes[i].Parent < 0)
				{
					auto& root = pose.Nodes[i];
					const glm::mat4 corrected = correction * Math::ComposeTransform(root.Translation, root.Rotation, root.Scale);
					if (!Math::IsFinite(corrected) || !Math::DecomposeAffineTransform(corrected, root.Translation, root.Rotation, root.Scale))
						return false;
				}
			return true;
		}
		bool HorizontalTransform(const MeshSource& source, const AnimationClip* clip, float time, int32_t node, glm::mat4& out, const glm::mat4* reference = nullptr)
		{
			auto pose = SampleAnimation(source, clip, time);
			std::vector<glm::mat4> globals;
			if (node < 0 || !ComposeAnimationPose(source, pose, globals) || static_cast<size_t>(node) >= globals.size() || !HorizontalFrame(globals[node], out))
				return false;
			return !reference || CorrectRootPose(source, pose, *reference * glm::inverse(out));
		}
		float BlendRootYaw(float from, float to, float weight)
		{
			return from + std::remainder(to - from, glm::two_pi<float>()) * weight;
		}

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
		// Forgets the script-call problems reported for one entity.
		void EraseProblemsOf(std::map<std::pair<UUID, std::string>, std::string>& problems, UUID id)
		{
			std::erase_if(problems, [id](const auto& entry) { return entry.first.first == id; });
		}
	}

	AnimationSystem::AnimationSystem(Scene& scene)
		: m_Scene(scene)
	{
		m_Scene.GetRegistry().on_destroy<AnimatorComponent>().connect<&AnimationSystem::OnDestroy>(*this);
		m_Scene.GetRegistry().on_destroy<InverseKinematicsComponent>().connect<&AnimationSystem::OnIKDestroy>(*this);
		m_Scene.GetRegistry().on_destroy<SpringBoneComponent>().connect<&AnimationSystem::OnSpringDestroy>(*this);
		Update(0);
	}

	AnimationSystem::~AnimationSystem()
	{
		m_Scene.GetRegistry().on_destroy<AnimatorComponent>().disconnect<&AnimationSystem::OnDestroy>(*this);
		m_Scene.GetRegistry().on_destroy<InverseKinematicsComponent>().disconnect<&AnimationSystem::OnIKDestroy>(*this);
		m_Scene.GetRegistry().on_destroy<SpringBoneComponent>().disconnect<&AnimationSystem::OnSpringDestroy>(*this);
	}

	void AnimationSystem::OnDestroy(entt::registry& registry, entt::entity handle)
	{
		m_EmptyMeshes.erase(handle);
		std::erase_if(m_Playback, [handle](const auto& entry) { return entry.second.Handle == handle; });
		if (const auto* id = registry.try_get<IDComponent>(handle))
		{
			m_UnplayableReported.erase(id->ID);
			EraseProblemsOf(m_LayerProblems, id->ID);
			EraseProblemsOf(m_IKCallProblems, id->ID);
			EraseProblemsOf(m_BoneProblems, id->ID);
		}
	}

	void AnimationSystem::OnIKDestroy(entt::registry& registry, entt::entity handle)
	{
		std::erase_if(m_IK, [handle](const auto& entry) { return entry.second.Handle == handle; });
		if (const auto* id = registry.try_get<IDComponent>(handle))
			EraseProblemsOf(m_IKCallProblems, id->ID);
		// The pose returns to the animation alone. Spring bones were solved on top of the IK solution, so they go too
		// until they solve again from the animation.
		for (auto& [id, playback] : m_Playback)
		{
			if (playback.Handle == handle && (!playback.IKOverlay.empty() || !playback.SpringOverlay.empty()))
			{
				playback.IKOverlay.clear();
				playback.SpringOverlay.clear();
				Finish(playback);
			}
		}
	}

	void AnimationSystem::DropOverlay(Playback& playback, Overlay overlay)
	{
		if ((playback.*overlay).empty())
			return;
		(playback.*overlay).clear();
		Finish(playback);
	}

	void AnimationSystem::StoreOverlay(Playback& playback, Overlay overlay, std::vector<uint32_t>& nodes, const AnimationLocalPose& pose, const std::vector<glm::mat4>& globals)
	{
		std::ranges::sort(nodes);
		nodes.erase(std::ranges::unique(nodes).begin(), nodes.end());
		auto& entries = playback.*overlay;
		entries.clear();
		for (uint32_t node : nodes)
			entries.push_back({ node, pose.Nodes[node] });
		playback.Globals = globals;
		playback.OverlayDropped = false;
	}

	void AnimationSystem::ReportProblem(std::string& reported, std::string problem, Entity entity, const char* owner, const std::string* chain)
	{
		if (!problem.empty() && problem != reported)
		{
			if (chain)
				BS_CORE_WARN("{} '{}': chain '{}': {}", owner, entity.GetName(), *chain, problem);
			else
				BS_CORE_WARN("{} '{}': {}", owner, entity.GetName(), problem);
		}
		reported = std::move(problem);
	}

	std::string AnimationSystem::JoinProblems(const std::vector<std::string>& problems)
	{
		return fmt::format("{}", fmt::join(problems, "; "));
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
		const auto graph = AssetManager::GetAnimationGraph(component->StateMachine);
		if (inserted || playback.Graph != graph || playback.ConfiguredGraph != component->StateMachine || playback.Handle != static_cast<entt::entity>(entity) || playback.Source != source || playback.ConfiguredClip != component->Clip)
		{
			playback = {};
			playback.Handle = entity;
			playback.EntityName = entity.GetName();
			playback.Source = source;
			playback.ConfiguredClip = component->Clip;
			playback.ConfiguredGraph = component->StateMachine;
			playback.Graph = graph;
			playback.Looping = component->Looping;
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
			if (!component->StateMachine.empty())
			{
				playback.Playing = false;
				playback.Clip = -1;
				playback.Space.reset();
				playback.PendingStart.clear();
				if (!graph)
					BS_CORE_ERROR("Animator '{}': cannot load state machine '{}': {}", entity.GetName(), component->StateMachine, AssetManager::GetError(component->StateMachine));
				else
				{
					for (const auto& parameter : graph->Parameters)
					{
						playback.Parameters.push_back(parameter.Default);
						playback.ParameterProblems.emplace_back();
					}
					playback.StateProblems.resize(graph->States.size());
					playback.Control = GraphControl::Active;
					EnterState(playback, graph->InitialStateIndex, *component, 0);
					playback.Paused = !component->PlayOnStart;
					playback.Playing = playback.Playing && component->PlayOnStart;
				}
			}
			Evaluate(playback);
		}
		if (ResolveRootNode(playback, *component))
			Evaluate(playback);
		if (playback.Control != GraphControl::Active)
		{
			playback.Looping = component->Looping;
			if (playback.FadeDuration == 0)
			{
				playback.Outgoing.Looping = component->Looping;
				playback.Outgoing.RootMotion = playback.Outgoing.Clip >= 0 && component->RootMotion;
			}
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
					if (playback.RootMotion)
						StripRoot(playback, pose, sample.Clip);
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
		if (playback.RootMotion && !playback.Rest && !playback.Space)
			StripRoot(playback, playback.Local, playback.Clip);
		if (playback.BlendWeight < 1 && !playback.Rest)
		{
			auto from = playback.Outgoing.Clip >= 0 ? SampleAnimation(*playback.Source, &playback.Source->Animations[playback.Outgoing.Clip], static_cast<float>(playback.Outgoing.Time)) : playback.Snapshot;
			if (playback.Outgoing.Clip >= 0)
			{
				if (playback.Outgoing.RootMotion)
					StripRoot(playback, from, playback.Outgoing.Clip);
			}
			BlendAnimationPose(playback.Local, from, playback.BlendWeight);
		}
		Finish(playback);
	}

	void AnimationSystem::Finish(Playback& playback)
	{
		const MeshSource& source = *playback.Source;
		playback.Pose = playback.Local;
		// The outgoing clip of a fade can still extract root motion.
		const int32_t rootNode = (playback.RootMotion || playback.Outgoing.RootMotion) ? playback.RootNode : -1;
		for (LayerState& layer : playback.Layers)
		{
			const float weight = layer.Weight.Get();
			if (!layer.Resolved || layer.Resolved->Clip < 0 || weight <= 0)
				continue;
			LayerResolution& resolved = *layer.Resolved;
			if (rootNode >= 0 && resolved.RootMaskNode != rootNode)
			{
				// Root motion owns the extraction node and everything above it: a layer moving any of them would move the
				// stripped root again, which the entity already received as displacement.
				resolved.RootMask = resolved.Mask;
				bool animated = false;
				for (int32_t node = rootNode; node >= 0; node = source.Nodes[node].Parent)
				{
					animated = animated || (resolved.RootMask.Weights[node] > 0 && resolved.RootMask.Channels[node] != 0);
					resolved.RootMask.Weights[node] = 0;
				}
				resolved.RootMaskNode = rootNode;
				std::string problem = animated ? fmt::format("clip '{}' animates the root-motion joint '{}' or a node above it, which layers leave to root motion", resolved.Key.Clip, source.Nodes[rootNode].Name) : std::string();
				if (!problem.empty() && problem != layer.RootProblem)
					BS_CORE_WARN("Animator '{}': layer '{}': {}", playback.EntityName, layer.Name, problem);
				layer.RootProblem = std::move(problem);
			}
			const AnimationLayerMask& mask = rootNode >= 0 ? resolved.RootMask : resolved.Mask;
			const AnimationLocalPose sample = SampleAnimation(source, &source.Animations[resolved.Clip], static_cast<float>(layer.Time));
			if (resolved.Key.Blend == AnimationLayerBlend::Additive)
				AddAnimationPose(playback.Pose, sample, resolved.Reference, mask, weight);
			else
				OverrideAnimationPose(playback.Pose, sample, mask, weight);
		}
		const auto composes = [&](const AnimationLocalPose& pose) { return ComposeAnimationPose(source, pose, playback.Globals) && SkinPalettesFinite(source, playback.Globals); };
		bool valid;
		if (playback.IKOverlay.empty() && playback.SpringOverlay.empty())
			valid = composes(playback.Pose);
		else
		{
			AnimationLocalPose pose = playback.Pose;
			// Spring bones solved on top of IK, so their entries (which include IK's changes to the same nodes) go last.
			for (const auto* overlay : { &playback.IKOverlay, &playback.SpringOverlay })
				for (const auto& [node, transform] : *overlay)
				{
					// The overlays are cleared with the playback whenever Source changes.
					BS_CORE_ASSERT(node < pose.Nodes.size());
					pose.Nodes[node] = transform;
				}
			valid = composes(pose);
			if (valid)
				playback.OverlayDropped = false;
			// The IK and spring passes check their own results, so the overlays fail on a pose that changed under them. A
			// pose that fails without them too is reported below, not as an IK problem.
			else if (composes(playback.Pose))
			{
				if (!playback.OverlayDropped)
					BS_CORE_WARN("Animator '{}': the IK and spring bone solution does not fit the newly evaluated pose; showing the pose without them until they solve again", playback.EntityName);
				playback.OverlayDropped = true;
				playback.IKOverlay.clear();
				playback.SpringOverlay.clear();
				valid = true;
			}
		}
		if (!valid)
		{
			if (!playback.InvalidPose)
				BS_CORE_ERROR("Animator '{}': non-finite evaluated pose; displaying rest pose", playback.EntityName);
			playback.Local = SampleAnimation(source, nullptr, 0);
			playback.Pose = playback.Local;
			playback.IKOverlay.clear();
			playback.SpringOverlay.clear();
			ComposeAnimationPose(source, playback.Local, playback.Globals);
		}
		playback.InvalidPose = !valid;
	}

	void AnimationSystem::Update(Timestep timestep, bool runLateUpdate)
	{
		BS_PROFILE_SCOPE("Animation");
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
			else if (!component.MatchVelocity)
			{
				// Re-enabling starts a fresh measurement: the movement while it was off is not one frame's.
				playback->LastPosition.reset();
				playback->MeasuredSpeed = 0;
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
			if (playback->Control == GraphControl::Active && !playback->Paused)
				UpdateGraph(*playback, component);
			// Layers play on while the base holds its pose.
			const bool layered = !component.Layers.empty() || !playback->Layers.empty();
			if (layered)
				AdvanceLayers(*playback, component, playback->Paused ? 0.0 : static_cast<double>(seconds));
			// The base holds its pose while stopped or finished, without a clip, and while its blend space cannot play
			// (it resumes once it can).
			const bool baseAdvances = playback->Playing && (playback->Space ? ResolveSpace(*playback, *playback->Space, component) : playback->Clip >= 0);
			if (!baseAdvances)
			{
				if (layered)
					Finish(*playback);
				continue;
			}
			const bool invalidSpeed = !std::isfinite(component.Speed) || component.Speed < 0 || component.Speed > MaxAnimatorSpeed;
			if (invalidSpeed && !playback->InvalidSpeed)
				BS_CORE_WARN("Animator '{}': invalid Speed {}; clamping to [0, {}]", entity.GetName(), component.Speed, MaxAnimatorSpeed);
			playback->InvalidSpeed = invalidSpeed;
			const double plainSpeed = std::isfinite(component.Speed) ? std::clamp(component.Speed, 0.0f, MaxAnimatorSpeed) : 0;
			double speed = plainSpeed;
			if (playback->Space && component.MatchVelocity && !playback->RootMotion)
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
			const double shown = playback->FadeDuration > 0 ? std::min(static_cast<double>(seconds), playback->FadeDuration - playback->FadeTime) : seconds;
			ResetRootIntervals(*playback);
			const auto motion = FrameRootMotion(*playback, advance, shown, seconds, plainSpeed);
			const UUID id = entity.GetUUID();
			size_t budget = MaxEventsPerFrame;
			if (playback->Space)
				AdvanceSpace(*playback, *playback->Space, component, advance, id, budget);
			else
			{
				// A two-clip blend's second clip fires only while it shows; a crossfade's target always does, from its
				// first frame.
				if (playback->BlendWeight > 0 || playback->FadeDuration > 0)
					CollectEvents(*playback, component, playback->Clip, playback->Time, advance, id, budget, playback->Looping);
				playback->Time = WrapClock(playback->Time + advance, playback->Source->Animations[playback->Clip].Duration, playback->Looping);
			}
			if (playback->Outgoing.Clip >= 0)
			{
				// The outgoing clip is never a blend space sample, so it keeps the plain Speed. It stops firing where the
				// fade ends, even within this update.
				const double outgoingRate = playback->Space ? plainSpeed : speed;
				const double outgoingAdvance = static_cast<double>(seconds) * outgoingRate;
				const double visibleAdvance = shown * outgoingRate;
				if (playback->BlendWeight < 1)
					CollectEvents(*playback, component, playback->Outgoing.Clip, playback->Outgoing.Time, visibleAdvance, id, budget, playback->Outgoing.Looping);
				playback->Outgoing.Time = WrapClock(playback->Outgoing.Time + outgoingAdvance, playback->Source->Animations[playback->Outgoing.Clip].Duration, playback->Outgoing.Looping);
			}
			if (playback->Outgoing.Space)
			{
				auto& outgoing = *playback->Outgoing.Space;
				if (outgoing.Duration > 0)
					outgoing.Phase = WrapClock(outgoing.Phase + static_cast<double>(seconds) * plainSpeed / outgoing.Duration, 1.0, playback->Outgoing.Looping);
			}
			if (playback->FadeDuration > 0)
			{
				playback->FadeTime += seconds;
				playback->BlendWeight = static_cast<float>(std::min(playback->FadeTime / playback->FadeDuration, 1.0));
				if (playback->BlendWeight >= 1)
					ClearTransition(*playback);
			}
			const double cycleDuration = CycleDuration(*playback);
			if (cycleDuration > 0)
				playback->StatePhase += advance / cycleDuration;
			if (motion)
				ApplyRootMotion(entity, *playback, *motion, seconds);
			// A frame without root motion still owns its part of a step that earlier root-motion frames queued for.
			else if (auto* physics = m_Scene.GetPhysicsWorld(); physics && seconds > 0 && entity.HasComponent<CharacterControllerComponent>())
				physics->QueueRootMotionGap(entity, seconds);
			const bool finished = playback->Space ? playback->Space->Phase >= 1 : playback->Time >= playback->Source->Animations[playback->Clip].Duration;
			// Time clamps exactly to Duration; summed phase fractions can finish just below 1.
			if (!playback->Looping && finished && cycleDuration > 0)
				playback->StatePhase = std::max(playback->StatePhase, 1.0);
			if (!playback->Looping && finished && playback->FadeDuration == 0 &&
				(playback->Outgoing.Clip < 0 || playback->Outgoing.Time >= playback->Source->Animations[playback->Outgoing.Clip].Duration))
				playback->Playing = false;
			Evaluate(*playback);
		}
		if (runLateUpdate)
			LateUpdate(timestep);
	}

	void AnimationSystem::LateUpdate(Timestep timestep)
	{
		const float seconds = timestep.GetSeconds();
		// Update already reported an invalid step; attachments still follow the rigs.
		if (std::isfinite(seconds) && seconds >= 0)
		{
			SolveIK(seconds);
			SolveSprings(seconds);
		}
		UpdateAttachments();
	}

	void AnimationSystem::AdvanceSpace(Playback& playback, SpaceState& state, const AnimatorComponent& component, double advance, UUID id, size_t& budget)
	{
		if (state.Duration <= 0)
		{
			// Static poses have no cycle: a non-looping space of them is done at once.
			if (!playback.Looping && advance > 0)
				state.Phase = 1;
			return;
		}
		// The heaviest sample alone fires events: the clips are in step, so each would fire its own copy.
		const double phaseAdvance = advance / state.Duration;
		const double dominantDuration = playback.Source->Animations[playback.Clip].Duration;
		CollectEvents(playback, component, playback.Clip, state.Phase * dominantDuration, phaseAdvance * dominantDuration, id, budget, playback.Looping);
		state.Phase = WrapClock(state.Phase + phaseAdvance, 1.0, playback.Looping);
		playback.Time = state.Phase * state.Duration;
	}

	void AnimationSystem::CollectEvents(Playback& playback, const AnimatorComponent& component, int32_t clip, double from, double advance, UUID id, size_t& budget, bool looping)
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
		if (looping && duration > 0)
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
			// entity, or in the target, joint or offset, has no Transform, and a non-finite value anywhere (a script can
			// write one into a Translation) would spread into the entity's.
			glm::mat4 world(1.0f);
			const auto singular = [](const glm::mat4& matrix) {
				for (int column = 0; column < 4; column++)
					if (glm::any(glm::isnan(matrix[column])) || glm::any(glm::isinf(matrix[column])))
						return true;
				const glm::mat3 basis(matrix);
				const float determinant = glm::determinant(basis);
				return !std::isfinite(determinant) || !(glm::length(basis[0]) > 1e-6f && glm::length(basis[1]) > 1e-6f && glm::length(basis[2]) > 1e-6f) || determinant == 0.0f;
			};
			if (problem.empty())
			{
				world = m_Scene.GetWorldTransform(target) * bone * Math::ComposeTransform(attachment.Translation, attachment.Rotation, attachment.Scale);
				if (const Entity parent = entity.GetParent(); parent && singular(m_Scene.GetWorldTransform(parent)))
					problem = fmt::format("its parent '{}' has a zero scale or a non-finite world transform", parent.GetName());
				else if (singular(world))
					problem = "its world transform would have a zero scale or non-finite values";
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

	void AnimationSystem::ReportRootClipProblem(Playback& playback, int32_t clip, const std::string& problem)
	{
		auto& reported = playback.RootClips[clip].Problem;
		if (reported != problem && !problem.empty())
			BS_CORE_ERROR("Animator '{}': root motion undefined for clip '{}' at node '{}': {}; preserving the authored pose", playback.EntityName,
						  playback.Source->Animations[clip].Name, playback.Source->Nodes[playback.RootNode].Name, problem);
		reported = problem;
	}
	bool AnimationSystem::CanExtractRoot(Playback& playback, int32_t clipIndex)
	{
		if (clipIndex < 0 || playback.RootNode < 0 || !playback.RootReference)
			return false;
		const glm::mat4 reference = *playback.RootReference;
		auto [it, inserted] = playback.RootClips.try_emplace(clipIndex);
		auto& frames = it->second;
		if (inserted)
		{
			const auto& clip = playback.Source->Animations[clipIndex];
			glm::mat4 zero, end;
			frames.Valid = HorizontalTransform(*playback.Source, &clip, 0, playback.RootNode, zero, &reference) &&
						   HorizontalTransform(*playback.Source, &clip, clip.Duration, playback.RootNode, end, &reference);
			if (frames.Valid)
			{
				frames.Cycle = end * glm::inverse(zero);
				frames.Valid = Math::IsFinite(frames.Cycle);
			}
			if (!frames.Valid)
				ReportRootClipProblem(playback, clipIndex, "clip endpoint twist or pose correction is undefined");
		}
		return frames.Valid;
	}
	void AnimationSystem::ResetRootIntervals(Playback& playback)
	{
		for (auto& [clip, frames] : playback.RootClips)
			frames.IntervalValid = true;
	}
	void AnimationSystem::StripRoot(Playback& playback, AnimationLocalPose& pose, int32_t clip)
	{
		if (!CanExtractRoot(playback, clip) || !playback.RootReference || !playback.RootClips.at(clip).IntervalValid || pose.Nodes.empty())
			return;
		std::vector<glm::mat4> globals;
		glm::mat4 horizontal;
		auto fail = [&] { ReportRootClipProblem(playback, clip, "sampled twist or hierarchy is undefined"); };
		if (!ComposeAnimationPose(*playback.Source, pose, globals) || !HorizontalFrame(globals[playback.RootNode], horizontal))
		{
			fail();
			return;
		}
		const glm::mat4 correction = *playback.RootReference * glm::inverse(horizontal);
		auto stripped = pose;
		if (!CorrectRootPose(*playback.Source, stripped, correction))
		{
			fail();
			return;
		}
		pose = std::move(stripped);
	}
	double AnimationSystem::CycleDuration(const Playback& playback)
	{
		return playback.Space ? playback.Space->Duration : (playback.Clip >= 0 ? playback.Source->Animations[playback.Clip].Duration : 0);
	}

	bool AnimationSystem::ResolveRootNode(Playback& playback, const AnimatorComponent& component)
	{
		const bool oldEnabled = playback.RootMotion;
		const auto* state = playback.Control == GraphControl::Active && playback.StateIndex ? &playback.Graph->States[*playback.StateIndex] : nullptr;
		playback.RootMotion = component.RootMotion && (playback.Control != GraphControl::Active || (state && state->RootMotion));
		if (!component.RootMotion)
			playback.Outgoing.RootMotion = false;
		const int32_t node = component.RootMotion && !component.RootJoint.empty() ? FindNode(*playback.Source, component.RootJoint) : -1;
		const bool changed = node != playback.RootNode;
		if (changed)
		{
			playback.RootNode = node;
			playback.RootReference.reset();
			playback.RootClips.clear();
			glm::mat4 reference;
			if (node >= 0 && HorizontalTransform(*playback.Source, nullptr, 0, node, reference))
				playback.RootReference = reference;
		}
		std::string problem;
		if (component.RootMotion && node < 0)
			problem = "RootJoint must name a model node for root motion (got '" + component.RootJoint + "')";
		else if (node >= 0 && !playback.RootReference)
			problem = "root motion undefined at node '" + component.RootJoint + "' in the rest pose; preserving the authored pose";
		if (problem != playback.RootProblem && !problem.empty())
			BS_CORE_ERROR("Animator '{}': {}", playback.EntityName, problem);
		playback.RootProblem = problem;
		return changed || oldEnabled != playback.RootMotion;
	}
	std::optional<glm::mat4> AnimationSystem::SourceRootDelta(Playback& playback, int32_t clip, double time, const std::optional<SpaceState>& space, double advance, bool looping, bool enabled)
	{
		if (!enabled)
			return std::nullopt;
		return space ? SpaceRootDelta(playback, *space, advance, looping) : RootDelta(playback, clip, time, advance, looping);
	}
	std::optional<AnimationSystem::RootMotionFrame> AnimationSystem::FrameRootMotion(Playback& playback, double advance, double shown, double seconds, double plainSpeed)
	{
		auto sample = [&]() -> std::optional<RootMotionFrame> {
			auto incoming = [&](double amount) { return SourceRootDelta(playback, playback.Clip, playback.Time, playback.Space, amount, playback.Looping, playback.RootMotion); };
			auto motion = incoming(advance);
			if (playback.BlendWeight >= 1)
			{
				if (!motion)
					return std::nullopt;
				return RootMotionFrame{ .Delta = *motion };
			}
			glm::mat4 tail(1);
			// The part of the frame the fade covers; the incoming source alone moves the rest (the tail).
			double fading = 1;
			if (shown < seconds && seconds > 0)
			{
				fading = shown / seconds;
				const auto faded = incoming(advance * fading);
				if (motion && faded)
				{
					tail = glm::inverse(*faded) * *motion;
					motion = faded;
				}
				else
					motion.reset();
			}
			const auto& source = playback.Outgoing;
			const auto outgoing = SourceRootDelta(playback, source.Clip, source.Time, source.Space, shown * plainSpeed, source.Looping, source.RootMotion);
			if (!motion && !outgoing)
				return std::nullopt;
			const glm::mat4 from = outgoing.value_or(glm::mat4(1)), to = motion.value_or(glm::mat4(1));
			const double weight = playback.FadeDuration > 0 ? playback.BlendWeight + shown / (2.0 * playback.FadeDuration) : playback.BlendWeight;
			// Each source owns its side of the fade; after the fade ends within this frame (the tail), the incoming one
			// owns all of it.
			const double fadeShare = (motion ? weight : 0.0) + (outgoing ? 1.0 - weight : 0.0);
			const double share = fading * fadeShare + (1.0 - fading) * (motion ? 1.0 : 0.0);
			return RootMotionFrame{ .Delta = YawTransform(glm::mix(glm::vec3(from[3]), glm::vec3(to[3]), static_cast<float>(weight)), BlendRootYaw(RootYaw(from), RootYaw(to), static_cast<float>(weight))) * tail, .Share = static_cast<float>(std::clamp(share, 0.0, 1.0)) };
		};
		const auto motion = sample();
		// A later source may invalidate a clip already used by an earlier source. Recompute with
		// the settled per-clip validity so displacement and pose stripping exclude the same samples.
		const bool failed = std::any_of(playback.RootClips.begin(), playback.RootClips.end(), [](const auto& entry) { return !entry.second.IntervalValid; });
		return failed ? sample() : motion;
	}
	std::optional<glm::mat4> AnimationSystem::SpaceRootDelta(Playback& playback, const SpaceState& space, double advance, bool looping)
	{
		glm::vec3 translation(0);
		float yaw = 0, total = 0;
		bool valid = false;
		for (const auto& sample : space.Samples)
			if (sample.Weight > 0 && sample.Clip >= 0)
			{
				const double duration = playback.Source->Animations[sample.Clip].Duration;
				const auto delta = RootDelta(playback, sample.Clip, space.Phase * duration, space.Duration > 0 ? advance / space.Duration * duration : 0, looping);
				// Unsupported samples retain their authored motion and contribute no entity displacement.
				const auto frame = delta.value_or(glm::mat4(1));
				valid = valid || delta.has_value();
				translation += glm::vec3(frame[3]) * sample.Weight;
				total += sample.Weight;
				yaw = total > sample.Weight ? BlendRootYaw(yaw, RootYaw(frame), sample.Weight / total) : RootYaw(frame);
			}
		return valid ? std::optional(YawTransform(translation, yaw)) : std::nullopt;
	}
	std::optional<glm::mat4> AnimationSystem::RootDelta(Playback& playback, int32_t clipIndex, double from, double advance, bool looping)
	{
		if (!CanExtractRoot(playback, clipIndex) || !playback.RootReference)
			return std::nullopt;
		const glm::mat4 reference = *playback.RootReference;
		const auto& clip = playback.Source->Animations[clipIndex];
		auto& frames = playback.RootClips.at(clipIndex);
		if (!frames.IntervalValid)
			return std::nullopt;
		const double duration = clip.Duration;
		if (duration <= 0 || advance <= 0)
			return glm::mat4(1);
		auto fail = [&]() -> std::optional<glm::mat4> {
			frames.IntervalValid = false;
			ReportRootClipProblem(playback, clipIndex, "sampled twist or hierarchy is undefined");
			return std::nullopt;
		};
		glm::mat4 start, end;
		const double to = from + advance;
		if (!HorizontalTransform(*playback.Source, &clip, static_cast<float>(from), playback.RootNode, start, &reference) ||
			!HorizontalTransform(*playback.Source, &clip, static_cast<float>(looping ? std::fmod(to, duration) : std::min(to, duration)), playback.RootNode, end, &reference))
			return fail();
		glm::mat4 cycles(1);
		if (looping)
		{
			double count = std::floor(to / duration);
			glm::mat4 cycle = frames.Cycle;
			while (count >= 1)
			{
				if (std::fmod(count, 2.0) >= 1)
					cycles = cycles * cycle;
				count = std::floor(count / 2.0);
				if (count >= 1)
					cycle = cycle * cycle;
			}
		}
		const glm::mat4 result = reference * glm::inverse(start) * cycles * end * glm::inverse(reference);
		if (!Math::IsFinite(result))
			return fail();
		return result;
	}

	void AnimationSystem::ApplyRootMotion(Entity entity, Playback& playback, const RootMotionFrame& motion, float seconds)
	{
		if (seconds <= 0 || playback.RootNode < 0)
			return;
		auto report = [&](const std::string& problem) {
			if (problem != playback.RootApplyProblem && !problem.empty())
				BS_CORE_ERROR("Animator '{}': {}", entity.GetName(), problem);
			playback.RootApplyProblem = problem;
		};
		const glm::mat4 old = m_Scene.GetWorldTransform(entity);
		glm::vec3 position, scale;
		glm::quat rotation;
		const glm::mat4& delta = motion.Delta;
		if (!Math::IsFinite(delta))
		{
			report("root motion produced a non-finite extracted delta");
			return;
		}
		if (!Math::IsFinite(old) || !Math::DecomposeTransform(old, position, rotation, scale))
		{
			report("root motion needs a finite delta and nondegenerate world transform");
			return;
		}
		const glm::vec3 displacement = rotation * (scale * glm::vec3(delta[3]));
		const float yaw = RootYaw(delta);
		auto* physics = m_Scene.GetPhysicsWorld();
		const bool character = physics && entity.HasComponent<CharacterControllerComponent>();
		const glm::mat4 world = Math::ComposeTransform(character ? position : position + displacement, rotation * glm::angleAxis(yaw, glm::vec3(0, 1, 0)), scale);
		glm::mat4 local = world;
		if (const Entity parent = entity.GetParent())
			local = glm::inverse(m_Scene.GetWorldTransform(parent)) * world;
		glm::vec3 localPosition, localScale;
		glm::quat localRotation;
		if (!Math::IsFinite(world) || !Math::IsFinite(local) || !Math::DecomposeTransform(local, localPosition, localRotation, localScale))
		{
			report("root motion cannot represent the resulting local transform (check parent scale)");
			return;
		}
		if (character)
			physics->QueueRootMotion(entity, displacement, motion.Share, seconds);
		m_Scene.SetWorldTransform(entity, world);
		report("");
	}

	void AnimationSystem::ApplyStateAxes(const Playback& playback, const AnimationState& state, SpaceState& space)
	{
		if (state.ParameterXIndex)
			space.Parameter.x = playback.Parameters[*state.ParameterXIndex];
		if (state.ParameterYIndex)
			space.Parameter.y = playback.Parameters[*state.ParameterYIndex];
	}
	bool AnimationSystem::EnterState(Playback& playback, size_t stateIndex, const AnimatorComponent& component, float fade)
	{
		const auto& state = playback.Graph->States[stateIndex];
		auto& reported = playback.StateProblems[stateIndex];
		SpaceState space{ .Name = state.BlendSpace };
		ApplyStateAxes(playback, state, space);
		std::string problem;
		const int32_t clip = state.BlendSpace.empty() ? FindClip(*playback.Source, state.Clip) : ResolveSpaceState(*playback.Source, component, space, problem);
		if (clip < 0)
		{
			const std::string message = "state '" + state.Name + "' cannot play: " + (problem.empty() ? "clip '" + state.Clip + "' not found" : problem);
			if (message != reported)
				BS_CORE_ERROR("Animator '{}': {}", playback.EntityName, message);
			reported = message;
			return false;
		}
		if (problem != reported && !problem.empty())
			BS_CORE_ERROR("Animator '{}': state '{}': {}", playback.EntityName, state.Name, problem);
		reported = problem;
		space.Problem = problem;
		if (fade > 0)
			BeginTransition(playback, fade);
		else
			ClearTransition(playback);
		playback.Space.reset();
		if (!state.BlendSpace.empty())
			playback.Space = std::move(space);
		playback.StateIndex = stateIndex;
		playback.StatePhase = 0;
		playback.Looping = state.Looping;
		ResolveRootNode(playback, component);
		Start(playback, clip);
		return true;
	}

	void AnimationSystem::UpdateGraph(Playback& playback, const AnimatorComponent& component)
	{
		const AnimationGraph& graph = *playback.Graph;
		if (!playback.StateIndex)
		{
			// Retry an initial state whose model clip or authored blend space was unavailable at bind time.
			EnterState(playback, graph.InitialStateIndex, component, 0);
			return;
		}
		const auto& state = graph.States[*playback.StateIndex];
		if (playback.Space)
		{
			ApplyStateAxes(playback, state, *playback.Space);
			ResolveSpace(playback, *playback.Space, component);
		}
		const double duration = CycleDuration(playback);
		for (const auto& transition : graph.Transitions)
		{
			if ((transition.FromIndex && transition.FromIndex != playback.StateIndex) || transition.ToIndex == *playback.StateIndex)
				continue;
			if (transition.ExitTime && duration > 0 && playback.StatePhase < *transition.ExitTime)
				continue;
			if (!std::ranges::all_of(transition.Conditions, [&](const AnimationCondition& condition) { return ConditionHolds(condition, playback.Parameters[condition.Parameter]); }))
				continue;
			if (EnterState(playback, transition.ToIndex, component, transition.Duration))
			{
				for (const auto& condition : transition.Conditions)
					if (graph.Parameters[condition.Parameter].Type == AnimationParameterType::Trigger)
						playback.Parameters[condition.Parameter] = 0;
				return;
			}
		}
	}

	bool AnimationSystem::SetParameterValue(Entity entity, const std::string& name, float value, AnimationParameterType type)
	{
		auto* playback = EnsurePlayable(entity);
		if (!playback)
			return false;
		const size_t typeIndex = static_cast<size_t>(type);
		std::optional<size_t> index;
		if (playback->Graph)
		{
			const auto& parameters = playback->Graph->Parameters;
			const auto it = std::ranges::find(parameters, name, &AnimationParameter::Name);
			if (it != parameters.end())
				index = static_cast<size_t>(it - parameters.begin());
		}
		std::string problem;
		if (playback->ConfiguredGraph.empty())
			problem = "Animator has no StateMachine";
		else if (!playback->Graph)
			problem = "state machine is not loaded: " + AssetManager::GetError(playback->ConfiguredGraph);
		else if (playback->Control != GraphControl::Active)
			problem = playback->Control == GraphControl::Stopped ? "state machine was stopped" : "direct playback took control of the state machine";
		else if (!std::isfinite(value))
			problem = "value must be finite";
		else
		{
			const auto& parameters = playback->Graph->Parameters;
			if (!index)
			{
				problem = "unknown name; declared parameters:";
				for (const auto& parameter : parameters)
					problem += " " + parameter.Name;
			}
			else if (parameters[*index].Type != type)
				problem = fmt::format("type mismatch: expected {}, got {}", ParameterTypeName(parameters[*index].Type), ParameterTypeName(type));
			else
			{
				playback->Parameters[*index] = value;
				playback->ParameterProblems[*index][typeIndex].clear();
				return true;
			}
		}
		auto& reported = index ? playback->ParameterProblems[*index][typeIndex] : playback->UnknownParameterProblems[typeIndex];
		const std::string diagnostic = index ? problem : name + ": " + problem;
		if (reported != diagnostic)
			BS_CORE_ERROR("Animator '{}': parameter '{}': {}", entity.GetName(), name, problem);
		reported = diagnostic;
		return false;
	}

	bool AnimationSystem::SetParameter(Entity entity, const std::string& name, float value)
	{
		return SetParameterValue(entity, name, value, AnimationParameterType::Float);
	}
	bool AnimationSystem::SetParameter(Entity entity, const std::string& name, bool value)
	{
		return SetParameterValue(entity, name, value ? 1.0f : 0.0f, AnimationParameterType::Bool);
	}
	bool AnimationSystem::SetTrigger(Entity entity, const std::string& name)
	{
		return SetParameterValue(entity, name, 1, AnimationParameterType::Trigger);
	}
	std::string AnimationSystem::GetState(Entity entity)
	{
		const auto* playback = Ensure(entity);
		return playback && playback->Control == GraphControl::Active && playback->StateIndex ? playback->Graph->States[*playback->StateIndex].Name : std::string();
	}

	void AnimationSystem::ClearTransition(Playback& playback)
	{
		playback.Outgoing = {};
		playback.Snapshot = {};
		playback.FadeTime = 0;
		playback.FadeDuration = 0;
		playback.BlendWeight = 1;
	}

	void AnimationSystem::BeginTransition(Playback& playback, float seconds)
	{
		const bool settled = playback.BlendWeight >= 1 && !playback.Rest;
		playback.Snapshot = playback.Local;
		playback.Outgoing.Space = playback.RootMotion && settled ? playback.Space : std::nullopt;
		playback.Outgoing.Clip = settled && !playback.Space ? playback.Clip : -1;
		playback.Outgoing.Time = playback.Time;
		playback.Outgoing.RootMotion = playback.RootMotion && (playback.Outgoing.Clip >= 0 || playback.Outgoing.Space);
		playback.Outgoing.Looping = playback.Looping;
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
		ResetRootIntervals(playback);
		Evaluate(playback);
	}

	void AnimationSystem::TakeDirectControl(Playback& playback, const AnimatorComponent& component)
	{
		playback.Control = GraphControl::Direct;
		playback.Paused = false;
		playback.Looping = component.Looping;
		ResolveRootNode(playback, component);
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
		TakeDirectControl(*playback, entity.GetComponent<AnimatorComponent>());
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
		TakeDirectControl(*playback, entity.GetComponent<AnimatorComponent>());
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
		TakeDirectControl(*playback, entity.GetComponent<AnimatorComponent>());
		// Updating a locomotion blend's weight must not restart either clock.
		if (playback->Outgoing.Clip != a || playback->Clip != b || playback->FadeDuration > 0 || playback->Space)
		{
			playback->Time = playback->Outgoing.Time = 0;
			playback->EventLimitLogged = false;
		}
		playback->Space.reset();
		playback->Outgoing.Space.reset();
		playback->Outgoing.Clip = a;
		playback->Outgoing.RootMotion = playback->RootMotion;
		playback->Outgoing.Looping = playback->Looping;
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
		const bool finished = playback->Space && playback->Space->Phase >= 1 && !playback->Looping;
		if (playback->Space && playback->Space->Name == space && playback->Playing && !finished)
		{
			TakeDirectControl(*playback, component);
			return SetBlendSpaceParameter(entity, parameter);
		}
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
		TakeDirectControl(*playback, component);
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
		{
			playback->Paused = paused;
			playback->Playing = !paused && (playback->Clip >= 0 || playback->Space) && !playback->Rest;
		}
	}

	void AnimationSystem::Stop(Entity entity)
	{
		if (auto* playback = Ensure(entity))
		{
			playback->Playing = false;
			playback->Time = 0;
			playback->Rest = true;
			playback->Control = GraphControl::Stopped;
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
		if (playback->Outgoing.Clip >= 0)
			playback->Outgoing.Time = std::min(seconds, playback->Source->Animations[playback->Outgoing.Clip].Duration);
		playback->Rest = false;
		const double duration = CycleDuration(*playback);
		playback->StatePhase = duration > 0 ? playback->Time / duration : 0;
		ResetRootIntervals(*playback);
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
		return static_cast<float>(CycleDuration(*playback));
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
		ReportCallProblem(m_BoneProblems, entity, "GetBoneTransform on", "GetBoneTransform", joint, error);
	}

	void AnimationSystem::ReportCallProblem(std::map<std::pair<UUID, std::string>, std::string>& reported, Entity entity, const char* owner, const char* operation, const std::string& name, const std::string& problem, const std::string& detail)
	{
		const std::pair<UUID, std::string> key{ entity.GetUUID(), fmt::format("{}:{}", operation, name) };
		if (problem.empty())
		{
			reported.erase(key);
			return;
		}
		std::string& last = reported[key];
		if (last != problem)
			BS_CORE_ERROR("{} '{}': {}{}", owner, entity.GetName(), problem, detail);
		last = problem;
	}

	void AnimationSystem::ReportLayerCall(Entity entity, const char* operation, const std::string& layer, const std::string& problem, const std::string& detail)
	{
		ReportCallProblem(m_LayerProblems, entity, "Animator", operation, layer, problem, detail);
	}

	void AnimationSystem::ReportIKCall(Entity entity, const char* operation, const std::string& chain, const std::string& problem, const std::string& detail)
	{
		ReportCallProblem(m_IKCallProblems, entity, "InverseKinematics", operation, chain, problem, detail);
	}

	float AnimationSystem::ClampSetting(float value, float low, float high, float fallback, const std::string& name, std::vector<std::string>& problems)
	{
		if (std::isnan(value))
		{
			problems.push_back(fmt::format("{} is not a number; using {}", name, fallback));
			return fallback;
		}
		if (value >= low && value <= high)
			return value;
		problems.push_back(fmt::format("{} must be between {} and {}; clamping", name, low, high));
		return std::clamp(value, low, high);
	}

	bool AnimationSystem::LayerKey::Matches(const AnimationLayer& config, const std::string& clip) const
	{
		const auto same = [](float a, float b) { return a == b || (std::isnan(a) && std::isnan(b)); };
		return Clip == clip && Blend == config.Blend && ReferenceClip == config.ReferenceClip && same(ReferenceTime, config.ReferenceTime) &&
			   std::ranges::equal(Mask, config.Mask, [&](const AnimationMaskJoint& a, const AnimationMaskJoint& b) { return a.Joint == b.Joint && same(a.Weight, b.Weight); });
	}

	const AnimationSystem::LayerResolution& AnimationSystem::ResolveLayer(Playback& playback, const AnimationLayer& config, LayerState& state)
	{
		const std::string& clip = state.ScriptClip ? *state.ScriptClip : config.Clip;
		if (state.Resolved && state.Resolved->Key.Matches(config, clip))
			return *state.Resolved;
		const MeshSource& source = *playback.Source;
		// Built as a value and moved in: Clang with libstdc++ rejects optional::emplace() for a nested struct with default
		// member initializers.
		LayerResolution resolved{ .Key = { clip, config.Blend, config.ReferenceClip, config.ReferenceTime, config.Mask } };
		std::vector<std::string> problems;
		if (static_cast<size_t>(config.Blend) >= AnimationLayerBlendCount)
			problems.push_back(fmt::format("Blend {} is not a blend mode; using Override", static_cast<int>(config.Blend)));
		// Unlike the base Clip, an empty layer Clip plays nothing.
		resolved.Clip = clip.empty() ? -1 : FindClip(source, clip);
		if (!clip.empty() && resolved.Clip < 0)
			problems.push_back(fmt::format("clip '{}' not found in '{}'", clip, source.Key));
		std::vector<std::string> missing;
		resolved.Mask.Weights = AnimationMaskWeights(source, config.Mask, missing);
		if (!missing.empty())
			problems.push_back(fmt::format("mask joint{} {} not found in '{}'", missing.size() > 1 ? "s" : "", fmt::join(missing, ", "), source.Key));
		resolved.Mask.Channels = resolved.Clip >= 0 ? AnimatedChannels(source, source.Animations[resolved.Clip]) : std::vector<uint8_t>(source.Nodes.size(), 0);
		if (config.Blend == AnimationLayerBlend::Additive && resolved.Clip >= 0)
		{
			const int32_t reference = config.ReferenceClip.empty() ? resolved.Clip : FindClip(source, config.ReferenceClip);
			if (reference < 0)
			{
				problems.push_back(fmt::format("reference clip '{}' not found in '{}'", config.ReferenceClip, source.Key));
				resolved.Clip = -1;
			}
			else
			{
				const AnimationClip& referenceClip = source.Animations[reference];
				float time = config.ReferenceTime;
				if (!std::isfinite(time) || time < 0)
				{
					problems.emplace_back("ReferenceTime must be finite and nonnegative; using 0");
					time = 0;
				}
				else if (time > referenceClip.Duration)
				{
					problems.push_back(fmt::format("ReferenceTime {} is past the end of '{}' ({} s); using its last frame", time, referenceClip.Name, referenceClip.Duration));
					time = referenceClip.Duration;
				}
				resolved.Reference = SampleAnimation(source, &referenceClip, time);
			}
		}
		std::string problem = fmt::format("{}", fmt::join(problems, "; "));
		if (!problem.empty() && problem != state.Problem)
			BS_CORE_WARN("Animator '{}': layer '{}': {}", playback.EntityName, config.Name, problem);
		state.Problem = std::move(problem);
		state.Resolved = std::move(resolved);
		return *state.Resolved;
	}

	void AnimationSystem::AdvanceLayers(Playback& playback, const AnimatorComponent& component, double seconds)
	{
		std::vector<LayerState> layers;
		layers.reserve(component.Layers.size());
		// Each state moves to at most one configuration, even if C++ code wrote duplicate names past Validate.
		std::vector<bool> taken(playback.Layers.size(), false);
		for (const AnimationLayer& config : component.Layers)
		{
			LayerState state;
			const auto found = std::ranges::find_if(playback.Layers, [&](const LayerState& layer) { return !taken[&layer - playback.Layers.data()] && layer.Name == config.Name; });
			if (found != playback.Layers.end())
			{
				taken[static_cast<size_t>(found - playback.Layers.begin())] = true;
				state = std::move(*found);
			}
			else
			{
				state.Name = config.Name;
				state.ConfiguredClip = config.Clip;
				state.Weight = FadingWeight(config.Weight);
			}
			// C++ code can store values Validate rejects: they are clamped, and said so once.
			std::vector<std::string> problems;
			ClampSetting(config.Weight, 0, 1, AnimationLayer{}.Weight, "Weight", problems);
			const float speed = ClampSetting(config.Speed, 0, MaxAnimatorSpeed, AnimationLayer{}.Speed, "Speed", problems);
			std::string problem = fmt::format("{}", fmt::join(problems, "; "));
			if (!problem.empty() && problem != state.ConfigProblem)
				BS_CORE_WARN("Animator '{}': layer '{}': {}", playback.EntityName, config.Name, problem);
			state.ConfigProblem = std::move(problem);
			state.Weight.Configure(config.Weight);
			if (config.Clip != state.ConfiguredClip)
			{
				state.ConfiguredClip = config.Clip;
				state.ScriptClip.reset();
				state.Time = 0;
			}
			const int32_t clip = ResolveLayer(playback, config, state).Clip;
			if (clip >= 0 && seconds > 0)
				state.Time = WrapClock(state.Time + seconds * speed, playback.Source->Animations[clip].Duration, config.Looping);
			state.Weight.Advance(static_cast<float>(seconds));
			layers.push_back(std::move(state));
		}
		playback.Layers = std::move(layers);
	}

	AnimationSystem::LayerState* AnimationSystem::FindLayer(Entity entity, Playback& playback, const std::string& layer, const char* operation)
	{
		AdvanceLayers(playback, entity.GetComponent<AnimatorComponent>(), 0);
		if (auto found = std::ranges::find(playback.Layers, layer, &LayerState::Name); found != playback.Layers.end())
			return &*found;
		// Unknown names share one slot per operation (see ReportCallProblem).
		ReportLayerCall(entity, operation, {}, fmt::format("no layer named '{}'", layer));
		return nullptr;
	}

	bool AnimationSystem::PlayLayer(Entity entity, const std::string& layer, const std::optional<std::string>& clip)
	{
		constexpr const char* operation = "PlayAnimationLayer";
		auto* playback = EnsurePlayable(entity);
		LayerState* state = playback ? FindLayer(entity, *playback, layer, operation) : nullptr;
		if (!state)
			return false;
		ReportLayerCall(entity, operation, {}, {});
		// An empty name would select the first clip in FindClip, while a layer reads an empty Clip as nothing.
		if (clip && clip->empty())
		{
			ReportLayerCall(entity, operation, layer, fmt::format("layer '{}': the clip name must not be empty", layer));
			return false;
		}
		if (clip && FindClip(*playback->Source, *clip) < 0)
		{
			ReportLayerCall(entity, operation, layer, fmt::format("layer '{}': clip '{}' not found in '{}'", layer, *clip, playback->Source->Key));
			return false;
		}
		const auto& component = entity.GetComponent<AnimatorComponent>();
		const AnimationLayer& config = *std::ranges::find(component.Layers, layer, &AnimationLayer::Name);
		// Applied only when the layer then shows something: a failed call changes nothing.
		LayerState previous = *state;
		state->ScriptClip = clip;
		state->Time = 0;
		if (ResolveLayer(*playback, config, *state).Clip < 0)
		{
			// The configuration problem itself was logged when the layer resolved.
			std::string problem = state->Problem.empty() ? fmt::format("layer '{}' has no Clip; pass one to PlayAnimationLayer", layer) : fmt::format("layer '{}' shows nothing with this clip (see its configuration warning)", layer);
			*state = std::move(previous);
			ReportLayerCall(entity, operation, layer, problem);
			return false;
		}
		ReportLayerCall(entity, operation, layer, {});
		Finish(*playback);
		return true;
	}

	bool AnimationSystem::SetLayerWeight(Entity entity, const std::string& layer, float weight, float seconds)
	{
		constexpr const char* operation = "SetAnimationLayerWeight";
		auto* playback = EnsurePlayable(entity);
		LayerState* state = playback ? FindLayer(entity, *playback, layer, operation) : nullptr;
		if (!state)
			return false;
		ReportLayerCall(entity, operation, {}, {});
		if (!IsValidFade(weight, seconds))
		{
			// The values stay out of the compared problem, so a script computing a different bad value every frame
			// logs once.
			ReportLayerCall(entity, operation, layer, fmt::format("layer '{}': the weight must be between 0 and 1 and the fade time finite and nonnegative", layer), fmt::format(" (got {} and {})", weight, seconds));
			return false;
		}
		ReportLayerCall(entity, operation, layer, {});
		state->Weight.FadeTo(weight, seconds);
		Finish(*playback);
		return true;
	}

	std::optional<float> AnimationSystem::GetLayerWeight(Entity entity, const std::string& layer)
	{
		constexpr const char* operation = "GetAnimationLayerWeight";
		auto* playback = EnsurePlayable(entity);
		LayerState* state = playback ? FindLayer(entity, *playback, layer, operation) : nullptr;
		if (!state)
			return std::nullopt;
		ReportLayerCall(entity, operation, {}, {});
		return state->Weight.Get();
	}

	std::vector<AnimationEventRecord> AnimationSystem::TakeEvents()
	{
		return std::exchange(m_PendingEvents, {});
	}
}
