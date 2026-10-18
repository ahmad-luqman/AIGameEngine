#include "Basalt/Animation/AnimationGraph.h"

#include "Basalt/Core/JsonUtils.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace Basalt {

	const AnimationState* AnimationGraph::FindState(const std::string& name) const
	{
		const auto it = std::ranges::find(States, name, &AnimationState::Name);
		return it == States.end() ? nullptr : &*it;
	}

	std::optional<AnimationGraph> AnimationGraph::FromJson(const nlohmann::json& data, std::string& outError)
	{
		using json = nlohmann::json;
		outError.clear();
		try
		{
			auto object = [&](const json& value, std::initializer_list<std::string_view> keys) {
				std::string error;
				if (!value.is_object() || !RejectUnknownKeys(value, keys, "animation graph", error))
					throw std::runtime_error(error.empty() ? "expected an object" : error);
			};
			auto list = [](const json& value, size_t max) {
				if (!value.is_array() || value.size() > max)
					throw std::runtime_error("expected an array with at most " + std::to_string(max) + " elements");
			};
			auto number = [](const json& value) {
				float result = 0;
				if (!JsonToFloat(value, result))
					throw std::runtime_error("expected a finite float");
				return result;
			};
			auto name = [](const json& value) {
				const std::string result = value.get<std::string>();
				if (result.empty())
					throw std::runtime_error("name must not be empty");
				return result;
			};
			object(data, { "InitialState", "Parameters", "States", "Transitions" });
			AnimationGraph graph;
			graph.InitialState = name(data.at("InitialState"));
			const json parameters = data.value("Parameters", json::array());
			list(parameters, 64);
			std::unordered_set<std::string> names;
			for (const auto& p : parameters)
			{
				object(p, { "Name", "Type", "Default" });
				AnimationParameter parameter;
				parameter.Name = name(p.at("Name"));
				if (!names.insert(parameter.Name).second)
					throw std::runtime_error("duplicate parameter '" + parameter.Name + "'");
				const std::string type = p.at("Type").get<std::string>();
				if (type == "Float")
					parameter.Default = number(p.value("Default", json(0)));
				else if (type == "Bool" || type == "Trigger")
				{
					parameter.Type = type == "Bool" ? AnimationParameterType::Bool : AnimationParameterType::Trigger;
					parameter.Default = p.value("Default", false) ? 1.0f : 0.0f;
					if (parameter.Type == AnimationParameterType::Trigger && parameter.Default != 0)
						throw std::runtime_error("triggers must default to false");
				}
				else
					throw std::runtime_error("unknown parameter type '" + type + "'");
				graph.Parameters.push_back(std::move(parameter));
			}
			auto parameterIndex = [&](const std::string& key) {
				const auto it = std::ranges::find(graph.Parameters, key, &AnimationParameter::Name);
				if (it == graph.Parameters.end())
					throw std::runtime_error("unknown parameter '" + key + "'");
				return static_cast<size_t>(it - graph.Parameters.begin());
			};
			list(data.at("States"), 128);
			names.clear();
			for (const auto& s : data.at("States"))
			{
				object(s, { "Name", "Clip", "BlendSpace", "ParameterX", "ParameterY", "Looping", "RootMotion" });
				AnimationState state;
				state.Name = name(s.at("Name"));
				if (!names.insert(state.Name).second)
					throw std::runtime_error("duplicate state '" + state.Name + "'");
				state.Clip = s.value("Clip", std::string());
				state.BlendSpace = s.value("BlendSpace", std::string());
				if (s.contains("Clip") == s.contains("BlendSpace") || (s.contains("BlendSpace") && state.BlendSpace.empty()))
					throw std::runtime_error("state must select exactly one Clip or BlendSpace");
				state.ParameterX = s.value("ParameterX", std::string());
				state.ParameterY = s.value("ParameterY", std::string());
				for (const std::string* axis : { &state.ParameterX, &state.ParameterY })
					if (!axis->empty() && graph.Parameters[parameterIndex(*axis)].Type != AnimationParameterType::Float)
						throw std::runtime_error("blend axes require Float parameters");
				if (state.BlendSpace.empty() && (!state.ParameterX.empty() || !state.ParameterY.empty()))
					throw std::runtime_error("clip states cannot have blend axes");
				state.Looping = s.value("Looping", true);
				state.RootMotion = s.value("RootMotion", false);
				graph.States.push_back(std::move(state));
			}
			if (!graph.FindState(graph.InitialState))
				throw std::runtime_error("InitialState does not name a state");
			const json transitions = data.value("Transitions", json::array());
			list(transitions, 512);
			for (const auto& t : transitions)
			{
				object(t, { "From", "To", "Duration", "ExitTime", "Conditions" });
				AnimationTransition transition;
				transition.From = t.value("From", std::string());
				transition.To = name(t.at("To"));
				if ((!transition.From.empty() && !graph.FindState(transition.From)) || !graph.FindState(transition.To))
					throw std::runtime_error("transition names an unknown state");
				transition.Duration = number(t.value("Duration", json(0.15f)));
				if (transition.Duration < 0 || transition.Duration > 60)
					throw std::runtime_error("Duration must be in [0, 60]");
				if (t.contains("ExitTime"))
				{
					const float exit = number(t.at("ExitTime"));
					if (exit < 0 || exit > 1)
						throw std::runtime_error("ExitTime must be in [0, 1]");
					transition.ExitTime = exit;
				}
				const json conditions = t.value("Conditions", json::array());
				list(conditions, 64);
				for (const auto& c : conditions)
				{
					object(c, { "Parameter", "Operator", "Value" });
					AnimationCondition condition;
					condition.Parameter = parameterIndex(name(c.at("Parameter")));
					const auto type = graph.Parameters[condition.Parameter].Type;
					const std::string op = c.value("Operator", std::string("Equal"));
					if (op == "Greater")
						condition.Comparison = AnimationComparison::Greater;
					else if (op == "Less")
						condition.Comparison = AnimationComparison::Less;
					else if (op == "NotEqual")
						condition.Comparison = AnimationComparison::NotEqual;
					else if (op != "Equal")
						throw std::runtime_error("unknown condition operator '" + op + "'");
					if (type == AnimationParameterType::Float)
						condition.Value = number(c.at("Value"));
					else
					{
						condition.Value = c.value("Value", true) ? 1.0f : 0.0f;
						if (op != "Equal" && op != "NotEqual")
							throw std::runtime_error("Bool/Trigger conditions require Equal or NotEqual");
						if (type == AnimationParameterType::Trigger && (op != "Equal" || condition.Value != 1))
							throw std::runtime_error("Trigger conditions must test Equal true");
					}
					transition.Conditions.push_back(condition);
				}
				graph.Transitions.push_back(std::move(transition));
			}
			return graph;
		}
		catch (const nlohmann::json::exception& e)
		{
			outError = e.what();
		}
		catch (const std::runtime_error& e)
		{
			outError = e.what();
		}
		return std::nullopt;
	}
}
