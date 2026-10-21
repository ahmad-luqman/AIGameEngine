#include "Basalt/Animation/Retargeting.h"

#include "Basalt/Animation/AnimationPose.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Scene/Components.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

namespace Basalt {

	size_t RetargetMap::MappedCount() const
	{
		return static_cast<size_t>(std::ranges::count_if(SourceOf, [](int32_t node) { return node >= 0; }));
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

		// Rig prefixes that carry no body part: VRM's J_Bip_C_, 3ds Max's Bip01, Rigify's DEF-/ORG-.
		bool IsNoise(const std::string& token)
		{
			return token == "j" || token == "bip" || token == "bip01" || token == "c" || token == "def" || token == "org";
		}

		struct RestFrame
		{
			std::vector<glm::mat4> Globals;
			std::vector<glm::quat> Rotations;
		};

		glm::quat WorldRotation(const glm::mat4& global)
		{
			glm::vec3 translation, scale;
			glm::quat rotation;
			if (!Math::DecomposeAffineTransform(global, translation, rotation, scale))
				return glm::quat(1, 0, 0, 0);
			return glm::normalize(rotation);
		}

		bool Rest(const MeshSource& source, RestFrame& out)
		{
			if (!EvaluateAnimation(source, nullptr, 0, out.Globals))
				return false;
			out.Rotations.resize(out.Globals.size());
			for (size_t i = 0; i < out.Globals.size(); i++)
				out.Rotations[i] = WorldRotation(out.Globals[i]);
			return true;
		}

		// The root's rest height above the lowest mapped joint, in model space (glTF is Y-up).
		float LegLength(const std::vector<glm::mat4>& globals, int32_t root, const std::vector<int32_t>& mapped)
		{
			float lowest = globals[root][3].y;
			for (int32_t node : mapped)
				lowest = std::min(lowest, globals[node][3].y);
			return globals[root][3].y - lowest;
		}

		constexpr size_t MaxBakedFloats = size_t{ 16 } * 1024 * 1024;
		constexpr float CubicSampleRate = 30.0f;
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
			for (const std::string& token : Tokens(StripNamespace(joint)))
			{
				if (token == "l" || token == "left")
					side = "Left";
				else if (token == "r" || token == "right")
					side = "Right";
				else if (!IsNoise(token))
					part += token;
			}
			const auto found = Parts().find(part);
			if (found == Parts().end())
				return {};
			const bool paired = found->second != "Hips" && found->second != "Spine" && found->second != "Chest" && found->second != "UpperChest" && found->second != "Neck" && found->second != "Head";
			// A side on a centre bone, or none on a paired one, is some other joint (a twist bone, a prop).
			if (paired == side.empty())
				return {};
			return side + found->second;
		}

		RetargetMap BuildMap(const MeshSource& target, const MeshSource& source, const std::vector<AnimationRetargetJoint>& joints, std::vector<std::string>& outWarnings)
		{
			RetargetMap map;
			map.SourceOf.assign(target.Nodes.size(), -1);
			map.Reasons.assign(target.Nodes.size(), nullptr);
			std::vector<bool> used(source.Nodes.size(), false);
			auto pair = [&](size_t targetNode, size_t sourceNode, const char* reason) {
				if (map.SourceOf[targetNode] >= 0 || used[sourceNode])
					return;
				map.SourceOf[targetNode] = static_cast<int32_t>(sourceNode);
				map.Reasons[targetNode] = reason;
				used[sourceNode] = true;
			};
			for (const AnimationRetargetJoint& joint : joints)
			{
				if (joint.Source.empty() || joint.Target.empty())
					continue;
				const int32_t targetNode = FindNode(target, joint.Target);
				const int32_t sourceNode = FindNode(source, joint.Source);
				if (targetNode < 0 || sourceNode < 0)
					outWarnings.push_back(fmt::format("RetargetJoints names '{}', which '{}' does not have", targetNode < 0 ? joint.Target : joint.Source, targetNode < 0 ? target.Key : source.Key));
				else
					pair(static_cast<size_t>(targetNode), static_cast<size_t>(sourceNode), "joint map");
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
					pair(i, found->second, "name");
			for (size_t i = 0; i < target.Nodes.size(); i++)
				if (const auto found = sourceBones.find(HumanoidBone(target.Nodes[i].Name)); found != sourceBones.end())
					pair(i, found->second, "humanoid");

			// The root: the humanoid hips when both sides have them mapped together, else the topmost mapped target node.
			for (uint32_t node : target.NodeOrder)
				if (map.SourceOf[node] >= 0 && HumanoidBone(target.Nodes[node].Name) == "Hips")
				{
					map.TargetRoot = static_cast<int32_t>(node);
					break;
				}
			if (map.TargetRoot < 0)
				for (uint32_t node : target.NodeOrder)
					if (map.SourceOf[node] >= 0)
					{
						map.TargetRoot = static_cast<int32_t>(node);
						break;
					}
			if (map.TargetRoot < 0)
				return map;
			map.SourceRoot = map.SourceOf[map.TargetRoot];

			RestFrame targetRest, sourceRest;
			if (!Rest(target, targetRest) || !Rest(source, sourceRest))
				return map;
			std::vector<int32_t> targetMapped, sourceMapped;
			for (size_t i = 0; i < map.SourceOf.size(); i++)
				if (map.SourceOf[i] >= 0)
				{
					targetMapped.push_back(static_cast<int32_t>(i));
					sourceMapped.push_back(map.SourceOf[i]);
				}
			const float targetLeg = LegLength(targetRest.Globals, map.TargetRoot, targetMapped);
			const float sourceLeg = LegLength(sourceRest.Globals, map.SourceRoot, sourceMapped);
			if (targetLeg > 1e-6f && sourceLeg > 1e-6f && std::isfinite(targetLeg / sourceLeg))
				map.RootScale = targetLeg / sourceLeg;
			return map;
		}

		bool BakeClip(const MeshSource& target, const MeshSource& source, const RetargetMap& map, const AnimationClip& clip, AnimationClip& outClip, std::string& outError)
		{
			outClip = AnimationClip{ .Name = clip.Name, .Duration = clip.Duration };
			RestFrame targetRest, sourceRest;
			if (!Rest(target, targetRest) || !Rest(source, sourceRest))
			{
				outError = "non-finite rest pose";
				return false;
			}

			// Sample times: every source key, plus a steady rate where cubic curves bend between keys.
			std::set<float> times = { 0.0f, clip.Duration };
			bool stepped = !clip.Tracks.empty();
			bool cubic = false;
			for (const AnimationTrack& track : clip.Tracks)
			{
				times.insert(track.Times.begin(), track.Times.end());
				stepped = stepped && track.Interpolation == AnimationInterpolation::Step;
				cubic = cubic || track.Interpolation == AnimationInterpolation::CubicSpline;
			}
			if (cubic)
			{
				const auto steps = static_cast<size_t>(std::ceil(clip.Duration * CubicSampleRate));
				if (steps > MaxBakedFloats)
				{
					outError = "the baked clip exceeds the decoded animation budget";
					return false;
				}
				for (size_t i = 1; i < steps; i++)
					times.insert(clip.Duration * static_cast<float>(i) / static_cast<float>(steps));
			}

			// Which source channels the clip moves, per source node.
			const std::vector<uint8_t> channels = AnimatedChannels(source, clip);
			const size_t mapped = map.MappedCount();
			if (times.size() > MaxBakedFloats / std::max<size_t>(1, mapped * 11))
			{
				outError = "the baked clip exceeds the decoded animation budget";
				return false;
			}

			struct Channel
			{
				std::vector<glm::vec4> Values;
			};
			std::vector<Channel> rotations(target.Nodes.size()), translations(target.Nodes.size()), scales(target.Nodes.size());
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
						const glm::quat world = WorldRotation(sourceGlobals[sourceNode]) * glm::inverse(sourceRest.Rotations[sourceNode]) * targetRest.Rotations[node];
						local.Rotation = glm::normalize(glm::inverse(WorldRotation(parent)) * world);
						std::vector<glm::vec4>& rotation = rotations[node].Values;
						glm::vec4 value(local.Rotation.x, local.Rotation.y, local.Rotation.z, local.Rotation.w);
						// Keep consecutive keys on one hemisphere so linear blending takes the short way.
						if (!rotation.empty() && glm::dot(rotation.back(), value) < 0)
							value = -value;
						rotation.push_back(value);
						if (static_cast<int32_t>(node) == map.TargetRoot)
						{
							const glm::vec3 displacement = (glm::vec3(sourceGlobals[sourceNode][3]) - glm::vec3(sourceRest.Globals[sourceNode][3])) * map.RootScale;
							const glm::vec3 position = glm::vec3(targetRest.Globals[node][3]) + displacement;
							local.Translation = glm::vec3(glm::inverse(parent) * glm::vec4(position, 1));
							translations[node].Values.emplace_back(local.Translation, 0.0f);
						}
						const glm::vec3 restScale = source.Nodes[sourceNode].Scale;
						glm::vec3 ratio(1.0f);
						for (int axis = 0; axis < 3; axis++)
							if (std::abs(restScale[axis]) > 1e-6f)
								ratio[axis] = sourcePose.Nodes[sourceNode].Scale[axis] / restScale[axis];
						local.Scale = targetNode.Scale * ratio;
						scales[node].Values.emplace_back(local.Scale, 0.0f);
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
				add(translations[node].Values, AnimationPath::Translation, (animated & AnimatedChannel::Translation) != 0);
				add(rotations[node].Values, AnimationPath::Rotation, (animated & AnimatedChannel::Rotation) != 0);
				add(scales[node].Values, AnimationPath::Scale, (animated & AnimatedChannel::Scale) != 0);
			}

			// Morph weights follow node and target names; they do not depend on the skeletons.
			for (const AnimationTrack& track : clip.Tracks)
			{
				if (track.Path != AnimationPath::Weights)
					continue;
				const MeshNode& sourceNode = source.Nodes[track.Node];
				const auto& sourceTargets = source.Meshes[sourceNode.MeshIndex].MorphTargets;
				for (size_t node = 0; node < target.Nodes.size(); node++)
				{
					const MeshNode& targetNode = target.Nodes[node];
					if (targetNode.MorphWeightOffset < 0 || StripNamespace(targetNode.Name) != StripNamespace(sourceNode.Name) ||
						std::ranges::any_of(outClip.Tracks, [&](const AnimationTrack& baked) { return baked.Path == AnimationPath::Weights && baked.Node == node; }))
						continue;
					const auto& targetTargets = target.Meshes[targetNode.MeshIndex].MorphTargets;
					std::vector<int32_t> from(targetTargets.size(), -1);
					bool any = false;
					for (size_t t = 0; t < targetTargets.size(); t++)
						if (const auto it = std::ranges::find(sourceTargets, targetTargets[t]); it != sourceTargets.end())
						{
							from[t] = static_cast<int32_t>(it - sourceTargets.begin());
							any = true;
						}
					if (!any)
						continue;
					const bool cubicWeights = track.Interpolation == AnimationInterpolation::CubicSpline;
					const size_t values = track.Weights.size() / track.Width;
					AnimationTrack weights{ .Node = static_cast<uint32_t>(node), .Path = AnimationPath::Weights, .Interpolation = track.Interpolation, .Times = track.Times, .Width = static_cast<uint32_t>(targetTargets.size()) };
					weights.Weights.reserve(values * targetTargets.size());
					for (size_t v = 0; v < values; v++)
						for (size_t t = 0; t < targetTargets.size(); t++)
						{
							// Unmatched targets hold their default; cubic tangents of those are flat.
							const bool tangent = cubicWeights && v % 3 != 1;
							weights.Weights.push_back(from[t] >= 0 ? track.Weights[v * track.Width + static_cast<size_t>(from[t])] : (tangent ? 0.0f : targetNode.MorphWeights[t]));
						}
					outClip.Tracks.push_back(std::move(weights));
				}
			}
			return true;
		}
	}

}
