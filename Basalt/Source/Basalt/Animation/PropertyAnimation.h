#pragma once

#include "Basalt/Core/UUID.h"
#include "Basalt/Scene/ComponentRegistry.h"
#include "Basalt/Scene/Entity.h"

#include <entt/entt.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace Basalt {
	class Scene;
	class Entity;
	struct PropertyAnimationComponent;
	struct PropertyAnimationField;
	// Empty on valid declarations; otherwise the component-level or indexed track/key problem. Does not require target
	// components or entities. Keys are checked against their field's kind (finite Values, whole integers, 0/1 booleans,
	// valid enum values, an empty Text on fields other than strings and enums, an empty Asset on fields other than asset
	// references); the field's own ranges are the component's to check when a sample patches it. outFields, when given,
	// gets every track's resolved field, in order (complete only when the result is empty).
	std::string ValidatePropertyAnimation(const PropertyAnimationComponent& animation, std::vector<PropertyAnimationField>* outFields = nullptr);

	// A track's Field resolved against the registry: the animated field's type and the list elements leading to it.
	struct PropertyAnimationField
	{
		// Integer keys hold whole numbers up to this (every one of them is exact in Value's float lanes).
		static constexpr uint32_t MaxInteger = 1u << 24;
		// One list element on the way to the field: the List field holding it, then the element at an index, or the one
		// whose UniqueField (its schema's) equals a name.
		struct Step
		{
			std::string List;
			std::variant<size_t, std::string> Selector;
			std::string UniqueField;
		};

		// Empty for a top-level field.
		std::vector<Step> Steps;
		// The animated field: the component's, or the last element's.
		std::string Leaf;
		ComponentFieldType Type;
		// Enum fields: the valid values.
		std::vector<std::string> EnumOptions;
		// The field names a project asset, so keys hold it in Asset rather than Text.
		bool IsAsset = false;

		// Integer, boolean, string, enum and asset fields: their tracks always step.
		bool IsDiscrete() const;
		// The field's value in the component's JSON, or null with outError when an element on the way is missing.
		nlohmann::json* Find(nlohmann::json& component, std::string& outError) const;
	};

	// Resolves timeline targets and applies samples through the registry's validated patch path.
	class PropertyAnimation
	{
	public:
		// Resolves `field` on `component`: a top-level field ("Translation") or a path through list elements, each picked by
		// index or by its unique name ("Layers[Upper].Weight", "Chains[0].Colliders[1].Name"; a selector of only digits is
		// an index). The field must be a number (up to four lanes; quaternion rotations as their three Euler degrees),
		// integer, boolean, string, enum or asset reference (single values), other than the name that identifies a list
		// element. Components that are not animatable (ComponentInfo::SupportsPropertyAnimation) and fields outside a
		// component's ComponentInfo::LiveFields are refused. nullopt, with the reason in outError, otherwise.
		static std::optional<PropertyAnimationField> ResolveField(const std::string& component, const std::string& field, std::string* outError = nullptr);
		// Every field path of `component` on the entity that ResolveField accepts, in declaration order, with list elements
		// picked by their unique name where it is a usable selector (nonempty, not only digits, without ']', used once),
		// otherwise by index. Empty when the entity lacks the component.
		static std::vector<std::string> ListFields(Entity entity, const std::string& component);
		// The entity a timeline's track with this Target animates: `timeline` for 0, otherwise that entity of its scene,
		// which is an invalid Entity when it is not there.
		static Entity ResolveTarget(Entity timeline, UUID target);
		// Samples normalized time in [0,1]. A missing entity, component or list element fails before anything changes; when
		// a component rejects its patch, components patched before it (in order of target UUID, then name) stay changed.
		// Tracks reaching the same field of the same entity (also through different spellings) apply in order, so the last
		// nonempty one wins.
		static bool Sample(Entity entity, const PropertyAnimationComponent& animation, float time, std::string& outError);
		// Captures a field of `target` (ResolveTarget's), adding PropertyAnimation to `entity` if needed. Inserts/replaces in
		// the last nonempty track of that entity spelled the same way, else in the last nonempty one reaching the same field
		// another way, else in the last such empty one; a new track of a discrete field is Stepped. Fails while a keyed track
		// of the same entity and component no longer reaches its field.
		static bool Key(Entity entity, const std::string& component, const std::string& field, float time, std::string& outError, UUID target = 0);
	};

	// Session-owned property clocks. PropertyAnimation component updates restart a timeline; clocks never serialize. A new
	// clock starts loading what its asset tracks name, so a flipbook does not show a placeholder per frame in renderers that
	// stream.
	class PropertyAnimationSystem
	{
	public:
		explicit PropertyAnimationSystem(Scene& scene);
		~PropertyAnimationSystem();
		void Update(float deltaTime);
		// Play restarts, Stop samples zero and stops, Seek preserves playing/stopped state. Failures stop
		// the clock; successful Play/Stop/Seek repairs it. Pause/Resume reject failed clocks.
		bool Control(Entity entity, const std::string& action, float time, std::string& outError);
		// Read-only status (normalized time, playing, failed, error), or null for a foreign/missing entity.
		nlohmann::json Status(Entity entity) const;

	private:
		void Reset(entt::registry& registry, entt::entity entity);
		struct Clock
		{
			double Time = 0;
			bool Failed = false;
			bool Playing = true;
			std::string Error;
		};
		Clock& ClockFor(Entity entity);
		Scene& m_Scene;
		std::unordered_map<UUID, Clock> m_Clocks;
	};
}
