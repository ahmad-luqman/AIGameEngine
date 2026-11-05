#include "Basalt/Animation/PropertyAnimation.h"

#include "Basalt/Asset/AssetKey.h"
#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Asset/MaterialAsset.h"
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
#include <charconv>
#include <map>
#include <string_view>
#include <variant>

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

		bool IsIndex(std::string_view selector)
		{
			return !selector.empty() && std::ranges::all_of(selector, [](char c) { return c >= '0' && c <= '9'; });
		}

		bool HasName(const nlohmann::json& element, const std::string& uniqueField, const std::string& name)
		{
			return element.is_object() && element.contains(uniqueField) && element.at(uniqueField) == name;
		}

		// "a number[5] field", "an enum[3] field", "a list field": what kind of field `type` is, for messages.
		std::string DescribeKind(const ComponentFieldType& type)
		{
			const std::string kind = type.Kind == ComponentFieldKind::List ? "list" : type.ToString();
			return fmt::format("{} {} field", std::string_view("aeiou").find(kind.front()) != std::string_view::npos ? "an" : "a", kind);
		}

		std::string DescribeKind(const PropertyAnimationField& field)
		{
			return field.IsAsset ? "an asset field" : DescribeKind(field.Type);
		}

		// Why a key does not fit its field, or empty.
		std::string CheckKey(const PropertyAnimationField& field, const PropertyAnimationKey& key)
		{
			for (int lane = 0; lane < 4; lane++)
				if (!std::isfinite(key.Value[lane]))
					return "Value must be finite";
			if (key.Text.size() > PropertyAnimationKey::MaxTextLength)
				return fmt::format("Text holds at most {} bytes", PropertyAnimationKey::MaxTextLength);
			if (key.Asset.size() > PropertyAnimationKey::MaxTextLength)
				return fmt::format("Asset holds at most {} bytes", PropertyAnimationKey::MaxTextLength);
			const bool text = !field.IsAsset && (field.Type.Kind == ComponentFieldKind::String || field.Type.Kind == ComponentFieldKind::Enum);
			const char* keyField = field.IsAsset ? "Asset" : text ? "Text"
																  : "Value";
			// Text or Asset on another field would be ignored: say so rather than animate what an agent did not mean.
			if (!text && !key.Text.empty())
				return fmt::format("Text must be empty: {} is {} (keys use {})", field.Leaf, DescribeKind(field), keyField);
			if (!field.IsAsset && !key.Asset.empty())
				return fmt::format("Asset must be empty: {} is {} (keys use {})", field.Leaf, DescribeKind(field), keyField);
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
			if (field.IsAsset)
				return left.Asset;
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
			// Stepped rotations keep their keys' Euler bits: a slerp at blend 0 would still round-trip them.
			if (type.QuaternionRotation && !track.Stepped)
			{
				const auto first = Math::QuatFromEuler(glm::radians(glm::vec3(left.Value)));
				const auto second = Math::QuatFromEuler(glm::radians(glm::vec3(right.Value)));
				const auto degrees = glm::degrees(Math::EulerFromQuat(glm::normalize(glm::slerp(first, second, static_cast<float>(blend)))));
				return { degrees.x, degrees.y, degrees.z };
			}
			const auto lerpLane = [&](int lane) { return static_cast<float>(std::lerp(static_cast<double>(left.Value[lane]), static_cast<double>(right.Value[lane]), blend)); };
			if (type.Count == 0)
				return lerpLane(0);
			nlohmann::json value = nlohmann::json::array();
			for (int lane = 0; lane < static_cast<int>(type.Count); lane++)
				value.push_back(lerpLane(lane));
			return value;
		}

		// Starts loading what asset tracks name, so it loads ahead of its keys rather than when one first shows it. Only the
		// kinds renderers stream: anything else loads when its component reads it.
		void PreloadAssets(const PropertyAnimationComponent& animation)
		{
			for (const PropertyAnimationTrack& track : animation.Tracks)
			{
				const auto field = PropertyAnimation::ResolveField(track.Component, track.Field);
				if (!field || !field->IsAsset)
					continue;
				for (const PropertyAnimationKey& key : track.Keys)
				{
					if (MaterialAsset::IsMaterialKey(key.Asset))
						AssetManager::RequestMaterial(key.Asset);
					else if (AssetManager::IsTextureFile(key.Asset) || GetSubAssetFragment(key.Asset).starts_with(EmbeddedImageMarker))
						AssetManager::RequestTexture(key.Asset);
					else if (AssetManager::IsMeshFile(key.Asset))
						AssetManager::RequestMesh(key.Asset);
				}
			}
		}

		// How a path picks element `index` of `list`: by its unique name when that picks it alone and reads as a name
		// (nonempty, not only digits, without ']'), otherwise by index.
		std::string ElementSelector(const ComponentObjectSchema& element, const nlohmann::json& list, size_t index)
		{
			if (!element.UniqueField.empty())
				if (const auto unique = list[index].find(element.UniqueField); unique != list[index].end() && unique->is_string())
				{
					const std::string& name = unique->get_ref<const std::string&>();
					if (!name.empty() && !IsIndex(name) && name.find(']') == std::string::npos && std::ranges::count_if(list, [&](const nlohmann::json& other) { return HasName(other, element.UniqueField, name); }) == 1)
						return name;
				}
			return std::to_string(index);
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
				const std::string path = prefix + name;
				if (type->second.Kind != ComponentFieldKind::List)
				{
					if (PropertyAnimation::ResolveField(component, path))
						out.push_back(path);
					continue;
				}
				if (!value->is_array())
					continue;
				const ComponentObjectSchema& element = *type->second.Element;
				for (size_t i = 0; i < value->size(); i++)
					if ((*value)[i].is_object())
						CollectFields(component, element.Fields, element.FieldTypes, (*value)[i], fmt::format("{}[{}].", path, ElementSelector(element, *value, i)), out);
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
			if (const size_t* index = std::get_if<size_t>(&step.Selector))
			{
				if (*index >= list.size())
				{
					outError = fmt::format("{} has no element {} (it has {})", step.List, *index, list.size());
					return nullptr;
				}
				node = &list[*index];
			}
			else
			{
				const std::string& name = std::get<std::string>(step.Selector);
				const auto found = std::ranges::find_if(list, [&](const nlohmann::json& element) { return HasName(element, step.UniqueField, name); });
				if (found == list.end())
				{
					outError = fmt::format("{} has no element whose {} is '{}'", step.List, step.UniqueField, name);
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

	std::string ValidatePropertyAnimation(const PropertyAnimationComponent& c, std::vector<PropertyAnimationField>* outFields)
	{
		if (outFields)
			outFields->clear();
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
			auto field = PropertyAnimation::ResolveField(track.Component, track.Field, &problem);
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
			if (outFields)
				outFields->push_back(std::move(*field));
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
		const auto notAPath = [&] { return fail(fmt::format("'{}' is not a field path (Field or List[index or name].Field)", field)); };
		const auto* info = ComponentRegistry::Find(component);
		if (!info)
			return fail(fmt::format("there is no component '{}'", component));
		if (!info->SupportsPropertyAnimation())
			return fail(fmt::format("{} cannot be animated", component));
		// The schema the next name is looked up in: the component's, then each picked element's.
		struct Schema
		{
			const std::map<std::string, ComponentFieldType>* Types;
			const std::map<std::string, std::vector<std::string>>* Enums;
			const std::vector<std::string>* Assets;
		} schema{ &info->FieldTypes, &info->EnumOptions, &info->AssetFields };
		PropertyAnimationField result;
		std::string_view rest = field;
		// The path with its selectors left out, as ComponentInfo::AnimatableFields spells it.
		std::string declared;
		while (true)
		{
			const size_t end = rest.find_first_of("[.");
			const std::string name(rest.substr(0, end));
			if (name.empty())
				return notAPath();
			const auto type = schema.Types->find(name);
			if (type == schema.Types->end())
				return fail(fmt::format("{} has no field '{}'", component, name));
			declared += name;
			if (end == std::string_view::npos)
			{
				result.Leaf = name;
				result.Type = type->second;
				break;
			}
			if (type->second.Kind != ComponentFieldKind::List)
				return fail(fmt::format("{} is not a list, so it takes no [element] or .Field (a track animates a whole field; vector keys hold all its lanes)", name));
			if (rest[end] == '.')
				return fail(fmt::format("{} is a list: pick an element first, e.g. {}[0].{}", name, name, rest.substr(end + 1)));
			const size_t close = rest.find(']', end);
			if (close == std::string_view::npos || close + 1 >= rest.size() || rest[close + 1] != '.')
				return notAPath();
			const std::string_view selector = rest.substr(end + 1, close - end - 1);
			const ComponentObjectSchema& element = *type->second.Element;
			PropertyAnimationField::Step step{ .List = name, .UniqueField = element.UniqueField };
			if (IsIndex(selector))
			{
				// Lists hold at most MaxCount elements, so a larger index (or one past size_t) can never pick one.
				size_t index = 0;
				const auto [parsed, error] = std::from_chars(selector.data(), selector.data() + selector.size(), index);
				if (error != std::errc{} || index >= type->second.MaxCount)
					return fail(fmt::format("{} holds at most {} elements, so [{}] picks none", name, type->second.MaxCount, selector));
				step.Selector = index;
			}
			else if (selector.empty() || element.UniqueField.empty())
				return fail(fmt::format("{} elements have no names, so pick one by index ({}[0])", name, name));
			else
				step.Selector = std::string(selector);
			result.Steps.push_back(std::move(step));
			schema = { &element.FieldTypes, &element.EnumOptions, &element.AssetFields };
			declared += "[].";
			rest = rest.substr(close + 2);
		}
		if (info->AnimatableFields && std::ranges::find(*info->AnimatableFields, declared) == info->AnimatableFields->end())
			return fail(fmt::format("{}.{} rebuilds the physics object when it changes, which ends its contacts or resets its rest pose (a ragdoll's Enabled starts or ends it), so tracks do not animate it (they can animate {})", component, declared, fmt::join(*info->AnimatableFields, ", ")));
		const ComponentFieldType& type = result.Type;
		result.IsAsset = std::ranges::find(*schema.Assets, result.Leaf) != schema.Assets->end();
		// Renaming the element mid-timeline would lose it (and the state systems key by that name).
		if (!result.Steps.empty() && result.Leaf == result.Steps.back().UniqueField)
			return fail(fmt::format("{} names its list element, which tracks do not animate", result.Leaf));
		const bool supported = result.IsDiscrete() ? type.Count == 0 : type.Kind == ComponentFieldKind::Number && type.Count <= 4 && (!type.QuaternionRotation || type.Count == 3);
		if (!supported)
			return fail(fmt::format("{} is {}; tracks animate numbers (up to 4 lanes) and single integers, booleans, strings, enums and asset references", result.Leaf, DescribeKind(type)));
		if (type.Kind == ComponentFieldKind::Enum)
		{
			const auto options = schema.Enums->find(result.Leaf);
			if (options == schema.Enums->end())
				return fail(fmt::format("{} has no enum values", result.Leaf));
			result.EnumOptions = options->second;
		}
		return result;
	}

	std::vector<std::string> PropertyAnimation::ListFields(Entity entity, const std::string& component)
	{
		std::vector<std::string> fields;
		const auto* info = ComponentRegistry::Find(component);
		if (!entity || !info || !info->SupportsPropertyAnimation() || !info->Has(entity))
			return fields;
		CollectFields(component, info->Fields, info->FieldTypes, info->Serialize(entity), "", fields);
		return fields;
	}

	Entity PropertyAnimation::ResolveTarget(Entity timeline, UUID target)
	{
		return target == 0 ? timeline : timeline.GetScene()->GetEntityByUUID(target);
	}

	std::string PropertyAnimation::DescribeMissingTarget(UUID target)
	{
		return target == MissingEntity ? "its entity was deleted from the prefab" : fmt::format("entity {} is not in the scene", static_cast<uint64_t>(target));
	}

	bool PropertyAnimation::Sample(Entity entity, const PropertyAnimationComponent& animation, float time, std::string& outError)
	{
		if (!entity || !std::isfinite(time) || time < 0 || time > 1)
		{
			outError = "sample requires a live entity and normalized time in [0, 1]";
			return false;
		}
		std::vector<PropertyAnimationField> fields;
		outError = ValidatePropertyAnimation(animation, &fields);
		if (!outError.empty())
			return false;
		BS_CORE_ASSERT(fields.size() == animation.Tracks.size(), "a valid timeline resolves every track, in order");
		// Each animated component's JSON as serialized and as the tracks leave it, so tracks into one list compose and the
		// list is patched once.
		struct Working
		{
			Entity Target;
			nlohmann::json Original;
			nlohmann::json Current;
		};
		std::map<std::pair<uint64_t, std::string>, Working> components;
		for (size_t i = 0; i < animation.Tracks.size(); i++)
		{
			const PropertyAnimationTrack& track = animation.Tracks[i];
			if (track.Keys.empty())
				continue;
			const auto location = [&] { return track.Target == 0 ? track.Component + "." + track.Field : fmt::format("{}.{} of entity {}", track.Component, track.Field, static_cast<uint64_t>(track.Target)); };
			const Entity target = ResolveTarget(entity, track.Target);
			if (!target)
			{
				outError = location() + ": " + DescribeMissingTarget(track.Target);
				return false;
			}
			const auto* info = ComponentRegistry::Find(track.Component);
			if (!info->Has(target))
			{
				outError = fmt::format("{}: the entity has no {}", location(), track.Component);
				return false;
			}
			auto [entry, inserted] = components.try_emplace({ static_cast<uint64_t>(target.GetUUID()), track.Component });
			Working& working = entry->second;
			if (inserted)
			{
				working.Target = target;
				working.Original = info->Serialize(target);
				working.Current = working.Original;
			}
			std::string problem;
			nlohmann::json* current = fields[i].Find(working.Current, problem);
			if (!current)
			{
				outError = location() + ": " + problem;
				return false;
			}
			// Held keys and zero-speed timelines must not emit redundant system update signals.
			if (nlohmann::json value = SampleTrack(track, fields[i], time); !SameValue(fields[i].Type, *current, value))
				*current = std::move(value);
		}
		for (const auto& [key, working] : components)
		{
			// Only fields a track changed differ (a list two tracks changed and changed back does not).
			nlohmann::json patch = nlohmann::json::object();
			for (const auto& [name, value] : working.Current.items())
				if (value != working.Original.at(name))
					patch[name] = value;
			if (!patch.empty() && !ComponentRegistry::AddOrPatch(working.Target, key.second, patch, outError))
			{
				if (working.Target != entity)
					outError = fmt::format("entity '{}': {}", working.Target.GetName(), outError);
				return false;
			}
		}
		return true;
	}

	bool PropertyAnimation::Key(Entity entity, const std::string& component, const std::string& field, float time, std::string& outError, UUID target)
	{
		if (!entity || !std::isfinite(time) || time < 0 || time > 1)
		{
			outError = "key requires a live entity and normalized time in [0, 1]";
			return false;
		}
		std::string problem;
		const auto resolved = ResolveField(component, field, &problem);
		const auto fail = [&](const std::string& reason) {
			outError = fmt::format("cannot key {}.{}: {}", component, field, reason);
			return false;
		};
		if (!resolved)
			return fail(problem);
		const Entity owner = ResolveTarget(entity, target);
		if (!owner)
			return fail(DescribeMissingTarget(target));
		const auto* info = ComponentRegistry::Find(component);
		if (!info->Has(owner))
			return fail("the entity has no " + component);
		nlohmann::json data = info->Serialize(owner);
		const nlohmann::json* value = resolved->Find(data, problem);
		if (!value)
			return fail(problem);
		nlohmann::json lanes = nlohmann::json::array({ 0.0f, 0.0f, 0.0f, 0.0f });
		std::string text;
		std::string asset;
		const ComponentFieldType& type = resolved->Type;
		// The registry writes each field as its declared kind; a value of another type is reported, never thrown on.
		const auto mismatched = [&] { return fail(fmt::format("its value {} is not {}", value->dump(), DescribeKind(*resolved))); };
		switch (type.Kind)
		{
			case ComponentFieldKind::Integer:
			{
				if (!value->is_number_unsigned())
					return mismatched();
				const uint64_t integer = value->get<uint64_t>();
				if (integer > PropertyAnimationField::MaxInteger)
					return fail(fmt::format("its value {} is past {}, the largest integer a key holds", integer, PropertyAnimationField::MaxInteger));
				lanes[0] = static_cast<float>(integer);
				break;
			}
			case ComponentFieldKind::Boolean:
				if (!value->is_boolean())
					return mismatched();
				lanes[0] = value->get<bool>() ? 1.0f : 0.0f;
				break;
			case ComponentFieldKind::String:
			case ComponentFieldKind::Enum:
				if (!value->is_string())
					return mismatched();
				{
					std::string& slot = resolved->IsAsset ? asset : text;
					slot = value->get<std::string>();
					if (slot.size() > PropertyAnimationKey::MaxTextLength)
						return fail(fmt::format("its value is longer than {} bytes", PropertyAnimationKey::MaxTextLength));
					break;
				}
			default:
				if (type.Count == 0)
				{
					if (!value->is_number())
						return mismatched();
					lanes[0] = *value;
				}
				else
				{
					if (!value->is_array() || value->size() != type.Count)
						return mismatched();
					for (size_t lane = 0; lane < type.Count; lane++)
						lanes[lane] = (*value)[lane];
				}
				break;
		}
		const auto* animationInfo = ComponentRegistry::Find("PropertyAnimation");
		auto animation = animationInfo->Has(entity) ? animationInfo->Serialize(entity) : nlohmann::json{ { "Tracks", nlohmann::json::array() } };
		auto& tracks = animation["Tracks"];
		// A track of that entity spelled the same way, else one reaching the same field another way ("Layers[0].Weight" keys
		// the track written "Layers[Upper].Weight"). A track of that entity and component that reaches nothing now would fail
		// every sample, so keying past it is refused rather than reported as done (scene.info reports the rest).
		std::string stale;
		const uint64_t self = entity.GetUUID(), wanted = owner.GetUUID();
		const auto reaches = [&](const nlohmann::json& existing) {
			const uint64_t existingTarget = existing.at("Target");
			if (existing.at("Component") != component || (existingTarget == 0 ? self : existingTarget) != wanted)
				return false;
			const std::string& spelling = existing.at("Field").get_ref<const std::string&>();
			std::string unreachable;
			const auto other = ResolveField(component, spelling);
			const nlohmann::json* reached = other ? other->Find(data, unreachable) : nullptr;
			if (!reached && other && !existing.at("Keys").empty() && stale.empty())
				stale = fmt::format("its track {}.{} no longer reaches its field ({}); fix or remove it first", component, spelling, unreachable);
			return reached == value;
		};
		const auto pick = [&](bool sameSpelling) {
			auto track = tracks.end(), fallback = tracks.end();
			for (auto it = tracks.begin(); it != tracks.end(); ++it)
				if (reaches(*it) && (!sameSpelling || it->at("Field") == field))
				{
					fallback = it;
					if (!it->at("Keys").empty())
						track = it;
				}
			return track != tracks.end() ? track : fallback;
		};
		auto track = pick(true);
		if (track == tracks.end())
			track = pick(false);
		if (!stale.empty())
			return fail(stale);
		if (track == tracks.end())
		{
			tracks.push_back({ { "Component", component }, { "Field", field }, { "Target", wanted == self ? 0 : wanted }, { "Stepped", resolved->IsDiscrete() }, { "Keys", nlohmann::json::array() } });
			track = std::prev(tracks.end());
		}
		auto& keys = (*track)["Keys"];
		auto key = std::find_if(keys.begin(), keys.end(), [&](const auto& k) { return k.at("Time") == time; });
		if (key != keys.end())
		{
			(*key)["Value"] = lanes;
			(*key)["Text"] = text;
			(*key)["Asset"] = asset;
		}
		else
			keys.push_back({ { "Time", time }, { "Value", lanes }, { "Text", text }, { "Asset", asset } });
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
		{
			const auto& animation = entity.GetComponent<PropertyAnimationComponent>();
			it->second.Playing = animation.PlayOnStart;
			PreloadAssets(animation);
		}
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
