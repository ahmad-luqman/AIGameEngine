#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	enum class AnimationParameterType
	{
		Float,
		Bool,
		Trigger
	};
	// Immutable declarations shared by every Animator using a .banimgraph asset.
	struct AnimationParameter
	{
		std::string Name;
		AnimationParameterType Type = AnimationParameterType::Float;
		float Default = 0;
	};
	// One named clip or blend-space state, including its playback and motion policy.
	struct AnimationState
	{
		std::string Name;
		std::string Clip;
		std::string BlendSpace;
		std::string ParameterX, ParameterY;
		bool Looping = true;
		bool RootMotion = false;
	};
	enum class AnimationComparison
	{
		Greater,
		Less,
		Equal,
		NotEqual
	};
	// A typed comparison against one parameter declaration by index.
	struct AnimationCondition
	{
		size_t Parameter = 0;
		AnimationComparison Comparison = AnimationComparison::Equal;
		float Value = 1;
	};
	// Ordered edge; exit time and conditions must all pass before the target starts.
	struct AnimationTransition
	{
		// Empty From matches any state, except To (no implicit self-restarts).
		std::string From, To;
		float Duration = 0.15f;
		std::optional<float> ExitTime;
		std::vector<AnimationCondition> Conditions;
	};
	// Strict, bounded graph parser. Transitions use array order as priority and all their conditions must match.
	struct AnimationGraph
	{
		std::string InitialState;
		std::vector<AnimationParameter> Parameters;
		std::vector<AnimationState> States;
		std::vector<AnimationTransition> Transitions;
		static std::optional<AnimationGraph> FromJson(const nlohmann::json& data, std::string& outError);
		const AnimationState* FindState(const std::string& name) const;
	};
}
