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
		static_assert(InputControlKeys.size() == std::variant_size_v<decltype(InputBinding::Control)>);

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

		// A binding's contribution: its control's value (1 or 0 for keys and buttons, a gamepad axis past its dead
		// zone) times Scale, clamped to 0..1 for a button. Across gamepads the strongest contribution wins, so a
		// scaled stick pushed the right way on one gamepad is not outweighed by another pushed the other way.
		float ReadBinding(const InputBinding& binding, InputFrame frame, InputActionType type)
		{
			const auto contribution = [&binding, type](float control) {
				const float value = control * binding.Scale;
				return type == InputActionType::Button ? std::clamp(value, 0.0f, 1.0f) : value;
			};
			// Every gamepad, or the one the binding names. Slots that are not connected read as released.
			const uint32_t first = binding.Gamepad.value_or(0);
			const uint32_t last = binding.Gamepad ? *binding.Gamepad + 1 : Input::MaxGamepads;
			const auto strongest = [&](auto&& read) {
				float best = 0.0f;
				for (uint32_t index = first; index < last; index++)
				{
					const float value = contribution(read(index));
					if (std::abs(value) > std::abs(best))
						best = value;
				}
				return best;
			};
			return std::visit(
				Overloaded{
					[&](KeyCode key) { return contribution(GameInput::IsKeyDown(key, frame) ? 1.0f : 0.0f); },
					[&](MouseCode button) { return contribution(GameInput::IsMouseButtonDown(button, frame) ? 1.0f : 0.0f); },
					[&](GamepadButton button) { return strongest([&](uint32_t index) { return GameInput::IsGamepadButtonDown(index, button, frame) ? 1.0f : 0.0f; }); },
					[&](GamepadAxis axis) { return strongest([&](uint32_t index) { return ApplyDeadZone(GameInput::GetGamepadAxis(index, axis, frame), binding.DeadZone); }); },
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

		// An optional number field; ValidateBinding checks its range.
		bool ReadOptionalFloat(const nlohmann::json& data, const char* key, float& out, std::string& outError)
		{
			const auto it = data.find(key);
			if (it == data.end() || JsonToFloat(*it, out))
				return true;
			outError = fmt::format("binding {} must be a number", key);
			return false;
		}

		// The control a binding's control key (an index into InputControlKeys) names, or nullopt for an unknown name.
		std::optional<decltype(InputBinding::Control)> ParseControl(size_t keyIndex, std::string_view name)
		{
			const auto wrap = [](const auto& parsed) -> std::optional<decltype(InputBinding::Control)> {
				if (parsed)
					return *parsed;
				return std::nullopt;
			};
			switch (keyIndex)
			{
				case 0:
					return wrap(KeyCodeFromString(name));
				case 1:
					return wrap(MouseCodeFromString(name));
				case 2:
					return wrap(GamepadButtonFromString(name));
				default:
					return wrap(GamepadAxisFromString(name));
			}
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
		for (const InputBinding& binding : bindings)
		{
			if (!ValidateBinding(binding, outError))
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
				sum += ReadBinding(binding, frame, action.Type);
			return std::clamp(sum, -1.0f, 1.0f);
		}
		float strongest = 0.0f;
		for (const InputBinding& binding : action.Bindings)
			strongest = std::max(strongest, ReadBinding(binding, frame, action.Type));
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

	bool InputActionMap::ValidateBinding(const InputBinding& binding, std::string& outError)
	{
		if (!std::isfinite(binding.Scale) || binding.Scale < -1.0f || binding.Scale > 1.0f)
		{
			outError = "binding Scale must be a number in [-1, 1]";
			return false;
		}
		if (!std::holds_alternative<GamepadAxis>(binding.Control))
		{
			if (binding.DeadZone != InputBinding::DefaultDeadZone)
			{
				outError = "binding DeadZone applies to GamepadAxis bindings only";
				return false;
			}
		}
		else if (!std::isfinite(binding.DeadZone) || binding.DeadZone < 0.0f || binding.DeadZone > InputBinding::MaxDeadZone)
		{
			outError = fmt::format("binding DeadZone must be a number in [0, {}]", InputBinding::MaxDeadZone);
			return false;
		}
		if (binding.Gamepad && !binding.IsGamepadControl())
		{
			outError = "binding Gamepad applies to GamepadButton and GamepadAxis bindings only";
			return false;
		}
		if (binding.Gamepad && *binding.Gamepad >= Input::MaxGamepads)
		{
			outError = fmt::format("binding Gamepad must be an integer from 0 to {}", Input::MaxGamepads - 1);
			return false;
		}
		return true;
	}

	nlohmann::json InputActionMap::BindingToJson(const InputBinding& binding)
	{
		nlohmann::json data = nlohmann::json::object();
		const std::string key(InputControlKeys[binding.Control.index()]);
		std::visit(
			Overloaded{
				[&](KeyCode control) { data[key] = std::string(KeyCodeToString(control)); },
				[&](MouseCode control) { data[key] = std::string(MouseCodeToString(control)); },
				[&](GamepadButton control) { data[key] = std::string(GamepadButtonToString(control)); },
				[&](GamepadAxis control) { data[key] = std::string(GamepadAxisToString(control)); },
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

	nlohmann::json InputActionMap::BindingsToJson(const std::vector<InputBinding>& bindings)
	{
		nlohmann::json data = nlohmann::json::array();
		for (const InputBinding& binding : bindings)
			data.push_back(BindingToJson(binding));
		return data;
	}

	std::optional<InputBinding> InputActionMap::BindingFromJson(const nlohmann::json& data, std::string& outError)
	{
		if (!data.is_object())
		{
			outError = "a binding must be an object such as {\"Key\": \"Space\"}";
			return std::nullopt;
		}
		std::optional<size_t> controlIndex;
		for (const auto& [key, value] : data.items())
		{
			const auto control = std::ranges::find(InputControlKeys, key);
			if (control != InputControlKeys.end())
			{
				if (controlIndex)
				{
					outError = fmt::format("a binding names one control, not both {} and {}", InputControlKeys[*controlIndex], key);
					return std::nullopt;
				}
				controlIndex = static_cast<size_t>(control - InputControlKeys.begin());
			}
			else if (key != "Scale" && key != "DeadZone" && key != "Gamepad")
			{
				outError = "binding: unknown key '" + key + "'";
				return std::nullopt;
			}
		}
		if (!controlIndex)
		{
			outError = "a binding needs one of Key, MouseButton, GamepadButton or GamepadAxis";
			return std::nullopt;
		}
		const std::string_view controlKey = InputControlKeys[*controlIndex];
		const nlohmann::json& control = data[std::string(controlKey)];
		if (!control.is_string())
		{
			outError = fmt::format("binding {} must be a control name", controlKey);
			return std::nullopt;
		}
		const auto parsed = ParseControl(*controlIndex, control.get<std::string>());
		if (!parsed)
		{
			outError = fmt::format("binding: unknown {} '{}'", controlKey, control.get<std::string>());
			return std::nullopt;
		}
		InputBinding binding;
		binding.Control = *parsed;
		if (!ReadOptionalFloat(data, "Scale", binding.Scale, outError) || !ReadOptionalFloat(data, "DeadZone", binding.DeadZone, outError))
			return std::nullopt;
		if (data.contains("DeadZone") && !std::holds_alternative<GamepadAxis>(binding.Control))
		{
			outError = "binding DeadZone applies to GamepadAxis bindings only";
			return std::nullopt;
		}
		if (const auto gamepad = data.find("Gamepad"); gamepad != data.end())
		{
			uint32_t index = 0;
			if (!JsonToUInt32(*gamepad, index))
			{
				outError = fmt::format("binding Gamepad must be an integer from 0 to {}", Input::MaxGamepads - 1);
				return std::nullopt;
			}
			binding.Gamepad = index;
		}
		if (!ValidateBinding(binding, outError))
			return std::nullopt;
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

	nlohmann::json InputActionMap::ActionsToJson() const
	{
		nlohmann::json actions = nlohmann::json::array();
		for (const InputAction& action : m_Actions)
			actions.push_back({ { "Name", action.Name }, { "Type", std::string(InputActionTypeToString(action.Type)) }, { "Bindings", BindingsToJson(action.Bindings) } });
		return actions;
	}

	nlohmann::json InputActionMap::ToJson() const
	{
		return { { "Format", FormatName }, { "Version", FormatVersion }, { "Actions", ActionsToJson() } };
	}

	std::optional<InputActionMap> InputActionMap::FromJson(const nlohmann::json& data, std::string& outError)
	{
		if (!data.is_object())
		{
			outError = "input actions must be an object {\"Actions\": [...]}";
			return std::nullopt;
		}
		if (!RejectUnknownKeys(data, { "Format", "Version", "Actions" }, "input actions", outError))
			return std::nullopt;
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
		for (size_t i = 0; i < actions->size(); i++)
		{
			const nlohmann::json& entry = (*actions)[i];
			const std::string position = fmt::format("input action #{}", i + 1);
			if (!entry.is_object())
			{
				outError = position + " must be an object {\"Name\", \"Type\", \"Bindings\"}";
				return std::nullopt;
			}
			if (!RejectUnknownKeys(entry, { "Name", "Type", "Bindings" }, position, outError))
				return std::nullopt;
			InputAction action;
			const auto name = entry.find("Name");
			if (name == entry.end() || !name->is_string())
			{
				outError = position + ": Name must be a string";
				return std::nullopt;
			}
			action.Name = name->get<std::string>();
			if (!IsValidName(action.Name, outError))
			{
				outError = position + ": " + outError;
				return std::nullopt;
			}
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
					outError = "input action '" + action.Name + "': Type must be \"Button\" or \"Axis\" (case-sensitive)";
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
