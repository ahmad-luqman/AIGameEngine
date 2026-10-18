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
	// One typed parameter declaration; each Animator owns its current value.
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
		std::optional<size_t> ParameterXIndex, ParameterYIndex;
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
		// Omitted Bool/Trigger values test true. Float conditions require an explicit value.
		float Value = 1;
	};
	// Ordered edge; exit time and conditions must all pass before the target starts.
	struct AnimationTransition
	{
		// Empty From matches any state except To. Explicit self-edges are rejected; states never restart implicitly.
		std::string From, To;
		std::optional<size_t> FromIndex;
		size_t ToIndex = 0;
		float Duration = 0.15f;
		std::optional<float> ExitTime;
		std::vector<AnimationCondition> Conditions;
	};
	// Immutable declarations shared by Animators using a .banimgraph asset. Transitions use array order as priority.
	struct AnimationGraph
	{
		std::string InitialState;
		size_t InitialStateIndex = 0;
		std::vector<AnimationParameter> Parameters;
		std::vector<AnimationState> States;
		std::vector<AnimationTransition> Transitions;
		// Strict, bounded parser that resolves parameter indices and validates all references.
		static std::optional<AnimationGraph> FromJson(const nlohmann::json& data, std::string& outError);
		const AnimationState* FindState(const std::string& name) const;
	};
}
