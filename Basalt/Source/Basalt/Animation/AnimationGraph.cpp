#include "Basalt/Animation/AnimationGraph.h"

#include "Basalt/Core/JsonUtils.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace Basalt {
	namespace {
		using json = nlohmann::json;
		// Parses declarations separately while carrying the precise location of a validation failure.
		class GraphParser
		{
		public:
			AnimationGraph Parse(const json& data)
			{
				Object(data, { "InitialState", "Parameters", "States", "Transitions" });
				m_Graph.InitialState = Name(data.at("InitialState"));
				m_Location = "Parameters";
				const json parameters = data.value("Parameters", json::array());
				List(parameters, 64);
				for (size_t i = 0; i < parameters.size(); i++)
				{
					m_Location = "Parameters[" + std::to_string(i) + "]";
					m_Graph.Parameters.push_back(ParseParameter(parameters[i]));
				}
				m_Location = "States";
				List(data.at("States"), 128);
				m_Names.clear();
				for (size_t i = 0; i < data.at("States").size(); i++)
				{
					m_Location = "States[" + std::to_string(i) + "]";
					m_Graph.States.push_back(ParseState(data.at("States")[i]));
				}
				m_Location = "InitialState";
				if (!m_Graph.FindState(m_Graph.InitialState))
					throw std::runtime_error("InitialState does not name a state");
				m_Graph.InitialStateIndex = static_cast<size_t>(m_Graph.FindState(m_Graph.InitialState) - m_Graph.States.data());
				m_Location = "Transitions";
				const json transitions = data.value("Transitions", json::array());
				List(transitions, 512);
				for (size_t i = 0; i < transitions.size(); i++)
				{
					m_Location = "Transitions[" + std::to_string(i) + "]";
					m_Graph.Transitions.push_back(ParseTransition(transitions[i]));
				}
				return std::move(m_Graph);
			}
			const std::string& GetLocation() const { return m_Location; }

		private:
			void Object(const json& value, std::initializer_list<std::string_view> keys)
			{
				std::string error;
				if (!value.is_object() || !RejectUnknownKeys(value, keys, "animation graph", error))
					throw std::runtime_error(error.empty() ? "expected an object" : error);
			}

			void List(const json& value, size_t max)
			{
				if (!value.is_array() || value.size() > max)
					throw std::runtime_error("expected an array with at most " + std::to_string(max) + " elements");
			}

			float Number(const json& value)
			{
				float result = 0;
				if (!JsonToFloat(value, result))
					throw std::runtime_error("expected a finite float");
				return result;
			}

			std::string Name(const json& value)
			{
				const std::string result = value.get<std::string>();
				if (result.empty())
					throw std::runtime_error("name must not be empty");
				return result;
			}

			AnimationParameter ParseParameter(const json& p)
			{
				Object(p, { "Name", "Type", "Default" });
				AnimationParameter parameter;
				parameter.Name = Name(p.at("Name"));
				if (!m_Names.insert(parameter.Name).second)
					throw std::runtime_error("duplicate parameter '" + parameter.Name + "'");
				const std::string type = p.at("Type").get<std::string>();
				if (type == "Float")
					parameter.Default = Number(p.value("Default", json(0)));
				else if (type == "Bool" || type == "Trigger")
				{
					parameter.Type = type == "Bool" ? AnimationParameterType::Bool : AnimationParameterType::Trigger;
					parameter.Default = p.value("Default", false) ? 1.0f : 0.0f;
					if (parameter.Type == AnimationParameterType::Trigger && parameter.Default != 0)
						throw std::runtime_error("triggers must default to false");
				}
				else
					throw std::runtime_error("unknown parameter type '" + type + "'");
				return parameter;
			}

			size_t ParameterIndex(const std::string& key) const
			{
				const auto it = std::ranges::find(m_Graph.Parameters, key, &AnimationParameter::Name);
				if (it == m_Graph.Parameters.end())
					throw std::runtime_error("unknown parameter '" + key + "'");
				return static_cast<size_t>(it - m_Graph.Parameters.begin());
			}

			AnimationState ParseState(const json& s)
			{
				Object(s, { "Name", "Clip", "BlendSpace", "ParameterX", "ParameterY", "Looping", "RootMotion" });
				AnimationState state;
				state.Name = Name(s.at("Name"));
				if (!m_Names.insert(state.Name).second)
					throw std::runtime_error("duplicate state '" + state.Name + "'");
				state.Clip = s.value("Clip", std::string());
				state.BlendSpace = s.value("BlendSpace", std::string());
				if (s.contains("Clip") == s.contains("BlendSpace") || (s.contains("BlendSpace") && state.BlendSpace.empty()))
					throw std::runtime_error("state must select exactly one Clip or BlendSpace");
				state.ParameterX = s.value("ParameterX", std::string());
				state.ParameterY = s.value("ParameterY", std::string());
				for (const std::string* axis : { &state.ParameterX, &state.ParameterY })
					if (!axis->empty() && m_Graph.Parameters[ParameterIndex(*axis)].Type != AnimationParameterType::Float)
						throw std::runtime_error("blend axes require Float parameters");
				if (state.BlendSpace.empty() && (!state.ParameterX.empty() || !state.ParameterY.empty()))
					throw std::runtime_error("clip states cannot have blend axes");
				state.Looping = s.value("Looping", true);
				state.RootMotion = s.value("RootMotion", false);
				if (!state.ParameterX.empty())
					state.ParameterXIndex = ParameterIndex(state.ParameterX);
				if (!state.ParameterY.empty())
					state.ParameterYIndex = ParameterIndex(state.ParameterY);
				return state;
			}

			AnimationComparison ParseComparison(const std::string& op)
			{
				if (op == "Greater")
					return AnimationComparison::Greater;
				if (op == "Less")
					return AnimationComparison::Less;
				if (op == "Equal")
					return AnimationComparison::Equal;
				if (op == "NotEqual")
					return AnimationComparison::NotEqual;
				throw std::runtime_error("unknown condition operator '" + op + "'");
			}

			AnimationTransition ParseTransition(const json& t)
			{
				Object(t, { "From", "To", "Duration", "ExitTime", "Conditions" });
				AnimationTransition transition;
				transition.From = t.value("From", std::string());
				transition.To = Name(t.at("To"));
				if ((!transition.From.empty() && !m_Graph.FindState(transition.From)) || !m_Graph.FindState(transition.To))
					throw std::runtime_error("transition names an unknown state");
				transition.ToIndex = static_cast<size_t>(m_Graph.FindState(transition.To) - m_Graph.States.data());
				if (!transition.From.empty())
					transition.FromIndex = static_cast<size_t>(m_Graph.FindState(transition.From) - m_Graph.States.data());
				if (!transition.From.empty() && transition.From == transition.To)
					throw std::runtime_error("explicit self-transition is not allowed");
				m_Location += " (" + (transition.From.empty() ? "Any" : transition.From) + "->" + transition.To + ")";
				const auto edgeLocation = m_Location;
				m_Location = edgeLocation + ".Duration";
				transition.Duration = Number(t.value("Duration", json(AnimationTransition{}.Duration)));
				if (transition.Duration < 0 || transition.Duration > 60)
					throw std::runtime_error("Duration must be in [0, 60]");
				if (t.contains("ExitTime"))
				{
					m_Location = edgeLocation + ".ExitTime";
					const float exit = Number(t.at("ExitTime"));
					if (exit < 0 || exit > 1)
						throw std::runtime_error("ExitTime must be in [0, 1]");
					transition.ExitTime = exit;
				}
				m_Location = edgeLocation + ".Conditions";
				const json conditions = t.value("Conditions", json::array());
				List(conditions, 64);
				for (size_t i = 0; i < conditions.size(); i++)
				{
					m_Location = edgeLocation + ".Conditions[" + std::to_string(i) + "]";
					const auto& c = conditions[i];
					Object(c, { "Parameter", "Operator", "Value" });
					AnimationCondition condition;
					condition.Parameter = ParameterIndex(Name(c.at("Parameter")));
					const auto type = m_Graph.Parameters[condition.Parameter].Type;
					const std::string op = c.value("Operator", std::string("Equal"));
					condition.Comparison = ParseComparison(op);
					if (type == AnimationParameterType::Float)
						condition.Value = Number(c.at("Value"));
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
				return transition;
			}

			AnimationGraph m_Graph;
			std::unordered_set<std::string> m_Names;
			std::string m_Location = "graph";
		};
	}
	const AnimationState* AnimationGraph::FindState(const std::string& name) const
	{
		const auto it = std::ranges::find(States, name, &AnimationState::Name);
		return it == States.end() ? nullptr : &*it;
	}
	std::optional<AnimationGraph> AnimationGraph::FromJson(const nlohmann::json& data, std::string& outError)
	{
		outError.clear();
		GraphParser parser;
		try
		{
			return parser.Parse(data);
		}
		catch (const nlohmann::json::exception& e)
		{
			outError = parser.GetLocation() + ": " + e.what();
		}
		catch (const std::runtime_error& e)
		{
			outError = parser.GetLocation() + ": " + e.what();
		}
		return std::nullopt;
	}
}
