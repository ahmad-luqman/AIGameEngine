#include "Basalt/Core/InputActions.h"

#include "Basalt/Core/EnumNames.h"
#include "Basalt/Core/JsonUtils.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace Basalt {

	namespace {

		const char* const s_TypeNames[] = { "Button", "Axis" };
		static_assert(std::size(s_TypeNames) == InputActionTypeCount);

		// The binding keys that name a control, in the order of InputBinding::Control's alternatives.
		constexpr const char* ControlKeys[] = { "Key", "MouseButton", "GamepadButton", "GamepadAxis" };

		template<class... Ts>
		struct Overloaded : Ts...
		{
			using Ts::operator()...;
		};

		float ApplyDeadZone(float value, float deadZone)
		{
			const float magnitude = std::abs(value);
			if (magnitude <= deadZone)
				return 0.0f;
			return std::copysign((magnitude - deadZone) / (1.0f - deadZone), value);
		}

		// The binding's value before Scale: 1 or 0 for keys and buttons, a gamepad axis past its dead zone.
		float ReadControl(const InputBinding& binding, InputFrame frame)
		{
			// Every gamepad, or the one the binding names. Slots that are not connected read as released.
			const uint32_t first = binding.Gamepad.value_or(0);
			const uint32_t last = binding.Gamepad ? *binding.Gamepad + 1 : Input::MaxGamepads;
			return std::visit(
				Overloaded{
					[frame](KeyCode key) { return GameInput::IsKeyDown(key, frame) ? 1.0f : 0.0f; },
					[frame](MouseCode button) { return GameInput::IsMouseButtonDown(button, frame) ? 1.0f : 0.0f; },
					[frame, first, last](GamepadButton button) {
						for (uint32_t index = first; index < last; index++)
						{
							if (GameInput::IsGamepadButtonDown(index, button, frame))
								return 1.0f;
						}
						return 0.0f;
					},
					[frame, first, last, &binding](GamepadAxis axis) {
						float strongest = 0.0f;
						for (uint32_t index = first; index < last; index++)
						{
							const float value = ApplyDeadZone(GameInput::GetGamepadAxis(index, axis, frame), binding.DeadZone);
							if (std::abs(value) > std::abs(strongest))
								strongest = value;
						}
						return strongest;
					},
				},
				binding.Control);
		}

		bool IsValidName(const std::string& name, std::string& outError)
		{
			if (name.empty() || name.size() > InputActionMap::MaxNameLength)
			{
				outError = fmt::format("action names must be 1 to {} characters long", InputActionMap::MaxNameLength);
				return false;
			}
			if (std::ranges::any_of(name, [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; }))
			{
				outError = "action names must not contain control characters";
				return false;
			}
			return true;
		}

		bool ReadScaleOrDeadZone(const nlohmann::json& data, const char* key, float min, float max, float& out, std::string& outError)
		{
			const auto it = data.find(key);
			if (it == data.end())
				return true;
			if (!JsonToFloat(*it, out) || out < min || out > max)
			{
				outError = fmt::format("binding {} must be a number in [{}, {}]", key, min, max);
				return false;
			}
			return true;
		}

	}

	std::string_view InputActionTypeToString(InputActionType type)
	{
		const char* name = EnumNameOrNull(s_TypeNames, type);
		return name ? std::string_view(name) : std::string_view("Unknown");
	}

	std::optional<InputActionType> InputActionTypeFromString(std::string_view name)
	{
		return EnumFromName<InputActionType>(s_TypeNames, name);
	}

	const InputAction* InputActionMap::Find(std::string_view name) const
	{
		const auto it = std::ranges::find(m_Actions, name, &InputAction::Name);
		return it != m_Actions.end() ? &*it : nullptr;
	}

	bool InputActionMap::SetBindings(std::string_view name, std::vector<InputBinding> bindings, std::string& outError)
	{
		const auto it = std::ranges::find(m_Actions, name, &InputAction::Name);
		if (it == m_Actions.end())
		{
			outError = fmt::format("unknown input action '{}'", name);
			return false;
		}
		if (bindings.size() > MaxBindings)
		{
			outError = fmt::format("an action has at most {} bindings", MaxBindings);
			return false;
		}
		it->Bindings = std::move(bindings);
		return true;
	}

	float InputActionMap::GetValue(const InputAction& action, InputFrame frame)
	{
		if (action.Type == InputActionType::Axis)
		{
			float sum = 0.0f;
			for (const InputBinding& binding : action.Bindings)
				sum += ReadControl(binding, frame) * binding.Scale;
			return std::clamp(sum, -1.0f, 1.0f);
		}
		float strongest = 0.0f;
		for (const InputBinding& binding : action.Bindings)
			strongest = std::max(strongest, std::clamp(ReadControl(binding, frame) * binding.Scale, 0.0f, 1.0f));
		return strongest;
	}

	bool InputActionMap::IsDown(const InputAction& action, InputFrame frame)
	{
		return std::abs(GetValue(action, frame)) >= PressThreshold;
	}

	bool InputActionMap::IsPressed(const InputAction& action)
	{
		return IsDown(action, InputFrame::Current) && !IsDown(action, InputFrame::Previous);
	}

	bool InputActionMap::IsReleased(const InputAction& action)
	{
		return !IsDown(action, InputFrame::Current) && IsDown(action, InputFrame::Previous);
	}

	nlohmann::json InputActionMap::BindingToJson(const InputBinding& binding)
	{
		nlohmann::json data = nlohmann::json::object();
		std::visit(
			Overloaded{
				[&data](KeyCode key) { data["Key"] = std::string(KeyCodeToString(key)); },
				[&data](MouseCode button) { data["MouseButton"] = std::string(MouseCodeToString(button)); },
				[&data](GamepadButton button) { data["GamepadButton"] = std::string(GamepadButtonToString(button)); },
				[&data](GamepadAxis axis) { data["GamepadAxis"] = std::string(GamepadAxisToString(axis)); },
			},
			binding.Control);
		if (binding.Scale != 1.0f)
			data["Scale"] = binding.Scale;
		if (std::holds_alternative<GamepadAxis>(binding.Control) && binding.DeadZone != InputBinding::DefaultDeadZone)
			data["DeadZone"] = binding.DeadZone;
		if (binding.Gamepad)
			data["Gamepad"] = *binding.Gamepad;
		return data;
	}

	std::optional<InputBinding> InputActionMap::BindingFromJson(const nlohmann::json& data, std::string& outError)
	{
		if (!data.is_object())
		{
			outError = "a binding must be an object such as {\"Key\": \"Space\"}";
			return std::nullopt;
		}
		const char* controlKey = nullptr;
		for (const auto& [key, value] : data.items())
		{
			if (std::ranges::find(ControlKeys, key) != std::end(ControlKeys))
			{
				if (controlKey)
				{
					outError = fmt::format("a binding names one control, not both {} and {}", controlKey, key);
					return std::nullopt;
				}
				controlKey = *std::ranges::find(ControlKeys, key);
			}
			else if (key != "Scale" && key != "DeadZone" && key != "Gamepad")
			{
				outError = "binding: unknown key '" + key + "'";
				return std::nullopt;
			}
		}
		if (!controlKey)
		{
			outError = "a binding needs one of Key, MouseButton, GamepadButton or GamepadAxis";
			return std::nullopt;
		}
		const nlohmann::json& control = data[controlKey];
		const std::string name = control.is_string() ? control.get<std::string>() : std::string();
		InputBinding binding;
		bool known = false;
		if (std::string_view(controlKey) == "Key")
		{
			if (const auto key = KeyCodeFromString(name))
			{
				binding.Control = *key;
				known = true;
			}
		}
		else if (std::string_view(controlKey) == "MouseButton")
		{
			if (const auto button = MouseCodeFromString(name))
			{
				binding.Control = *button;
				known = true;
			}
		}
		else if (std::string_view(controlKey) == "GamepadButton")
		{
			if (const auto button = GamepadButtonFromString(name))
			{
				binding.Control = *button;
				known = true;
			}
		}
		else if (const auto axis = GamepadAxisFromString(name))
		{
			binding.Control = *axis;
			known = true;
		}
		if (!known)
		{
			outError = control.is_string() ? fmt::format("binding: unknown {} '{}'", controlKey, name) : fmt::format("binding {} must be a control name", controlKey);
			return std::nullopt;
		}

		if (!ReadScaleOrDeadZone(data, "Scale", -1.0f, 1.0f, binding.Scale, outError))
			return std::nullopt;
		const bool axis = std::holds_alternative<GamepadAxis>(binding.Control);
		if (data.contains("DeadZone") && !axis)
		{
			outError = "binding DeadZone applies to GamepadAxis bindings only";
			return std::nullopt;
		}
		if (!ReadScaleOrDeadZone(data, "DeadZone", 0.0f, InputBinding::MaxDeadZone, binding.DeadZone, outError))
			return std::nullopt;
		if (const auto gamepad = data.find("Gamepad"); gamepad != data.end())
		{
			if (!binding.IsGamepadControl())
			{
				outError = "binding Gamepad applies to GamepadButton and GamepadAxis bindings only";
				return std::nullopt;
			}
			uint32_t index = 0;
			if (!JsonToUInt32(*gamepad, index) || index >= Input::MaxGamepads)
			{
				outError = fmt::format("binding Gamepad must be an integer from 0 to {}", Input::MaxGamepads - 1);
				return std::nullopt;
			}
			binding.Gamepad = index;
		}
		return binding;
	}

	std::optional<std::vector<InputBinding>> InputActionMap::BindingsFromJson(const nlohmann::json& data, std::string& outError)
	{
		if (!data.is_array())
		{
			outError = "Bindings must be an array";
			return std::nullopt;
		}
		if (data.size() > MaxBindings)
		{
			outError = fmt::format("an action has at most {} bindings", MaxBindings);
			return std::nullopt;
		}
		std::vector<InputBinding> bindings;
		bindings.reserve(data.size());
		for (const nlohmann::json& entry : data)
		{
			auto binding = BindingFromJson(entry, outError);
			if (!binding)
				return std::nullopt;
			bindings.push_back(*binding);
		}
		return bindings;
	}

	nlohmann::json InputActionMap::ToJson() const
	{
		nlohmann::json actions = nlohmann::json::array();
		for (const InputAction& action : m_Actions)
		{
			nlohmann::json bindings = nlohmann::json::array();
			for (const InputBinding& binding : action.Bindings)
				bindings.push_back(BindingToJson(binding));
			actions.push_back({ { "Name", action.Name }, { "Type", std::string(InputActionTypeToString(action.Type)) }, { "Bindings", std::move(bindings) } });
		}
		return { { "Format", FormatName }, { "Version", FormatVersion }, { "Actions", std::move(actions) } };
	}

	std::optional<InputActionMap> InputActionMap::FromJson(const nlohmann::json& data, std::string& outError)
	{
		if (!data.is_object())
		{
			outError = "input actions must be an object {\"Actions\": [...]}";
			return std::nullopt;
		}
		for (const auto& [key, value] : data.items())
		{
			if (key != "Format" && key != "Version" && key != "Actions")
			{
				outError = "input actions: unknown key '" + key + "'";
				return std::nullopt;
			}
		}
		if (const auto format = data.find("Format"); format != data.end() && *format != FormatName)
		{
			outError = fmt::format("input actions: Format must be \"{}\"", FormatName);
			return std::nullopt;
		}
		if (const auto version = data.find("Version"); version != data.end() && !(version->is_number_integer() && version->get<int64_t>() == FormatVersion))
		{
			outError = fmt::format("input actions: unsupported Version (this build reads version {})", FormatVersion);
			return std::nullopt;
		}

		InputActionMap map;
		const auto actions = data.find("Actions");
		if (actions == data.end())
			return map;
		if (!actions->is_array())
		{
			outError = "input actions: Actions must be an array";
			return std::nullopt;
		}
		if (actions->size() > MaxActions)
		{
			outError = fmt::format("input actions: at most {} actions are supported", MaxActions);
			return std::nullopt;
		}
		for (const nlohmann::json& entry : *actions)
		{
			if (!entry.is_object())
			{
				outError = "input actions: every action must be an object {\"Name\", \"Type\", \"Bindings\"}";
				return std::nullopt;
			}
			for (const auto& [key, value] : entry.items())
			{
				if (key != "Name" && key != "Type" && key != "Bindings")
				{
					outError = "input action: unknown key '" + key + "'";
					return std::nullopt;
				}
			}
			InputAction action;
			const auto name = entry.find("Name");
			if (name == entry.end() || !name->is_string())
			{
				outError = "input action: Name must be a string";
				return std::nullopt;
			}
			action.Name = name->get<std::string>();
			if (!IsValidName(action.Name, outError))
				return std::nullopt;
			if (map.Find(action.Name))
			{
				outError = "input actions: duplicate action '" + action.Name + "'";
				return std::nullopt;
			}
			if (const auto type = entry.find("Type"); type != entry.end())
			{
				const auto parsed = type->is_string() ? InputActionTypeFromString(type->get<std::string>()) : std::nullopt;
				if (!parsed)
				{
					outError = "input action '" + action.Name + "': Type must be \"Button\" or \"Axis\"";
					return std::nullopt;
				}
				action.Type = *parsed;
			}
			if (const auto bindings = entry.find("Bindings"); bindings != entry.end())
			{
				auto parsed = BindingsFromJson(*bindings, outError);
				if (!parsed)
				{
					outError = "input action '" + action.Name + "': " + outError;
					return std::nullopt;
				}
				action.Bindings = std::move(*parsed);
			}
			map.m_Actions.push_back(std::move(action));
		}
		return map;
	}

}
