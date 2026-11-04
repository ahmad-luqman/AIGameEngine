#include "Basalt/Animation/PropertyAnimation.h"

#include "Basalt/Core/Log.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/ranges.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string_view>

namespace Basalt {
	namespace {
		// Euler serialization can round-trip to different bits for the same orientation. Compare rotations in
		// quaternion space within float-roundoff tolerance, while other fields retain exact comparisons.
		bool SameValue(const ComponentFieldType& type, const nlohmann::json& first, const nlohmann::json& second)
		{
			if (first == second)
				return true;
			if (!type.QuaternionRotation || !first.is_array() || !second.is_array() || first.size() != 3 || second.size() != 3)
				return false;
			glm::vec3 a, b;
			for (size_t lane = 0; lane < 3; lane++)
				if (!JsonToFloat(first[lane], a[static_cast<int>(lane)]) || !JsonToFloat(second[lane], b[static_cast<int>(lane)]))
					return false;
			const auto qa = Math::QuatFromEuler(glm::radians(a)), qb = Math::QuatFromEuler(glm::radians(b));
			const float sign = glm::dot(qa, qb) < 0 ? -1.0f : 1.0f;
			for (int lane = 0; lane < 4; lane++)
				if (std::abs(qa[lane] - sign * qb[lane]) > 8 * std::numeric_limits<float>::epsilon())
					return false;
			return true;
		}

		// Lanes a key's Value uses for the field (scalars use X).
		int UsedLanes(const ComponentFieldType& type)
		{
			return std::max(1, static_cast<int>(type.Count));
		}

		bool IsIndex(std::string_view selector)
		{
			return !selector.empty() && std::ranges::all_of(selector, [](char c) { return c >= '0' && c <= '9'; });
		}

		// Why a key does not fit its field, or empty.
		std::string CheckKey(const PropertyAnimationField& field, const PropertyAnimationKey& key)
		{
			for (int lane = 0; lane < 4; lane++)
				if (!std::isfinite(key.Value[lane]))
					return "Value must be finite";
			if (key.Text.size() > PropertyAnimationKey::MaxTextLength)
				return fmt::format("Text holds at most {} bytes", PropertyAnimationKey::MaxTextLength);
			switch (field.Type.Kind)
			{
				case ComponentFieldKind::Integer:
					if (key.Value.x < 0 || key.Value.x > static_cast<float>(PropertyAnimationField::MaxInteger) || key.Value.x != std::floor(key.Value.x))
						return fmt::format("Value.x must be a whole number between 0 and {}", PropertyAnimationField::MaxInteger);
					break;
				case ComponentFieldKind::Boolean:
					if (key.Value.x != 0.0f && key.Value.x != 1.0f)
						return "Value.x must be 0 (false) or 1 (true)";
					break;
				case ComponentFieldKind::Enum:
					if (std::ranges::find(field.EnumOptions, key.Text) == field.EnumOptions.end())
						return fmt::format("Text '{}' is not a valid value (valid: {})", key.Text, fmt::join(field.EnumOptions, ", "));
					break;
				default:
					break;
			}
			return {};
		}

		// The track's value at normalized `time` as the field's JSON.
		nlohmann::json SampleTrack(const PropertyAnimationTrack& track, const PropertyAnimationField& field, float time)
		{
			const auto next = std::upper_bound(track.Keys.begin(), track.Keys.end(), time, [](float value, const PropertyAnimationKey& key) { return value < key.Time; });
			const auto& left = next == track.Keys.begin() ? *next : *std::prev(next);
			const auto& right = next == track.Keys.end() ? left : *next;
			const ComponentFieldType& type = field.Type;
			const int lanes = UsedLanes(type);
			switch (type.Kind)
			{
				case ComponentFieldKind::Integer:
					return static_cast<uint32_t>(left.Value.x);
				case ComponentFieldKind::Boolean:
					return left.Value.x != 0.0f;
				case ComponentFieldKind::String:
				case ComponentFieldKind::Enum:
					return left.Text;
				default:
					break;
			}
			const double blend = track.Stepped || left.Time == right.Time ? 0 : std::clamp((static_cast<double>(time) - left.Time) / (right.Time - left.Time), 0.0, 1.0);
			if (type.QuaternionRotation && !track.Stepped)
			{
				const auto first = Math::QuatFromEuler(glm::radians(glm::vec3(left.Value)));
				const auto second = Math::QuatFromEuler(glm::radians(glm::vec3(right.Value)));
				const auto degrees = glm::degrees(Math::EulerFromQuat(glm::normalize(glm::slerp(first, second, static_cast<float>(blend)))));
				return { degrees.x, degrees.y, degrees.z };
			}
			nlohmann::json value;
			for (int lane = 0; lane < lanes; lane++)
			{
				const float sample = static_cast<float>(std::lerp(static_cast<double>(left.Value[lane]), static_cast<double>(right.Value[lane]), blend));
				if (type.Count == 0)
					value = sample;
				else
					value.push_back(sample);
			}
			return value;
		}

		// Appends the paths of `fields` (described by `types`) found in `data` that `component` can animate.
		void CollectFields(const std::string& component, const std::vector<std::string>& fields, const std::map<std::string, ComponentFieldType>& types, const nlohmann::json& data, const std::string& prefix, std::vector<std::string>& out)
		{
			for (const std::string& name : fields)
			{
				const auto type = types.find(name);
				const auto value = data.find(name);
				if (type == types.end() || value == data.end())
					continue;
				if (type->second.Kind != ComponentFieldKind::List)
				{
					if (PropertyAnimation::ResolveField(component, prefix + name))
						out.push_back(prefix + name);
					continue;
				}
				const ComponentObjectSchema& element = *type->second.Element;
				if (!value->is_array())
					continue;
				for (size_t i = 0; i < value->size(); i++)
				{
					const nlohmann::json& item = (*value)[i];
					if (!item.is_object())
						continue;
					// The element's name, when it picks this element and no other.
					std::string selector = std::to_string(i);
					if (!element.UniqueField.empty())
						if (const auto unique = item.find(element.UniqueField); unique != item.end() && unique->is_string())
						{
							const auto& candidate = unique->get_ref<const std::string&>();
							const auto sameName = [&](const nlohmann::json& other) { return other.is_object() && other.contains(element.UniqueField) && other.at(element.UniqueField) == candidate; };
							if (!candidate.empty() && !IsIndex(candidate) && candidate.find(']') == std::string::npos && std::ranges::count_if(*value, sameName) == 1)
								selector = candidate;
						}
					CollectFields(component, element.Fields, element.FieldTypes, item, fmt::format("{}{}[{}].", prefix, name, selector), out);
				}
			}
		}
	}

	bool PropertyAnimationField::IsDiscrete() const
	{
		return Type.Kind == ComponentFieldKind::Integer || Type.Kind == ComponentFieldKind::Boolean || Type.Kind == ComponentFieldKind::String || Type.Kind == ComponentFieldKind::Enum;
	}

	nlohmann::json* PropertyAnimationField::Find(nlohmann::json& component, std::string& outError) const
	{
		nlohmann::json* node = &component;
		for (const Step& step : Steps)
		{
			if (!node->is_object() || !node->contains(step.List) || !node->at(step.List).is_array())
			{
				outError = fmt::format("there is no list {}", step.List);
				return nullptr;
			}
			nlohmann::json& list = node->at(step.List);
			if (step.Index)
			{
				if (*step.Index >= list.size())
				{
					outError = fmt::format("{} has no element {} (it has {})", step.List, *step.Index, list.size());
					return nullptr;
				}
				node = &list[*step.Index];
			}
			else
			{
				const auto found = std::ranges::find_if(list, [&](const nlohmann::json& element) { return element.is_object() && element.contains(step.UniqueField) && element.at(step.UniqueField) == step.Name; });
				if (found == list.end())
				{
					outError = fmt::format("{} has no element whose {} is '{}'", step.List, step.UniqueField, step.Name);
					return nullptr;
				}
				node = &*found;
			}
		}
		if (!node->is_object() || !node->contains(Leaf))
		{
			outError = fmt::format("there is no field {}", Leaf);
			return nullptr;
		}
		return &node->at(Leaf);
	}

	std::string ValidatePropertyAnimation(const PropertyAnimationComponent& c)
	{
		if (!std::isfinite(c.Duration) || c.Duration <= 0 || c.Duration > 86400)
			return "Duration must be in (0, 86400]";
		if (!std::isfinite(c.Speed) || c.Speed < 0 || c.Speed > 1000)
			return "Speed must be in [0, 1000]";
		if (c.Tracks.size() > PropertyAnimationComponent::MaxTracks)
			return "Tracks holds at most 64 tracks";
		for (size_t i = 0; i < c.Tracks.size(); i++)
		{
			const auto& track = c.Tracks[i];
			const auto location = [&] { return fmt::format("Tracks[{}] ({}.{})", i, track.Component, track.Field); };
			std::string problem;
			const auto field = PropertyAnimation::ResolveField(track.Component, track.Field, &problem);
			if (!field)
				return location() + ": " + problem;
			if (track.Keys.size() > PropertyAnimationComponent::MaxKeys)
				return location() + ": Keys holds at most 256 keys";
			float previous = -1;
			for (size_t j = 0; j < track.Keys.size(); j++)
			{
				const auto& key = track.Keys[j];
				const auto keyLocation = [&] { return fmt::format("{}.Keys[{}]", location(), j); };
				if (!std::isfinite(key.Time) || key.Time < 0 || key.Time > 1 || key.Time <= previous)
					return keyLocation() + ": Time must be strictly increasing in [0, 1]";
				previous = key.Time;
				if (std::string keyProblem = CheckKey(*field, key); !keyProblem.empty())
					return keyLocation() + ": " + keyProblem;
			}
		}
		return {};
	}

	std::optional<PropertyAnimationField> PropertyAnimation::ResolveField(const std::string& component, const std::string& field, std::string* outError)
	{
		const auto fail = [&](std::string problem) -> std::optional<PropertyAnimationField> {
			if (outError)
				*outError = std::move(problem);
			return std::nullopt;
		};
		const auto* info = ComponentRegistry::Find(component);
		if (!info)
			return fail(fmt::format("there is no component '{}'", component));
		if (!info->SupportsPropertyAnimation)
			return fail(fmt::format("{} cannot be animated (physics configuration and PropertyAnimation are excluded)", component));
		const std::map<std::string, ComponentFieldType>* types = &info->FieldTypes;
		const std::map<std::string, std::vector<std::string>>* enums = &info->EnumOptions;
		const std::vector<std::string>* assets = &info->AssetFields;
		std::string unique;
		PropertyAnimationField result;
		std::string_view rest = field;
		while (true)
		{
			const size_t end = rest.find_first_of("[.");
			const std::string name(rest.substr(0, end));
			if (name.empty())
				return fail(fmt::format("'{}' is not a field path (Field or List[index or name].Field)", field));
			const auto type = types->find(name);
			if (type == types->end())
				return fail(fmt::format("{} has no field '{}'", component, name));
			if (end == std::string_view::npos)
			{
				result.Leaf = name;
				result.Type = type->second;
				break;
			}
			if (rest[end] == '.' || type->second.Kind != ComponentFieldKind::List)
				return fail(fmt::format("'{}' is not a list, so '{}' cannot pick an element of it", name, field));
			const size_t close = rest.find(']', end);
			if (close == std::string_view::npos || close + 1 >= rest.size() || rest[close + 1] != '.')
				return fail(fmt::format("'{}' is not a field path (Field or List[index or name].Field)", field));
			const std::string_view selector = rest.substr(end + 1, close - end - 1);
			const ComponentObjectSchema& element = *type->second.Element;
			PropertyAnimationField::Step step{ .List = name, .UniqueField = element.UniqueField };
			if (IsIndex(selector))
			{
				// Lists hold at most MaxCount elements, so a longer index can never pick one.
				if (selector.size() > 9 || std::stoul(std::string(selector)) >= type->second.MaxCount)
					return fail(fmt::format("{} holds at most {} elements, so [{}] picks none", name, type->second.MaxCount, selector));
				step.Index = std::stoul(std::string(selector));
			}
			else if (selector.empty() || element.UniqueField.empty())
				return fail(fmt::format("{} elements have no names, so pick one by index ({}[0])", name, name));
			else
				step.Name = selector;
			result.Steps.push_back(std::move(step));
			types = &element.FieldTypes;
			enums = &element.EnumOptions;
			assets = &element.AssetFields;
			unique = element.UniqueField;
			rest = rest.substr(close + 2);
		}
		const ComponentFieldType& type = result.Type;
		if (std::ranges::find(*assets, result.Leaf) != assets->end())
			return fail(fmt::format("{} names an asset, which tracks do not animate", result.Leaf));
		if (!unique.empty() && result.Leaf == unique)
			return fail(fmt::format("{} names its list element, which tracks do not animate", result.Leaf));
		bool supported = false;
		switch (type.Kind)
		{
			case ComponentFieldKind::Number:
				supported = type.Count <= 4 && (!type.QuaternionRotation || type.Count == 3);
				break;
			case ComponentFieldKind::Boolean:
			case ComponentFieldKind::Integer:
			case ComponentFieldKind::String:
			case ComponentFieldKind::Enum:
				supported = type.Count == 0;
				break;
			default:
				break;
		}
		if (!supported)
			return fail(fmt::format("{} is {} field; tracks animate numbers (up to 4 lanes), integers, booleans, strings and enums", result.Leaf, type.ToString() == "object[]" ? "a list" : "an " + type.ToString()));
		if (type.Kind == ComponentFieldKind::Enum)
		{
			const auto options = enums->find(result.Leaf);
			if (options == enums->end())
				return fail(fmt::format("{} has no enum values", result.Leaf));
			result.EnumOptions = options->second;
		}
		return result;
	}

	std::vector<std::string> PropertyAnimation::ListFields(Entity entity, const std::string& component)
	{
		std::vector<std::string> fields;
		const auto* info = ComponentRegistry::Find(component);
		if (!entity || !info || !info->SupportsPropertyAnimation || !info->Has(entity))
			return fields;
		CollectFields(component, info->Fields, info->FieldTypes, info->Serialize(entity), "", fields);
		return fields;
	}

	bool PropertyAnimation::Sample(Entity entity, const PropertyAnimationComponent& animation, float time, std::string& outError)
	{
		if (!entity || !std::isfinite(time) || time < 0 || time > 1)
		{
			outError = "sample requires a live entity and normalized time in [0, 1]";
			return false;
		}
		outError = ValidatePropertyAnimation(animation);
		if (!outError.empty())
			return false;
		// Each component's JSON as serialized and as the tracks leave it, and the top-level fields they wrote, so tracks
		// into one list compose and the list is patched once.
		struct Working
		{
			nlohmann::json Original;
			nlohmann::json Current;
			std::set<std::string> Touched;
		};
		std::map<std::string, Working> components;
		for (const auto& track : animation.Tracks)
		{
			if (track.Keys.empty())
				continue;
			const auto location = [&] { return track.Component + "." + track.Field; };
			// ValidatePropertyAnimation resolved every track.
			const auto field = ResolveField(track.Component, track.Field);
			const auto* info = ComponentRegistry::Find(track.Component);
			if (!field || !info->Has(entity))
			{
				outError = location() + " must name a field of an existing component";
				return false;
			}
			auto [entry, inserted] = components.try_emplace(track.Component);
			Working& working = entry->second;
			if (inserted)
			{
				working.Original = info->Serialize(entity);
				working.Current = working.Original;
			}
			std::string problem;
			nlohmann::json* current = field->Find(working.Current, problem);
			if (!current)
			{
				outError = location() + ": " + problem;
				return false;
			}
			nlohmann::json value = SampleTrack(track, *field, time);
			// Held keys and zero-speed timelines must not emit redundant system update signals.
			if (!SameValue(field->Type, *current, value))
			{
				*current = std::move(value);
				working.Touched.insert(field->TopField());
			}
		}
		for (const auto& [component, working] : components)
		{
			nlohmann::json patch = nlohmann::json::object();
			for (const std::string& top : working.Touched)
				if (working.Current.at(top) != working.Original.at(top))
					patch[top] = working.Current.at(top);
			if (!patch.empty() && !ComponentRegistry::AddOrPatch(entity, component, patch, outError))
				return false;
		}
		return true;
	}

	bool PropertyAnimation::Key(Entity entity, const std::string& component, const std::string& field, float time, std::string& outError)
	{
		std::string problem;
		const auto resolved = ResolveField(component, field, &problem);
		if (!entity || !std::isfinite(time) || time < 0 || time > 1)
		{
			outError = "key requires a live entity and normalized time in [0, 1]";
			return false;
		}
		if (!resolved)
		{
			outError = fmt::format("cannot key {}.{}: {}", component, field, problem);
			return false;
		}
		const auto* info = ComponentRegistry::Find(component);
		if (!info->Has(entity))
		{
			outError = fmt::format("cannot key {}.{}: the entity has no {}", component, field, component);
			return false;
		}
		nlohmann::json data = info->Serialize(entity);
		const nlohmann::json* value = resolved->Find(data, problem);
		if (!value)
		{
			outError = fmt::format("cannot key {}.{}: {}", component, field, problem);
			return false;
		}
		nlohmann::json lanes = nlohmann::json::array({ 0.0f, 0.0f, 0.0f, 0.0f });
		std::string text;
		const ComponentFieldType& type = resolved->Type;
		switch (type.Kind)
		{
			case ComponentFieldKind::Integer:
				if (value->get<uint64_t>() > PropertyAnimationField::MaxInteger)
				{
					outError = fmt::format("cannot key {}.{}: its value {} is past {}, the largest integer a key holds", component, field, value->get<uint64_t>(), PropertyAnimationField::MaxInteger);
					return false;
				}
				lanes[0] = static_cast<float>(value->get<uint64_t>());
				break;
			case ComponentFieldKind::Boolean:
				lanes[0] = value->get<bool>() ? 1.0f : 0.0f;
				break;
			case ComponentFieldKind::String:
			case ComponentFieldKind::Enum:
				text = value->get<std::string>();
				if (text.size() > PropertyAnimationKey::MaxTextLength)
				{
					outError = fmt::format("cannot key {}.{}: its value is longer than {} bytes", component, field, PropertyAnimationKey::MaxTextLength);
					return false;
				}
				break;
			default:
				if (type.Count == 0)
					lanes[0] = *value;
				else
					for (size_t lane = 0; lane < type.Count; lane++)
						lanes[lane] = (*value)[lane];
				break;
		}
		const auto* animationInfo = ComponentRegistry::Find("PropertyAnimation");
		auto animation = animationInfo->Has(entity) ? animationInfo->Serialize(entity) : nlohmann::json{ { "Tracks", nlohmann::json::array() } };
		auto& tracks = animation["Tracks"];
		auto track = tracks.end(), fallback = tracks.end();
		for (auto it = tracks.begin(); it != tracks.end(); ++it)
			if (it->at("Component") == component && it->at("Field") == field)
			{
				fallback = it;
				if (!it->at("Keys").empty())
					track = it;
			}
		if (track == tracks.end())
			track = fallback;
		if (track == tracks.end())
		{
			tracks.push_back({ { "Component", component }, { "Field", field }, { "Stepped", resolved->IsDiscrete() }, { "Keys", nlohmann::json::array() } });
			track = std::prev(tracks.end());
		}
		auto& keys = (*track)["Keys"];
		auto key = std::find_if(keys.begin(), keys.end(), [&](const auto& k) { return k.at("Time") == time; });
		if (key != keys.end())
		{
			(*key)["Value"] = lanes;
			(*key)["Text"] = text;
		}
		else
			keys.push_back({ { "Time", time }, { "Value", lanes }, { "Text", text } });
		std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.at("Time") < b.at("Time"); });
		return ComponentRegistry::AddOrPatch(entity, "PropertyAnimation", animation, outError);
	}

	PropertyAnimationSystem::PropertyAnimationSystem(Scene& scene)
		: m_Scene(scene)
	{
		auto& registry = m_Scene.GetRegistry();
		registry.on_update<PropertyAnimationComponent>().connect<&PropertyAnimationSystem::Reset>(*this);
		registry.on_destroy<PropertyAnimationComponent>().connect<&PropertyAnimationSystem::Reset>(*this);
		// Registry destruction does not promise which component pool is erased first.
		registry.on_destroy<IDComponent>().connect<&PropertyAnimationSystem::Reset>(*this);
	}
	PropertyAnimationSystem::~PropertyAnimationSystem()
	{
		auto& registry = m_Scene.GetRegistry();
		registry.on_update<PropertyAnimationComponent>().disconnect(this);
		registry.on_destroy<PropertyAnimationComponent>().disconnect(this);
		registry.on_destroy<IDComponent>().disconnect(this);
	}
	void PropertyAnimationSystem::Reset(entt::registry& registry, entt::entity entity)
	{
		if (const auto* id = registry.try_get<IDComponent>(entity))
			m_Clocks.erase(id->ID);
	}
	PropertyAnimationSystem::Clock& PropertyAnimationSystem::ClockFor(Entity entity)
	{
		auto [it, inserted] = m_Clocks.try_emplace(entity.GetUUID());
		if (inserted)
			it->second.Playing = entity.GetComponent<PropertyAnimationComponent>().PlayOnStart;
		return it->second;
	}
	nlohmann::json PropertyAnimationSystem::Status(Entity entity) const
	{
		if (!entity || entity.GetScene() != &m_Scene || !entity.HasComponent<PropertyAnimationComponent>())
			return nullptr;
		const auto& animation = entity.GetComponent<PropertyAnimationComponent>();
		const auto it = m_Clocks.find(entity.GetUUID());
		if (it == m_Clocks.end())
			return { { "time", 0.0f }, { "playing", animation.PlayOnStart }, { "failed", false }, { "error", "" } };
		const auto& clock = it->second;
		return { { "time", animation.Duration > 0 ? clock.Time / animation.Duration : 0 }, { "playing", clock.Playing && !clock.Failed }, { "failed", clock.Failed }, { "error", clock.Error } };
	}
	bool PropertyAnimationSystem::Control(Entity entity, const std::string& action, float time, std::string& outError)
	{
		if (Status(entity).is_null())
		{
			outError = "control requires a PropertyAnimation entity in the running scene";
			return false;
		}
		if (action != "Play" && action != "Pause" && action != "Resume" && action != "Stop" && action != "Seek")
		{
			outError = "action must be Play, Pause, Resume, Stop or Seek";
			return false;
		}
		if (!std::isfinite(time) || time < 0 || time > 1)
		{
			outError = "time must be normalized in [0, 1]";
			return false;
		}
		auto& clock = ClockFor(entity);
		if (clock.Failed && (action == "Pause" || action == "Resume"))
		{
			outError = "timeline suspended after a failed sample; Play/Stop/Seek or edit PropertyAnimation to repair: " + clock.Error;
			return false;
		}
		const auto& animation = entity.GetComponent<PropertyAnimationComponent>();
		if (action == "Pause")
		{
			clock.Playing = false;
			return true;
		}
		if (action == "Resume")
		{
			clock.Playing = true;
			return true;
		}
		const float target = action == "Seek" ? time : 0;
		if (!PropertyAnimation::Sample(entity, animation, target, outError))
		{
			clock.Failed = true;
			clock.Playing = false;
			clock.Error = outError;
			return false;
		}
		clock.Time = static_cast<double>(target) * animation.Duration;
		clock.Failed = false;
		clock.Error.clear();
		if (action != "Seek")
			clock.Playing = action == "Play";
		return true;
	}
	void PropertyAnimationSystem::Update(float deltaTime)
	{
		if (!std::isfinite(deltaTime) || deltaTime < 0)
			return;
		for (auto handle : m_Scene.GetAllEntitiesWith<PropertyAnimationComponent>())
		{
			Entity entity(handle, &m_Scene);
			const auto& animation = entity.GetComponent<PropertyAnimationComponent>();
			auto& clock = ClockFor(entity);
			if (clock.Failed || !clock.Playing)
				continue;
			clock.Time += static_cast<double>(deltaTime) * animation.Speed;
			if (!(animation.Duration > 0) || !std::isfinite(animation.Duration) || !std::isfinite(clock.Time))
			{
				clock.Failed = true;
				clock.Playing = false;
				clock.Error = "invalid duration or clock";
				BS_CORE_ERROR("Property animation '{}' ({}): {}", entity.GetName(), static_cast<uint64_t>(entity.GetUUID()), clock.Error);
				continue;
			}
			if (animation.Looping)
				clock.Time = std::fmod(clock.Time, animation.Duration);
			else
			{
				clock.Time = std::min(clock.Time, static_cast<double>(animation.Duration));
				if (clock.Time >= animation.Duration)
					clock.Playing = false;
			}
			std::string error;
			if (!PropertyAnimation::Sample(entity, animation, static_cast<float>(clock.Time / animation.Duration), error))
			{
				clock.Failed = true;
				clock.Playing = false;
				clock.Error = error;
				BS_CORE_ERROR("Property animation '{}' ({}): {} (Play/Stop/Seek or edit PropertyAnimation to repair)", entity.GetName(), static_cast<uint64_t>(entity.GetUUID()), error);
			}
		}
	}
}
