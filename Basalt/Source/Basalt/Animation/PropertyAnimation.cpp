#include "Basalt/Animation/PropertyAnimation.h"

#include "Basalt/Core/Log.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <algorithm>
#include <cmath>

namespace Basalt {
	std::string ValidatePropertyAnimation(const PropertyAnimationComponent& c)
	{
		if (!std::isfinite(c.Duration) || c.Duration <= 0 || c.Duration > 86400)
			return "Duration must be in (0, 86400]";
		if (!std::isfinite(c.Speed) || c.Speed < 0 || c.Speed > 1000)
			return "Speed must be in [0, 1000]";
		if (c.Tracks.size() > 64)
			return "Tracks holds at most 64 tracks";
		for (const auto& track : c.Tracks)
		{
			if (!PropertyAnimation::FindField(track.Component, track.Field))
				return "Tracks require a registered numeric field outside PropertyAnimation";
			if (track.Keys.size() > 256)
				return "Keys holds at most 256 keys";
			float previous = -1;
			for (const auto& key : track.Keys)
			{
				if (!std::isfinite(key.Time) || key.Time < 0 || key.Time > 1 || key.Time <= previous)
					return "Keys.Time must be strictly increasing in [0, 1]";
				previous = key.Time;
				for (int lane = 0; lane < 4; lane++)
					if (!std::isfinite(key.Value[lane]))
						return "Keys.Value must be finite";
			}
		}
		return {};
	}

	const ComponentFieldType* PropertyAnimation::FindField(const std::string& component, const std::string& field)
	{
		if (component == "PropertyAnimation")
			return nullptr;
		const auto* info = ComponentRegistry::Find(component);
		if (!info)
			return nullptr;
		const auto it = info->FieldTypes.find(field);
		return it != info->FieldTypes.end() && it->second.Kind == ComponentFieldKind::Number && it->second.Count <= 4 ? &it->second : nullptr;
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
		std::map<std::string, nlohmann::json> patches;
		for (const auto& track : animation.Tracks)
		{
			if (track.Keys.empty())
				continue;
			const auto* type = FindField(track.Component, track.Field);
			const auto* info = ComponentRegistry::Find(track.Component);
			if (!type || !info->Has(entity))
			{
				outError = track.Component + "." + track.Field + " must name a numeric field of an existing component";
				return false;
			}
			const auto next = std::upper_bound(track.Keys.begin(), track.Keys.end(), time, [](float value, const PropertyAnimationKey& key) { return value < key.Time; });
			const auto& left = next == track.Keys.begin() ? *next : *std::prev(next);
			const auto& right = next == track.Keys.end() ? left : *next;
			const double blend = track.Stepped || left.Time == right.Time ? 0 : std::clamp((static_cast<double>(time) - left.Time) / (right.Time - left.Time), 0.0, 1.0);
			nlohmann::json value;
			for (int lane = 0; lane < std::max(1, static_cast<int>(type->Count)); lane++)
			{
				const double sample = std::lerp(static_cast<double>(left.Value[lane]), static_cast<double>(right.Value[lane]), blend);
				if (type->Count == 0)
					value = sample;
				else
					value.push_back(sample);
			}
			patches[track.Component][track.Field] = std::move(value);
		}
		for (const auto& [component, patch] : patches)
			if (!ComponentRegistry::AddOrPatch(entity, component, patch, outError))
			{
				outError.insert(0, ": ");
				outError.insert(0, component);
				return false;
			}
		return true;
	}

	bool PropertyAnimation::Key(Entity entity, const std::string& component, const std::string& field, float time, std::string& outError)
	{
		const auto* type = FindField(component, field);
		const auto* info = ComponentRegistry::Find(component);
		if (!entity || !type || !info->Has(entity) || !std::isfinite(time) || time < 0 || time > 1)
		{
			outError = "key requires an existing numeric field and normalized time in [0, 1]";
			return false;
		}
		const auto* animationInfo = ComponentRegistry::Find("PropertyAnimation");
		auto data = animationInfo->Has(entity) ? animationInfo->Serialize(entity) : nlohmann::json{ { "Tracks", nlohmann::json::array() } };
		auto& tracks = data["Tracks"];
		auto track = tracks.end();
		for (auto it = tracks.begin(); it != tracks.end(); ++it)
			if (it->at("Component") == component && it->at("Field") == field)
				track = it;
		if (track == tracks.end())
		{
			tracks.push_back({ { "Component", component }, { "Field", field }, { "Keys", nlohmann::json::array() } });
			track = std::prev(tracks.end());
		}
		const auto value = info->Serialize(entity).at(field);
		nlohmann::json lanes = nlohmann::json::array({ 0.0f, 0.0f, 0.0f, 0.0f });
		if (type->Count == 0)
			lanes[0] = value;
		else
			for (size_t lane = 0; lane < type->Count; lane++)
				lanes[lane] = value[lane];
		auto& keys = (*track)["Keys"];
		auto key = std::find_if(keys.begin(), keys.end(), [&](const auto& k) { return k.at("Time") == time; });
		if (key != keys.end())
			(*key)["Value"] = lanes;
		else
			keys.push_back({ { "Time", time }, { "Value", lanes } });
		std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.at("Time") < b.at("Time"); });
		return ComponentRegistry::AddOrPatch(entity, "PropertyAnimation", data, outError);
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
	nlohmann::json PropertyAnimationSystem::Status(Entity entity)
	{
		if (!entity || entity.GetScene() != &m_Scene || !entity.HasComponent<PropertyAnimationComponent>())
			return nullptr;
		const auto& animation = entity.GetComponent<PropertyAnimationComponent>();
		auto [it, inserted] = m_Clocks.try_emplace(entity.GetUUID());
		if (inserted)
			it->second.Playing = animation.PlayOnStart;
		return { { "time", animation.Duration > 0 ? it->second.Time / animation.Duration : 0 }, { "playing", it->second.Playing && !it->second.Failed }, { "failed", it->second.Failed } };
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
		auto& clock = m_Clocks.at(entity.GetUUID());
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
			return false;
		clock.Time = static_cast<double>(target) * animation.Duration;
		clock.Failed = false;
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
			auto [it, inserted] = m_Clocks.try_emplace(entity.GetUUID());
			if (inserted)
				it->second.Playing = animation.PlayOnStart;
			auto& clock = it->second;
			if (clock.Failed || !clock.Playing)
				continue;
			clock.Time += static_cast<double>(deltaTime) * animation.Speed;
			if (!(animation.Duration > 0) || !std::isfinite(animation.Duration) || !std::isfinite(clock.Time))
			{
				clock.Failed = true;
				BS_CORE_ERROR("Property animation '{}': invalid duration or clock", entity.GetName());
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
				BS_CORE_ERROR("Property animation '{}': {} (edit the PropertyAnimation component to restart)", entity.GetName(), error);
			}
		}
	}
}
