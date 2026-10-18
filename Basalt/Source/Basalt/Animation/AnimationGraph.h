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
		// Names retain authored/diagnostic context; resolved indices are authoritative at runtime.
		std::string ParameterX, ParameterY;
		// Missing axis indices start that coordinate at zero.
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
		// Authored names are diagnostic; runtime uses the indices below.
		std::string From, To;
		// nullopt matches any state except ToIndex.
		std::optional<size_t> FromIndex;
		// Index of the validated target state.
		size_t ToIndex = 0;
		float Duration = 0.15f;
		std::optional<float> ExitTime;
		std::vector<AnimationCondition> Conditions;
	};
	// Immutable declarations shared by Animators using a .banimgraph asset. Transitions use array order as priority.
	struct AnimationGraph
	{
		// Authored name is diagnostic; InitialStateIndex selects the initial state at runtime.
		std::string InitialState;
		// Index of the validated initial state.
		size_t InitialStateIndex = 0;
		std::vector<AnimationParameter> Parameters;
		std::vector<AnimationState> States;
		std::vector<AnimationTransition> Transitions;
		// Strict, bounded parser that resolves parameter and state indices and validates all references.
		static std::optional<AnimationGraph> FromJson(const nlohmann::json& data, std::string& outError);
	};
}
