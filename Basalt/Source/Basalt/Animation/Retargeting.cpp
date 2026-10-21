#include "Basalt/Animation/Retargeting.h"

#include "Basalt/Animation/AnimationPose.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Scene/Components.h"

#include <spdlog/fmt/ranges.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <set>

namespace Basalt {

	size_t RetargetMap::MappedCount() const
	{
		return static_cast<size_t>(std::ranges::count_if(SourceOf, [](int32_t node) { return node >= 0; }));
	}

	const char* ToString(RetargetMatch match)
	{
		switch (match)
		{
			case RetargetMatch::JointMap:
				return "joint map";
			case RetargetMatch::Name:
				return "name";
			case RetargetMatch::Humanoid:
				return "humanoid";
			case RetargetMatch::None:
				break;
		}
		return "none";
	}
	namespace {
		// Lowercase name pieces: separators split, as do lower-to-upper case changes ("LeftUpLeg" -> left, up, leg);
		// digits stay with the letters before them ("Spine1"), so Mixamo's Spine1 and Unreal's spine_01 differ.
		std::vector<std::string> Tokens(std::string_view name)
		{
			std::vector<std::string> tokens;
			std::string current;
			char previous = 0;
			for (char c : name)
			{
				const auto u = static_cast<unsigned char>(c);
				if (!std::isalnum(u))
				{
					if (!current.empty())
						tokens.push_back(std::move(current));
					current.clear();
				}
				else
				{
					if (!current.empty() && std::isupper(u) && std::islower(static_cast<unsigned char>(previous)))
					{
						tokens.push_back(std::move(current));
						current.clear();
					}
					current += static_cast<char>(std::tolower(u));
				}
				previous = c;
			}
			if (!current.empty())
				tokens.push_back(std::move(current));
			return tokens;
		}

		// Body parts by their joined tokens without side and rig prefixes. Mixamo and 3ds Max number the spine from
		// "Spine" (Spine1 is the chest); Unreal numbers spine_01 upward.
		const std::map<std::string, std::string, std::less<>>& Parts()
		{
			static const std::map<std::string, std::string, std::less<>> s_Parts = {
				{ "hips", "Hips" },
				{ "hip", "Hips" },
				{ "pelvis", "Hips" },
				{ "spine", "Spine" },
				{ "spine01", "Spine" },
				{ "abdomen", "Spine" },
				{ "spine1", "Chest" },
				{ "spine02", "Chest" },
				{ "chest", "Chest" },
				{ "spine2", "UpperChest" },
				{ "spine03", "UpperChest" },
				{ "upperchest", "UpperChest" },
				{ "neck", "Neck" },
				{ "neck01", "Neck" },
				{ "head", "Head" },
				{ "shoulder", "Shoulder" },
				{ "clavicle", "Shoulder" },
				{ "collar", "Shoulder" },
				{ "arm", "UpperArm" },
				{ "upperarm", "UpperArm" },
				{ "uparm", "UpperArm" },
				{ "forearm", "LowerArm" },
				{ "lowerarm", "LowerArm" },
				{ "hand", "Hand" },
				{ "upleg", "UpperLeg" },
				{ "thigh", "UpperLeg" },
				{ "upperleg", "UpperLeg" },
				{ "leg", "LowerLeg" },
				{ "calf", "LowerLeg" },
				{ "shin", "LowerLeg" },
				{ "lowerleg", "LowerLeg" },
				{ "foot", "Foot" },
				{ "toebase", "Toes" },
				{ "toe", "Toes" },
				{ "toes", "Toes" },
				{ "toe0", "Toes" },
				{ "ball", "Toes" },
			};
			return s_Parts;
		}

		// Rigify's deform spine counts segments from the pelvis: spine is the hips, spine.001 the spine, and so on.
		const std::map<std::string, std::string, std::less<>>& RigifySpine()
		{
			static const std::map<std::string, std::string, std::less<>> s_Spine = {
				{ "spine", "Hips" },
				{ "spine001", "Spine" },
				{ "spine002", "Chest" },
				{ "spine003", "UpperChest" },
				{ "spine004", "Neck" },
				{ "spine006", "Head" },
			};
			return s_Spine;
		}

		// Bones without a side; every other part comes in a Left and a Right.
		constexpr std::string_view CentreBones[] = { "Hips", "Spine", "Chest", "UpperChest", "Neck", "Head" };

		// Rig prefixes that carry no body part: VRM's J_Bip_C_, 3ds Max's Bip01/Bip001, Rigify's DEF-/ORG-.
		bool IsNoise(const std::string& token)
		{
			if (token.starts_with("bip") && std::all_of(token.begin() + 3, token.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
				return true;
			return token == "j" || token == "c" || token == "def" || token == "org";
		}

		// A global transform's rotation, or nothing when it is not affine (zero-scale axes still have one).
		std::optional<glm::quat> DecomposedRotation(const glm::mat4& global)
		{
			glm::vec3 translation, scale;
			glm::quat rotation;
			if (!Math::DecomposeAffineTransform(global, translation, rotation, scale))
				return std::nullopt;
			return glm::normalize(rotation);
		}

		glm::quat WorldRotation(const glm::mat4& global)
		{
			return DecomposedRotation(global).value_or(glm::quat(1, 0, 0, 0));
		}

		// Model-space rest positions and rotations; false for a non-finite rest pose. Joints whose rest transform is not affine get
		// the identity rotation and are listed in outDegenerate.
		bool Rest(const MeshSource& source, RestPose& out, std::vector<uint32_t>& outDegenerate)
		{
			std::vector<glm::mat4> globals;
			if (!EvaluateAnimation(source, nullptr, 0, globals))
				return false;
			out.Positions.resize(globals.size());
			out.Rotations.resize(globals.size());
			for (size_t i = 0; i < globals.size(); i++)
			{
				out.Positions[i] = glm::vec3(globals[i][3]);
				const auto rotation = DecomposedRotation(globals[i]);
				out.Rotations[i] = rotation.value_or(glm::quat(1, 0, 0, 0));
				if (!rotation)
					outDegenerate.push_back(static_cast<uint32_t>(i));
			}
			return true;
		}

		// The root's rest height above the lowest of `mapped`, in model space (glTF is Y-up).
		float LegLength(const std::vector<glm::vec3>& positions, int32_t root, const std::vector<int32_t>& mapped)
		{
			float lowest = positions[root].y;
			for (int32_t node : mapped)
				lowest = std::min(lowest, positions[node].y);
			return positions[root].y - lowest;
		}

		// Per mapped joint and key: up to three tracks (translation, rotation, scale), each a vec4 value and a time.
		constexpr size_t BakedFloatsPerMappedKey = size_t{ 3 } * (4 + 1);
		constexpr double CubicSampleRate = 30.0;
		constexpr const char* BudgetError = "the baked clip exceeds the decoded animation budget";

		// The bake's sample times: every source key, 0 and the duration, plus a 30 Hz grid over the whole clip when any
		// track is cubic. False, without allocating, when they cannot fit in `limit` keys; outTimes is filled only when
		// asked for or when the cheap bound is not enough.
		bool SampleTimes(const AnimationClip& clip, size_t limit, std::set<float>* outTimes)
		{
			double bound = 2;
			bool cubic = false;
			for (const AnimationTrack& track : clip.Tracks)
			{
				bound += static_cast<double>(track.Times.size());
				cubic = cubic || track.Interpolation == AnimationInterpolation::CubicSpline;
			}
			const double steps = cubic ? std::ceil(static_cast<double>(clip.Duration) * CubicSampleRate) : 0.0;
			// The grid's times are distinct, so a grid alone past the limit cannot fit.
			if (!(steps <= static_cast<double>(limit)))
				return false;
			if (!outTimes && bound + steps <= static_cast<double>(limit))
				return true;
			std::set<float> local;
			std::set<float>& times = outTimes ? *outTimes : local;
			times = { 0.0f, clip.Duration };
			for (const AnimationTrack& track : clip.Tracks)
				times.insert(track.Times.begin(), track.Times.end());
			for (size_t i = 1; i < static_cast<size_t>(steps); i++)
				times.insert(clip.Duration * static_cast<float>(i) / static_cast<float>(steps));
			return times.size() <= limit;
		}

		// Per target morph target of `node`: the source track's index for the same target name, or -1.
		std::vector<int32_t> MatchMorphTargets(const MeshSource& target, const MeshNode& node, const std::vector<std::string>& sourceTargets)
		{
			const auto& targetTargets = target.Meshes[node.MeshIndex].MorphTargets;
			std::vector<int32_t> from(targetTargets.size(), -1);
			for (size_t t = 0; t < targetTargets.size(); t++)
				if (const auto it = std::ranges::find(sourceTargets, targetTargets[t]); it != sourceTargets.end())
					from[t] = static_cast<int32_t>(it - sourceTargets.begin());
			return from;
		}

		// The target nodes a source weights track moves: same name (ignoring namespaces), at least one morph target name
		// in common.
		template<typename Visit>
		void ForEachWeightsTarget(const MeshSource& target, const MeshSource& source, const AnimationTrack& track, Visit&& visit)
		{
			if (track.Node >= source.Nodes.size() || source.Nodes[track.Node].MeshIndex < 0)
				return;
			const MeshNode& sourceNode = source.Nodes[track.Node];
			const auto& sourceTargets = source.Meshes[sourceNode.MeshIndex].MorphTargets;
			for (size_t node = 0; node < target.Nodes.size(); node++)
			{
				const MeshNode& targetNode = target.Nodes[node];
				if (targetNode.MorphWeightOffset < 0 || Retargeting::StripNamespace(targetNode.Name) != Retargeting::StripNamespace(sourceNode.Name))
					continue;
				std::vector<int32_t> from = MatchMorphTargets(target, targetNode, sourceTargets);
				if (std::ranges::any_of(from, [](int32_t index) { return index >= 0; }))
					visit(static_cast<uint32_t>(node), from);
			}
		}
	}

	namespace {
		// CheckBakeBudget, also returning the sample times when outTimes is given.
		bool CheckBudget(const MeshSource& target, const MeshSource& source, const RetargetMap& map, const AnimationClip& clip, std::set<float>* outTimes, std::string& outError)
		{
			// Weights tracks keep their keys, expanded to their target nodes' widths; the joint tracks take what is left, at
			// BakedFloatsPerMappedKey per mapped joint and sample time.
			size_t weightFloats = 0;
			for (const AnimationTrack& track : clip.Tracks)
				if (track.Path == AnimationPath::Weights && track.Width > 0)
				{
					const size_t values = track.Weights.size() / track.Width;
					ForEachWeightsTarget(target, source, track, [&](uint32_t, const std::vector<int32_t>& from) { weightFloats += values * from.size() + track.Times.size(); });
				}
			const size_t mapped = map.MappedCount();
			if (weightFloats > Retargeting::MaxClipBakedFloats ||
				!SampleTimes(clip, (Retargeting::MaxClipBakedFloats - weightFloats) / std::max<size_t>(1, mapped * BakedFloatsPerMappedKey), outTimes))
			{
				outError = BudgetError;
				return false;
			}
			return true;
		}
	}

	namespace Retargeting {
		std::string_view StripNamespace(std::string_view joint)
		{
			const size_t separator = joint.find_last_of(":|");
			return separator == std::string_view::npos ? joint : joint.substr(separator + 1);
		}

		std::string HumanoidBone(std::string_view joint)
		{
			std::string side, part;
			bool rigify = false;
			for (const std::string& token : Tokens(StripNamespace(joint)))
			{
				rigify = rigify || token == "def" || token == "org";
				if (token == "l" || token == "left")
					side = "Left";
				else if (token == "r" || token == "right")
					side = "Right";
				else if (!IsNoise(token))
					part += token;
			}
			const auto& parts = rigify && RigifySpine().contains(part) ? RigifySpine() : Parts();
			const auto found = parts.find(part);
			if (found == parts.end())
				return {};
			const bool paired = std::ranges::find(CentreBones, found->second) == std::end(CentreBones);
			// A side on a centre bone, or none on a paired one, is some other joint (a twist bone, a prop).
			if (paired == side.empty())
				return {};
			return side + found->second;
		}

		RetargetMap BuildMap(const MeshSource& target, const MeshSource& source, const std::vector<AnimationRetargetJoint>& joints, std::vector<std::string>& outWarnings)
		{
			RetargetMap map;
			map.SourceOf.assign(target.Nodes.size(), -1);
			map.Matches.assign(target.Nodes.size(), RetargetMatch::None);
			std::vector<bool> used(source.Nodes.size(), false);
			auto pair = [&](size_t targetNode, size_t sourceNode, RetargetMatch match) {
				if (map.SourceOf[targetNode] >= 0 || used[sourceNode])
					return false;
				map.SourceOf[targetNode] = static_cast<int32_t>(sourceNode);
				map.Matches[targetNode] = match;
				used[sourceNode] = true;
				return true;
			};
			for (const AnimationRetargetJoint& joint : joints)
			{
				// Entries without either name are the inspector's unnamed additions.
				if (joint.Source.empty() && joint.Target.empty())
					continue;
				if (joint.Source.empty() || joint.Target.empty())
				{
					outWarnings.push_back(fmt::format("joint map entry '{}' -> '{}' needs both a Source and a Target", joint.Source, joint.Target));
					continue;
				}
				const int32_t targetNode = FindNode(target, joint.Target);
				const int32_t sourceNode = FindNode(source, joint.Source);
				std::vector<std::string> missing;
				if (sourceNode < 0)
					missing.push_back(fmt::format("Source '{}' is not in '{}'", joint.Source, source.Key));
				if (targetNode < 0)
					missing.push_back(fmt::format("Target '{}' is not in '{}'", joint.Target, target.Key));
				if (!missing.empty())
					outWarnings.push_back(fmt::format("joint map entry '{}' -> '{}': {}", joint.Source, joint.Target, fmt::join(missing, "; ")));
				else if (!pair(static_cast<size_t>(targetNode), static_cast<size_t>(sourceNode), RetargetMatch::JointMap))
					outWarnings.push_back(fmt::format("joint map entry '{}' -> '{}' is skipped: an earlier entry already pairs one of the joints", joint.Source, joint.Target));
			}
			std::map<std::string_view, size_t> sourceNames;
			std::map<std::string, size_t> sourceBones;
			for (size_t i = 0; i < source.Nodes.size(); i++)
			{
				sourceNames.try_emplace(StripNamespace(source.Nodes[i].Name), i);
				if (std::string bone = HumanoidBone(source.Nodes[i].Name); !bone.empty())
					sourceBones.try_emplace(std::move(bone), i);
			}
			for (size_t i = 0; i < target.Nodes.size(); i++)
				if (const auto found = sourceNames.find(StripNamespace(target.Nodes[i].Name)); found != sourceNames.end())
					pair(i, found->second, RetargetMatch::Name);
			std::vector<std::string> targetBones(target.Nodes.size());
			for (size_t i = 0; i < target.Nodes.size(); i++)
				if (targetBones[i] = HumanoidBone(target.Nodes[i].Name); !targetBones[i].empty())
					if (const auto found = sourceBones.find(targetBones[i]); found != sourceBones.end())
						pair(i, found->second, RetargetMatch::Humanoid);
			// glTF cannot animate a node given as a matrix, so such a pair never moves.
			for (size_t i = 0; i < target.Nodes.size(); i++)
				if (map.SourceOf[i] >= 0 && target.Nodes[i].HasMatrix)
					outWarnings.push_back(fmt::format("joint '{}' of '{}' is given as a matrix, which cannot be animated, so it keeps its rest pose", target.Nodes[i].Name, target.Key));

			const auto firstMapped = [&](auto&& accept) -> int32_t {
				for (uint32_t node : target.NodeOrder)
					if (map.SourceOf[node] >= 0 && accept(node))
						return static_cast<int32_t>(node);
				return -1;
			};
			map.TargetRoot = firstMapped([&](uint32_t node) { return targetBones[node] == "Hips"; });
			if (map.TargetRoot < 0)
				map.TargetRoot = firstMapped([](uint32_t) { return true; });
			if (map.TargetRoot >= 0)
				map.SourceRoot = map.SourceOf[map.TargetRoot];

			std::vector<uint32_t> targetDegenerate, sourceDegenerate;
			const bool targetFinite = Rest(target, map.TargetRest, targetDegenerate);
			if (!targetFinite || !Rest(source, map.SourceRest, sourceDegenerate))
			{
				outWarnings.push_back(fmt::format("the rest pose of '{}' is not finite, so no clip can be retargeted", targetFinite ? source.Key : target.Key));
				map.TargetRest = {};
				map.SourceRest = {};
				return map;
			}
			// A joint without a rest rotation has no orientation to correct from; its children still pose, but it turns from
			// identity.
			for (uint32_t node : targetDegenerate)
				if (map.SourceOf[node] >= 0)
					outWarnings.push_back(fmt::format("joint '{}' of '{}' has a rest transform that is not affine; its rest orientation counts as identity", target.Nodes[node].Name, target.Key));
			for (uint32_t node : sourceDegenerate)
				if (used[node])
					outWarnings.push_back(fmt::format("joint '{}' of '{}' has a rest transform that is not affine; its rest orientation counts as identity", source.Nodes[node].Name, source.Key));
			if (map.TargetRoot < 0)
				return map;
			std::vector<int32_t> targetMapped, sourceMapped;
			for (size_t i = 0; i < map.SourceOf.size(); i++)
				if (map.SourceOf[i] >= 0)
				{
					targetMapped.push_back(static_cast<int32_t>(i));
					sourceMapped.push_back(map.SourceOf[i]);
				}
			const float targetLeg = LegLength(map.TargetRest.Positions, map.TargetRoot, targetMapped);
			const float sourceLeg = LegLength(map.SourceRest.Positions, map.SourceRoot, sourceMapped);
			if (targetLeg > 1e-6f && sourceLeg > 1e-6f && std::isfinite(targetLeg / sourceLeg))
				map.RootScale = targetLeg / sourceLeg;
			// Only a humanoid's hips are expected to stand above its feet; other roots (tails, props) move unscaled.
			else if (targetBones[map.TargetRoot] == "Hips")
				outWarnings.push_back(fmt::format("the root '{}' is not above any other paired joint in both skeletons, so its movement is not scaled to the model's size", target.Nodes[map.TargetRoot].Name));
			return map;
		}

		bool CheckBakeBudget(const MeshSource& target, const MeshSource& source, const RetargetMap& map, const AnimationClip& clip, std::string& outError)
		{
			return CheckBudget(target, source, map, clip, nullptr, outError);
		}

		bool BakeClip(const MeshSource& target, const MeshSource& source, const RetargetMap& map, const AnimationClip& clip, AnimationClip& outClip, std::vector<std::string>& outWarnings, std::string& outError)
		{
			outClip = AnimationClip{ .Name = clip.Name, .Duration = clip.Duration };
			const auto inSource = [&](int32_t node) { return node >= -1 && node < static_cast<int32_t>(source.Nodes.size()); };
			if (map.SourceOf.size() != target.Nodes.size() || map.Matches.size() != target.Nodes.size() || !std::ranges::all_of(map.SourceOf, inSource))
			{
				outError = "the joint map was built for other skeletons";
				return false;
			}
			if (map.TargetRest.Rotations.size() != target.Nodes.size() || map.TargetRest.Positions.size() != target.Nodes.size() ||
				map.SourceRest.Rotations.size() != source.Nodes.size() || map.SourceRest.Positions.size() != source.Nodes.size())
			{
				outError = "non-finite rest pose";
				return false;
			}
			for (const AnimationTrack& track : clip.Tracks)
			{
				const size_t stride = track.Interpolation == AnimationInterpolation::CubicSpline ? 3 : 1;
				const bool weights = track.Path == AnimationPath::Weights;
				if (track.Node >= source.Nodes.size() || track.Times.empty() ||
					(weights ? (track.Width == 0 || track.Width != source.Nodes[track.Node].MorphWeights.size() || track.Weights.size() != track.Times.size() * stride * track.Width)
							 : track.Values.size() != track.Times.size() * stride))
				{
					outError = "a track does not fit the library's nodes";
					return false;
				}
			}
			std::set<float> times;
			if (!CheckBudget(target, source, map, clip, &times, outError))
				return false;

			const bool stepped = !clip.Tracks.empty() && std::ranges::all_of(clip.Tracks, [](const AnimationTrack& track) { return track.Interpolation == AnimationInterpolation::Step; });
			// Which source channels the clip moves, per source node.
			const std::vector<uint8_t> channels = AnimatedChannels(source, clip);
			std::vector<std::vector<glm::vec4>> rotations(target.Nodes.size()), translations(target.Nodes.size()), scales(target.Nodes.size());
			std::vector<glm::mat4> sourceGlobals, targetGlobals(target.Nodes.size());
			for (float time : times)
			{
				const AnimationLocalPose sourcePose = SampleAnimation(source, &clip, time);
				if (!ComposeAnimationPose(source, sourcePose, sourceGlobals))
				{
					outError = fmt::format("non-finite source pose at {} s", time);
					return false;
				}
				for (uint32_t node : target.NodeOrder)
				{
					const MeshNode& targetNode = target.Nodes[node];
					const glm::mat4 parent = targetNode.Parent >= 0 ? targetGlobals[targetNode.Parent] : glm::mat4(1);
					AnimationNodePose local{ targetNode.Translation, targetNode.Rotation, targetNode.Scale };
					if (const int32_t sourceNode = map.SourceOf[node]; sourceNode >= 0 && !targetNode.HasMatrix)
					{
						// Turn the target's rest orientation by the source's turn away from its own rest, in model space.
						const glm::quat world = WorldRotation(sourceGlobals[sourceNode]) * glm::inverse(map.SourceRest.Rotations[sourceNode]) * map.TargetRest.Rotations[node];
						local.Rotation = glm::normalize(glm::inverse(WorldRotation(parent)) * world);
						std::vector<glm::vec4>& rotation = rotations[node];
						glm::vec4 value(local.Rotation.x, local.Rotation.y, local.Rotation.z, local.Rotation.w);
						// Keep consecutive keys on one hemisphere so linear blending takes the short way.
						if (!rotation.empty() && glm::dot(rotation.back(), value) < 0)
							value = -value;
						rotation.push_back(value);
						if (static_cast<int32_t>(node) == map.TargetRoot)
						{
							const glm::vec3 displacement = (glm::vec3(sourceGlobals[sourceNode][3]) - map.SourceRest.Positions[sourceNode]) * map.RootScale;
							const glm::vec3 position = map.TargetRest.Positions[node] + displacement;
							local.Translation = glm::vec3(glm::inverse(parent) * glm::vec4(position, 1));
							translations[node].emplace_back(local.Translation, 0.0f);
						}
						const glm::vec3 restScale = source.Nodes[sourceNode].Scale;
						glm::vec3 ratio(1.0f);
						for (int axis = 0; axis < 3; axis++)
							if (std::abs(restScale[axis]) > 1e-6f)
								ratio[axis] = sourcePose.Nodes[sourceNode].Scale[axis] / restScale[axis];
						local.Scale = targetNode.Scale * ratio;
						scales[node].emplace_back(local.Scale, 0.0f);
					}
					targetGlobals[node] = targetNode.HasMatrix ? parent * targetNode.Matrix : parent * Math::ComposeTransform(local.Translation, local.Rotation, local.Scale);
				}
			}

			// Keep the channels that move or that the source animates, so layers still leave the others alone.
			const std::vector<float> keyTimes(times.begin(), times.end());
			const auto varies = [](const std::vector<glm::vec4>& values) {
				return std::ranges::any_of(values, [&](const glm::vec4& value) { return glm::any(glm::greaterThan(glm::abs(value - values.front()), glm::vec4(1e-6f))); });
			};
			for (size_t node = 0; node < target.Nodes.size(); node++)
			{
				const int32_t sourceNode = map.SourceOf[node];
				if (sourceNode < 0 || target.Nodes[node].HasMatrix)
					continue;
				const uint8_t animated = channels[sourceNode];
				const auto add = [&](std::vector<glm::vec4>& values, AnimationPath path, bool keep) {
					if (values.empty() || !(keep || varies(values)))
						return;
					outClip.Tracks.push_back({ .Node = static_cast<uint32_t>(node), .Path = path, .Interpolation = stepped ? AnimationInterpolation::Step : AnimationInterpolation::Linear, .Times = keyTimes, .Values = std::move(values) });
				};
				add(translations[node], AnimationPath::Translation, (animated & AnimatedChannel::Translation) != 0);
				add(rotations[node], AnimationPath::Rotation, (animated & AnimatedChannel::Rotation) != 0);
				add(scales[node], AnimationPath::Scale, (animated & AnimatedChannel::Scale) != 0);
			}

			// Morph weights follow node and target names; they do not depend on the skeletons.
			for (const AnimationTrack& track : clip.Tracks)
			{
				if (track.Path != AnimationPath::Weights)
					continue;
				bool matched = false, moved = false;
				ForEachWeightsTarget(target, source, track, [&](uint32_t node, const std::vector<int32_t>& from) {
					matched = true;
					if (std::ranges::any_of(outClip.Tracks, [&](const AnimationTrack& baked) { return baked.Path == AnimationPath::Weights && baked.Node == node; }))
						return;
					moved = true;
					const MeshNode& targetNode = target.Nodes[node];
					const bool cubic = track.Interpolation == AnimationInterpolation::CubicSpline;
					const size_t values = track.Weights.size() / track.Width;
					AnimationTrack weights{ .Node = node, .Path = AnimationPath::Weights, .Interpolation = track.Interpolation, .Times = track.Times, .Width = static_cast<uint32_t>(from.size()) };
					weights.Weights.reserve(values * from.size());
					for (size_t v = 0; v < values; v++)
						for (size_t t = 0; t < from.size(); t++)
						{
							// Unmatched targets hold their default; cubic tangents of those are flat.
							const bool tangent = cubic && v % 3 != 1;
							weights.Weights.push_back(from[t] >= 0 ? track.Weights[v * track.Width + static_cast<size_t>(from[t])] : (tangent ? 0.0f : targetNode.MorphWeights[t]));
						}
					outClip.Tracks.push_back(std::move(weights));
				});
				if (!matched)
					outWarnings.push_back(fmt::format("clip '{}': the morph weights of '{}' match no node and morph target name of '{}'", clip.Name, source.Nodes[track.Node].Name, target.Key));
				else if (!moved)
					outWarnings.push_back(fmt::format("clip '{}': the morph weights of '{}' drive nodes an earlier weights track of the clip already drives, so they are skipped", clip.Name, source.Nodes[track.Node].Name));
			}
			return true;
		}
	}

}
