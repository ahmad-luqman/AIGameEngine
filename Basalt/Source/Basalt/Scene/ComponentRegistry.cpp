#include "Basalt/Scene/ComponentRegistry.h"
#include "Basalt/Animation/PropertyAnimation.h"

#include "Basalt/Asset/MaterialAsset.h"
#include "Basalt/Audio/AudioRules.h"
#include "Basalt/Core/EnumNames.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Particles/ParticleEmitterRules.h"
#include "Basalt/Physics/PhysicsLayers.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Renderer/LocalShadows.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/UI/UIRules.h"

#include <glm/gtc/type_ptr.hpp>
#include <spdlog/fmt/ranges.h>

#include <algorithm>
#include <set>
#include <array>
#include <cmath>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace Basalt {

	namespace {

		using json = nlohmann::json;

		// Reads named fields from a JSON object into a component, tracking which keys were used so that
		// unknown keys can be reported. In collect mode it only records field names.
		class FieldReader
		{
		public:
			// `prefix` qualifies field names in error messages for list elements, e.g. "Events[2].".
			explicit FieldReader(const json* data, std::string prefix = {})
				: m_Data(data)
				, m_Prefix(std::move(prefix))
			{
			}

			std::vector<std::string>& GetFieldNames() { return m_FieldNames; }
			std::map<std::string, std::vector<std::string>>& GetEnumOptions() { return m_EnumOptions; }
			std::vector<std::string>& GetAssetFields() { return m_AssetFields; }
			std::vector<std::string>& GetEntityFields() { return m_EntityFields; }
			std::map<std::string, ComponentFieldType>& GetFieldTypes() { return m_FieldTypes; }

			template<typename T>
			void Field(const char* name, T& value)
			{
				Describe(name, TypeOf<T>());
				const json* element = Find(name);
				if (element)
					ReadQualified(name, *element, value);
			}

			// Asset readers accept both legacy paths and persistent UUID/path references, on every data path.
			void AssetField(const char* name, std::string& value)
			{
				Describe(name, { ComponentFieldKind::String });
				if (IsCollecting())
					m_AssetFields.emplace_back(name);
				const json* element = Find(name);
				if (!element)
					return;
				if (element->is_string())
				{
					ReadQualified(name, *element, value);
					return;
				}
				std::string error;
				const auto& project = Project::GetActive();
				if (!project)
					throw std::runtime_error(Qualify(name) + ": UUID asset references require an active project");
				if (!project->GetAssets().Decode(*element, value, error, true))
					throw std::runtime_error(Qualify(name) + ": " + error);
				if (!error.empty())
					BS_CORE_WARN("Asset field {}: {}", Qualify(name), error);
			}

			// A field holding another entity's UUID (0 = none). Recorded so tools can resolve names and
			// copies can remap the reference.
			void Field(const char* name, UUID& value)
			{
				Describe(name, { ComponentFieldKind::Entity });
				const json* element = Find(name);
				if (element)
					ReadQualified(name, *element, value);
			}

			// A variable-length array of objects, each read with Fields(FieldReader&, E&) from a default E (so
			// missing keys keep their defaults). The whole list is replaced; an empty object counts as an empty
			// list (Lua cannot tell the two apart). E names itself with a static TypeName (and may name a
			// UniqueField). Defined below, after the element declarations it needs.
			template<typename E>
			void ListField(const char* name, std::vector<E>& value, uint32_t maxCount);

			// Rotation stored as Euler angles in degrees.
			void RotationField(const char* name, glm::quat& value)
			{
				Describe(name, { ComponentFieldKind::Number, 3 });
				const json* element = Find(name);
				if (!element)
					return;
				glm::vec3 degrees;
				ReadQualified(name, *element, degrees);
				value = Math::QuatFromEuler(glm::radians(degrees));
			}

			// Angle stored in degrees, held in radians.
			void AngleField(const char* name, float& radiansValue)
			{
				Describe(name, { ComponentFieldKind::Number });
				const json* element = Find(name);
				if (!element)
					return;
				float degrees = 0.0f;
				ReadQualified(name, *element, degrees);
				radiansValue = glm::radians(degrees);
			}

			template<typename E, size_t N>
			void EnumField(const char* name, E& value, const char* const (&names)[N])
			{
				DescribeEnum(name, { ComponentFieldKind::Enum }, names);
				const json* element = Find(name);
				if (element)
					value = ReadEnum<E>(Qualify(name).c_str(), *element, names);
			}

			// One enum value per axis, written as an array of names (e.g. ["Off", "Position", "Off"]). The
			// whole array is validated before any element changes.
			template<typename E, size_t Count, size_t N>
			void EnumArrayField(const char* name, std::array<E, Count>& value, const char* const (&names)[N])
			{
				static_assert(Count <= UINT8_MAX, "ComponentFieldType::Count is 8-bit");
				DescribeEnum(name, { ComponentFieldKind::Enum, static_cast<uint8_t>(Count) }, names);
				const json* element = Find(name);
				if (!element)
					return;
				const std::string qualified = Qualify(name);
				if (!element->is_array() || element->size() != Count)
					throw std::runtime_error("field '" + qualified + "' must be an array of " + std::to_string(Count) + " strings");
				std::array<E, Count> result;
				for (size_t i = 0; i < Count; i++)
					result[i] = ReadEnum<E>(qualified.c_str(), (*element)[i], names);
				value = result;
			}

			// Older files may store a field in a form (or under a name) that is no longer written. When
			// `isLegacy` accepts the stored value it is consumed and passed to `convert` instead of being read
			// normally; the field is not listed among the component's fields. Returns whether that happened.
			template<typename IsLegacy, typename Convert>
			bool LegacyField(const char* name, IsLegacy&& isLegacy, Convert&& convert)
			{
				if (!m_Data)
					return false;
				auto it = m_Data->find(name);
				if (it == m_Data->end() || !isLegacy(*it))
					return false;
				m_Consumed.insert(name);
				convert(*it);
				return true;
			}

			void CheckUnknownKeys(const std::vector<std::string>& validFields) const
			{
				if (!m_Data)
					return;
				for (auto it = m_Data->begin(); it != m_Data->end(); ++it)
				{
					if (m_Consumed.contains(it.key()))
						continue;
					std::string valid;
					for (size_t i = 0; i < validFields.size(); i++)
						valid += (i ? ", " : "") + validFields[i];
					throw std::runtime_error("unknown field '" + Qualify(it.key().c_str()) + "' (valid: " + valid + ")");
				}
			}

		private:
			// Collect mode (no data) records what tools need about each field; reads skip that bookkeeping.
			bool IsCollecting() const { return m_Data == nullptr; }

			std::string Qualify(const char* name) const { return m_Prefix + name; }

			template<typename T>
			void ReadQualified(const char* name, const json& element, T& value) const
			{
				Read(Qualify(name).c_str(), element, value);
			}

			// Records a field in collect mode: its name (in declaration order), type, and whether it names an entity.
			void Describe(const char* name, const ComponentFieldType& type)
			{
				if (!IsCollecting())
					return;
				m_FieldNames.emplace_back(name);
				m_FieldTypes[name] = type;
				if (type.Kind == ComponentFieldKind::Entity)
					m_EntityFields.emplace_back(name);
			}

			template<size_t N>
			void DescribeEnum(const char* name, const ComponentFieldType& type, const char* const (&names)[N])
			{
				Describe(name, type);
				if (IsCollecting())
					m_EnumOptions[name] = std::vector<std::string>(std::begin(names), std::end(names));
			}

			// The JSON type a Read overload accepts (see ComponentInfo::FieldTypes).
			template<typename T>
			static ComponentFieldType TypeOf()
			{
				if constexpr (std::is_same_v<T, float>)
					return { ComponentFieldKind::Number };
				else if constexpr (std::is_same_v<T, uint32_t>)
					return { ComponentFieldKind::Integer };
				else if constexpr (std::is_same_v<T, bool>)
					return { ComponentFieldKind::Boolean };
				else if constexpr (std::is_same_v<T, std::string>)
					return { ComponentFieldKind::String };
				else if constexpr (std::is_same_v<T, json>)
					return { ComponentFieldKind::Object };
				else if constexpr (std::is_same_v<T, glm::vec2> || std::is_same_v<T, glm::vec3> || std::is_same_v<T, glm::vec4>)
					return { ComponentFieldKind::Number, static_cast<uint8_t>(T::length()) };
				else if constexpr (std::is_same_v<T, glm::bvec2> || std::is_same_v<T, glm::bvec3> || std::is_same_v<T, glm::bvec4>)
					return { ComponentFieldKind::Boolean, static_cast<uint8_t>(T::length()) };
				else
					static_assert(sizeof(T) == 0, "add the field type to TypeOf");
			}

			template<typename E, size_t N>
			static E ReadEnum(const char* name, const json& element, const char* const (&names)[N])
			{
				if (!element.is_string())
					throw std::runtime_error(std::string("field '") + name + "' must be a string");
				const std::string text = element.get<std::string>();
				if (const std::optional<E> value = EnumFromName<E>(names, text))
					return *value;
				throw std::runtime_error(fmt::format("field '{}' has invalid value '{}' (valid: {})", name, text, fmt::join(names, ", ")));
			}

			const json* Find(const char* name)
			{
				if (!m_Data)
					return nullptr;
				auto it = m_Data->find(name);
				if (it == m_Data->end())
					return nullptr;
				m_Consumed.insert(name);
				return &*it;
			}

			static void Read(const char* name, const json& element, float& value)
			{
				if (!element.is_number())
					throw std::runtime_error(std::string("field '") + name + "' must be a number");
				if (!JsonToFloat(element, value))
					throw std::runtime_error(std::string("field '") + name + "' is out of range for a float");
			}

			static void Read(const char* name, const json& element, uint32_t& value)
			{
				if (!element.is_number_integer() && !element.is_number_unsigned())
					throw std::runtime_error(std::string("field '") + name + "' must be an integer");
				const int64_t number = element.get<int64_t>();
				if (number < 0 || number > static_cast<int64_t>(UINT32_MAX))
					throw std::runtime_error(std::string("field '") + name + "' is out of range");
				value = static_cast<uint32_t>(number);
			}

			// Entity references are written as unsigned numbers; Lua hands them back as signed 64-bit integers.
			static void Read(const char* name, const json& element, UUID& value)
			{
				if (element.is_number_unsigned())
					value = element.get<uint64_t>();
				else if (element.is_number_integer())
					value = static_cast<uint64_t>(element.get<int64_t>());
				else
					throw std::runtime_error(std::string("field '") + name + "' must be an entity ID (integer)");
			}

			static void Read(const char* name, const json& element, bool& value)
			{
				if (!element.is_boolean())
					throw std::runtime_error(std::string("field '") + name + "' must be true or false");
				value = element.get<bool>();
			}

			static void Read(const char* name, const json& element, std::string& value)
			{
				if (!element.is_string())
					throw std::runtime_error(std::string("field '") + name + "' must be a string");
				value = element.get<std::string>();
			}

			static void Read(const char* name, const json& element, json& value)
			{
				if (!element.is_object())
					throw std::runtime_error(std::string("field '") + name + "' must be an object");
				value = element;
			}

			template<glm::length_t L>
			static void Read(const char* name, const json& element, glm::vec<L, float, glm::defaultp>& value)
			{
				if (!element.is_array() || element.size() != static_cast<size_t>(L))
					throw std::runtime_error(std::string("field '") + name + "' must be an array of " + std::to_string(L) + " numbers");
				for (glm::length_t i = 0; i < L; i++)
				{
					if (!JsonToFloat(element[static_cast<size_t>(i)], value[i]))
						throw std::runtime_error(std::string("field '") + name + "' must contain only numbers in float range");
				}
			}

			template<glm::length_t L>
			static void Read(const char* name, const json& element, glm::vec<L, bool, glm::defaultp>& value)
			{
				if (!element.is_array() || element.size() != static_cast<size_t>(L) || !std::ranges::all_of(element, [](const json& v) { return v.is_boolean(); }))
					throw std::runtime_error(std::string("field '") + name + "' must be an array of " + std::to_string(L) + " booleans");
				for (glm::length_t i = 0; i < L; i++)
					value[i] = element[static_cast<size_t>(i)].get<bool>();
			}

		private:
			const json* m_Data = nullptr;
			std::string m_Prefix;
			std::unordered_set<std::string> m_Consumed;
			std::vector<std::string> m_FieldNames;
			std::map<std::string, std::vector<std::string>> m_EnumOptions;
			std::vector<std::string> m_EntityFields;
			std::vector<std::string> m_AssetFields;
			std::map<std::string, ComponentFieldType> m_FieldTypes;
		};

		// List element types. Their Fields and Write overloads are defined with the components' below; these
		// declarations let the list templates find them.
		void Fields(FieldReader& r, ParticleSizeKey& c);
		void Fields(FieldReader& r, ParticleColorKey& c);
		json Write(const ParticleSizeKey& c);
		json Write(const ParticleColorKey& c);
		void Fields(FieldReader& r, PropertyAnimationKey& c);
		void Fields(FieldReader& r, PropertyAnimationTrack& c);
		json Write(const PropertyAnimationKey& c);
		json Write(const PropertyAnimationTrack& c);
		void Fields(FieldReader& r, AnimationEvent& c);
		void Fields(FieldReader& r, AnimationBlendSample& c);
		void Fields(FieldReader& r, AnimationBlendSpace& c);
		void Fields(FieldReader& r, AnimationClipLibrary& c);
		void Fields(FieldReader& r, AnimationRetargetJoint& c);
		json Write(const AnimationClipLibrary& c);
		json Write(const AnimationRetargetJoint& c);
		json Write(const AnimationEvent& c);
		json Write(const AnimationBlendSample& c);
		json Write(const AnimationBlendSpace& c);
		void Fields(FieldReader& r, AnimationMaskJoint& c);
		void Fields(FieldReader& r, AnimationLayer& c);
		void Fields(FieldReader& r, IKChain& c);
		json Write(const AnimationMaskJoint& c);
		json Write(const AnimationLayer& c);
		json Write(const IKChain& c);
		void Fields(FieldReader& r, SpringChain& c);
		void Fields(FieldReader& r, SpringCollider& c);
		json Write(const SpringChain& c);
		json Write(const SpringCollider& c);

		// The shape of a list element, built once per element type from its field list.
		template<typename E>
		std::shared_ptr<const ComponentObjectSchema> SchemaOf()
		{
			static const std::shared_ptr<const ComponentObjectSchema> s_Schema = [] {
				FieldReader collector(nullptr);
				E element{};
				Fields(collector, element);
				// Entity references are remapped and resolved by name only at the top level of a component.
				BS_CORE_ASSERT(collector.GetEntityFields().empty(), "list elements cannot hold entity fields");
				auto schema = std::make_shared<ComponentObjectSchema>();
				schema->Name = E::TypeName;
				if constexpr (requires { E::UniqueField; })
					schema->UniqueField = E::UniqueField;
				if constexpr (requires { E::NormalizedTimeField; })
					schema->NormalizedTimeField = E::NormalizedTimeField;
				schema->Fields = collector.GetFieldNames();
				schema->FieldTypes = collector.GetFieldTypes();
				schema->EnumOptions = collector.GetEnumOptions();
				schema->AssetFields = collector.GetAssetFields();
				schema->Default = Write(element);
				BS_CORE_ASSERT(schema->UniqueField.empty() || (schema->FieldTypes.contains(schema->UniqueField) && schema->FieldTypes.at(schema->UniqueField).Kind == ComponentFieldKind::String), "UniqueField must name a string field");
				BS_CORE_ASSERT(schema->NormalizedTimeField.empty() || (schema->FieldTypes.contains(schema->NormalizedTimeField) && schema->FieldTypes.at(schema->NormalizedTimeField).Kind == ComponentFieldKind::Number && schema->FieldTypes.at(schema->NormalizedTimeField).Count == 0), "NormalizedTimeField must name a scalar number field");
				return schema;
			}();
			return s_Schema;
		}

		template<typename E>
		void FieldReader::ListField(const char* name, std::vector<E>& value, uint32_t maxCount)
		{
			const std::shared_ptr<const ComponentObjectSchema> schema = SchemaOf<E>();
			Describe(name, ComponentFieldType::List(maxCount, schema));
			// Element asset fields are declared as "List[].Field" (see ForEachAssetValue), one list level deep.
			if (IsCollecting())
				for (const std::string& field : schema->AssetFields)
				{
					BS_CORE_ASSERT(!IsListAssetField(field), "asset fields of lists inside list elements are not supported");
					m_AssetFields.push_back(fmt::format("{}[].{}", name, field));
				}
			const json* element = Find(name);
			if (!element)
				return;
			const std::string qualified = Qualify(name);
			const bool emptyObject = element->is_object() && element->empty();
			if (!element->is_array() && !emptyObject)
				throw std::runtime_error("field '" + qualified + "' must be an array of objects");
			if (element->size() > maxCount)
				throw std::runtime_error(fmt::format("field '{}' has {} elements (at most {})", qualified, element->size(), maxCount));
			std::vector<E> result;
			result.reserve(element->size());
			for (size_t i = 0; i < element->size(); i++)
			{
				const json& item = (*element)[i];
				const std::string itemName = fmt::format("{}[{}]", qualified, i);
				if (!item.is_object())
					throw std::runtime_error("field '" + itemName + "' must be an object");
				FieldReader reader(&item, itemName + ".");
				E& entry = result.emplace_back();
				Fields(reader, entry);
				reader.CheckUnknownKeys(schema->Fields);
			}
			value = std::move(result);
		}

		template<typename E>
		json ListToJson(const std::vector<E>& list)
		{
			json array = json::array();
			for (const E& element : list)
				array.push_back(Write(element));
			return array;
		}

		template<glm::length_t L>
		json ToJson(const glm::vec<L, float, glm::defaultp>& value)
		{
			json array = json::array();
			for (glm::length_t i = 0; i < L; i++)
				array.push_back(value[i]);
			return array;
		}

		template<glm::length_t L>
		json ToJson(const glm::vec<L, bool, glm::defaultp>& value)
		{
			json array = json::array();
			for (glm::length_t i = 0; i < L; i++)
				array.push_back(value[i]);
			return array;
		}

		template<typename E, size_t Count, size_t N>
		json EnumArrayToJson(const std::array<E, Count>& value, const char* const (&names)[N])
		{
			json array = json::array();
			for (E element : value)
				array.push_back(names[static_cast<size_t>(element)]);
			return array;
		}

		json RotationToJson(const glm::quat& rotation)
		{
			return ToJson(glm::degrees(Math::EulerFromQuat(rotation)));
		}

		constexpr const char* s_RigidBodyTypeNames[] = { "Static", "Dynamic", "Kinematic" };
		constexpr const char* s_ProjectionTypeNames[] = { "Perspective", "Orthographic" };
		constexpr const char* s_JointTypeNames[] = { "Fixed", "Point", "Hinge", "Slider", "Distance", "Cone", "SixDOF" };
		constexpr const char* s_JointMotorModeNames[] = { "Off", "Velocity", "Position" };
		constexpr const char* s_CombineModeNames[] = { "Default", "GeometricMean", "Average", "Min", "Multiply", "Max" };
		constexpr const char* s_ParticleShapeNames[] = { "Point", "Sphere", "Box", "Cone" };
		constexpr const char* s_ParticleSpaceNames[] = { "World", "Local" };
		constexpr const char* s_ParticleBlendNames[] = { "Alpha", "Additive" };
		constexpr const char* s_UIHorizontalAlignNames[] = { "Left", "Center", "Right" };
		constexpr const char* s_UIVerticalAlignNames[] = { "Top", "Middle", "Bottom" };
		constexpr const char* s_BlendSpaceTypeNames[] = { "1D", "2D" };
		constexpr const char* s_LayerBlendNames[] = { "Override", "Additive" };
		constexpr const char* s_IKChainTypeNames[] = { "TwoBone", "LookAt" };
		// Names are indexed by enum value; a new enum value needs a name here.
		static_assert(std::size(s_RigidBodyTypeNames) == static_cast<size_t>(RigidBodyType::Kinematic) + 1);
		static_assert(std::size(s_ProjectionTypeNames) == static_cast<size_t>(SceneCamera::ProjectionType::Orthographic) + 1);
		static_assert(std::size(s_JointTypeNames) == static_cast<size_t>(JointType::SixDOF) + 1);
		static_assert(std::size(s_JointMotorModeNames) == static_cast<size_t>(JointMotorMode::Position) + 1);
		static_assert(std::size(s_CombineModeNames) == static_cast<size_t>(PhysicsCombineMode::Max) + 1);
		static_assert(std::size(s_ParticleShapeNames) == ParticleEmitterShapeCount);
		static_assert(std::size(s_ParticleSpaceNames) == ParticleSimulationSpaceCount);
		static_assert(std::size(s_ParticleBlendNames) == ParticleBlendModeCount);
		static_assert(std::size(s_UIHorizontalAlignNames) == UIHorizontalAlignCount);
		static_assert(std::size(s_UIVerticalAlignNames) == UIVerticalAlignCount);
		static_assert(std::size(s_BlendSpaceTypeNames) == AnimationBlendSpaceTypeCount);
		static_assert(std::size(s_LayerBlendNames) == AnimationLayerBlendCount);
		static_assert(std::size(s_IKChainTypeNames) == IKChainTypeCount);

		// The name of an enum value. C++ code can store a value outside the enum (the registry never does);
		// writing it as the first name keeps saving from reading past the table.
		template<typename E, size_t N>
		const char* EnumName(const char* const (&names)[N], E value)
		{
			if (const char* name = EnumNameOrNull(names, value))
				return name;
			BS_CORE_ERROR("ComponentRegistry: enum value {} is out of range; writing '{}'", static_cast<size_t>(value), names[0]);
			return names[0];
		}

		// --- Per-component field definitions -----------------------------------------------------
		// Each Fields() overload lists every field once; it is used both to read and to enumerate names.

		void Fields(FieldReader& r, TagComponent& c)
		{
			r.Field("Tag", c.Tag);
		}

		void Fields(FieldReader& r, TransformComponent& c)
		{
			r.Field("Translation", c.Translation);
			r.RotationField("Rotation", c.Rotation);
			r.Field("Scale", c.Scale);
		}

		struct CameraFields
		{
			SceneCamera::ProjectionType Projection;
			float Fov, Near, Far, OrthoSize, OrthoNear, OrthoFar;
			bool Primary, FixedAspectRatio;
		};

		void Fields(FieldReader& r, CameraFields& c)
		{
			r.EnumField("Projection", c.Projection, s_ProjectionTypeNames);
			r.AngleField("FOV", c.Fov);
			r.Field("Near", c.Near);
			r.Field("Far", c.Far);
			r.Field("OrthoSize", c.OrthoSize);
			r.Field("OrthoNear", c.OrthoNear);
			r.Field("OrthoFar", c.OrthoFar);
			r.Field("Primary", c.Primary);
			r.Field("FixedAspectRatio", c.FixedAspectRatio);
		}

		void Fields(FieldReader& r, MeshComponent& c)
		{
			r.AssetField("Mesh", c.Mesh);
			r.Field("MeshIndex", c.MeshIndex);
			r.Field("CastShadows", c.CastShadows);
			r.Field("LODDistance", c.LODDistance);
		}

		void Fields(FieldReader& r, PropertyAnimationKey& c)
		{
			r.Field("Time", c.Time);
			r.Field("Value", c.Value);
		}
		void Fields(FieldReader& r, PropertyAnimationTrack& c)
		{
			r.Field("Component", c.Component);
			r.Field("Field", c.Field);
			r.Field("Stepped", c.Stepped);
			r.ListField("Keys", c.Keys, 256);
		}
		void Fields(FieldReader& r, PropertyAnimationComponent& c)
		{
			r.Field("Duration", c.Duration);
			r.Field("Speed", c.Speed);
			r.Field("Looping", c.Looping);
			r.Field("PlayOnStart", c.PlayOnStart);
			r.ListField("Tracks", c.Tracks, 64);
		}
		void Fields(FieldReader& r, AnimationEvent& c)
		{
			r.Field("Clip", c.Clip);
			r.Field("Time", c.Time);
			r.Field("Name", c.Name);
		}

		void Fields(FieldReader& r, AnimationBlendSample& c)
		{
			r.Field("Clip", c.Clip);
			r.Field("X", c.X);
			r.Field("Y", c.Y);
			r.Field("Speed", c.Speed);
		}

		void Fields(FieldReader& r, AnimationBlendSpace& c)
		{
			r.Field("Name", c.Name);
			r.EnumField("Type", c.Type, s_BlendSpaceTypeNames);
			r.ListField("Samples", c.Samples, AnimatorComponent::MaxBlendSamples);
		}

		void Fields(FieldReader& r, AnimationMaskJoint& c)
		{
			r.Field("Joint", c.Joint);
			r.Field("Weight", c.Weight);
		}

		void Fields(FieldReader& r, AnimationLayer& c)
		{
			r.Field("Name", c.Name);
			r.Field("Clip", c.Clip);
			r.EnumField("Blend", c.Blend, s_LayerBlendNames);
			r.Field("Weight", c.Weight);
			r.Field("Speed", c.Speed);
			r.Field("Looping", c.Looping);
			r.Field("ReferenceClip", c.ReferenceClip);
			r.Field("ReferenceTime", c.ReferenceTime);
			r.ListField("Mask", c.Mask, AnimationLayer::MaxMaskJoints);
		}

		void Fields(FieldReader& r, AnimationClipLibrary& c)
		{
			r.AssetField("Mesh", c.Mesh);
			r.ListField("Joints", c.Joints, AnimationClipLibrary::MaxJoints);
		}

		void Fields(FieldReader& r, AnimationRetargetJoint& c)
		{
			r.Field("Source", c.Source);
			r.Field("Target", c.Target);
		}

		void Fields(FieldReader& r, AnimatorComponent& c)
		{
			r.AssetField("Mesh", c.Mesh);
			r.Field("Clip", c.Clip);
			r.Field("Speed", c.Speed);
			r.Field("Looping", c.Looping);
			r.Field("PlayOnStart", c.PlayOnStart);
			r.Field("CastShadows", c.CastShadows);
			r.Field("MatchVelocity", c.MatchVelocity);
			r.AssetField("StateMachine", c.StateMachine);
			r.Field("RootMotion", c.RootMotion);
			r.Field("RootJoint", c.RootJoint);
			r.ListField("Events", c.Events, AnimatorComponent::MaxEvents);
			r.ListField("BlendSpaces", c.BlendSpaces, AnimatorComponent::MaxBlendSpaces);
			r.ListField("Layers", c.Layers, AnimatorComponent::MaxLayers);
			r.Field("LODDistance", c.LODDistance);
			r.Field("LODInterval", c.LODInterval);
			r.ListField("ClipLibraries", c.ClipLibraries, AnimatorComponent::MaxClipLibraries);
		}

		void Fields(FieldReader& r, BoneAttachmentComponent& c)
		{
			r.Field("Target", c.Target);
			r.Field("Joint", c.Joint);
			r.Field("Translation", c.Translation);
			r.RotationField("Rotation", c.Rotation);
			r.Field("Scale", c.Scale);
		}

		void Fields(FieldReader& r, IKChain& c)
		{
			r.Field("Name", c.Name);
			r.EnumField("Type", c.Type, s_IKChainTypeNames);
			r.Field("Joint", c.Joint);
			r.Field("ChainLength", c.ChainLength);
			r.Field("Axis", c.Axis);
			r.Field("MaxAngle", c.MaxAngle);
			r.Field("Pole", c.Pole);
			r.Field("Ground", c.Ground);
			r.Field("Weight", c.Weight);
		}

		void Fields(FieldReader& r, InverseKinematicsComponent& c)
		{
			r.ListField("Chains", c.Chains, InverseKinematicsComponent::MaxChains);
			r.Field("Pelvis", c.Pelvis);
			r.Field("RayHeight", c.RayHeight);
			r.Field("RayDepth", c.RayDepth);
			r.Field("Smoothing", c.Smoothing);
		}

		void Fields(FieldReader& r, SpringChain& c)
		{
			r.Field("Name", c.Name);
			r.Field("Joint", c.Joint);
			r.Field("ChainLength", c.ChainLength);
			r.Field("Stiffness", c.Stiffness);
			r.Field("Damping", c.Damping);
			r.Field("GravityFactor", c.GravityFactor);
			r.Field("Radius", c.Radius);
			r.Field("Weight", c.Weight);
		}

		void Fields(FieldReader& r, SpringCollider& c)
		{
			r.Field("Joint", c.Joint);
			r.Field("Offset", c.Offset);
			r.Field("Radius", c.Radius);
			r.Field("HalfHeight", c.HalfHeight);
		}

		void Fields(FieldReader& r, SpringBoneComponent& c)
		{
			r.ListField("Chains", c.Chains, SpringBoneComponent::MaxChains);
			r.ListField("Colliders", c.Colliders, SpringBoneComponent::MaxColliders);
			r.Field("Wind", c.Wind);
		}

		void Fields(FieldReader& r, MaterialComponent& c)
		{
			r.AssetField("Material", c.Material);
			r.Field("AlbedoColor", c.AlbedoColor);
			r.Field("Metallic", c.Metallic);
			r.Field("Roughness", c.Roughness);
			r.Field("EmissiveColor", c.EmissiveColor);
			r.Field("EmissiveIntensity", c.EmissiveIntensity);
			r.AssetField("AlbedoMap", c.AlbedoMap);
			r.AssetField("NormalMap", c.NormalMap);
			r.AssetField("MetallicRoughnessMap", c.MetallicRoughnessMap);
			r.AssetField("OcclusionMap", c.OcclusionMap);
			r.AssetField("EmissiveMap", c.EmissiveMap);
			r.Field("Clearcoat", c.Clearcoat);
			r.Field("ClearcoatRoughness", c.ClearcoatRoughness);
			r.Field("Transmission", c.Transmission);
			r.AssetField("ClearcoatMap", c.ClearcoatMap);
			r.AssetField("ClearcoatRoughnessMap", c.ClearcoatRoughnessMap);
			r.AssetField("ClearcoatNormalMap", c.ClearcoatNormalMap);
			r.AssetField("TransmissionMap", c.TransmissionMap);
			r.Field("UVOffset", c.UVOffset);
			r.Field("UVScale", c.UVScale);
			r.Field("UVRotation", c.UVRotation);
		}

		void Fields(FieldReader& r, DirectionalLightComponent& c)
		{
			r.Field("Color", c.Color);
			r.Field("Intensity", c.Intensity);
			r.Field("CastShadows", c.CastShadows);
			r.Field("ShadowSoftness", c.ShadowSoftness);
		}

		void Fields(FieldReader& r, PointLightComponent& c)
		{
			r.Field("Color", c.Color);
			r.Field("Intensity", c.Intensity);
			r.Field("Radius", c.Radius);
			r.Field("CastShadows", c.CastShadows);
			r.Field("ShadowSoftness", c.ShadowSoftness);
		}

		void Fields(FieldReader& r, SpotLightComponent& c)
		{
			r.Field("Color", c.Color);
			r.Field("Intensity", c.Intensity);
			r.Field("Range", c.Range);
			r.Field("InnerConeAngle", c.InnerConeAngle);
			r.Field("OuterConeAngle", c.OuterConeAngle);
			r.Field("CastShadows", c.CastShadows);
			r.Field("ShadowSoftness", c.ShadowSoftness);
		}

		void Fields(FieldReader& r, PostProcessComponent& c)
		{
			r.Field("Enabled", c.Enabled);
			r.Field("Priority", c.Priority);
			r.EnumField("Tonemapper", c.Tonemap, TonemapperNames);
			r.Field("Exposure", c.Exposure);
			r.Field("AutoExposure", c.AutoExposure);
			r.Field("MinExposure", c.MinExposure);
			r.Field("MaxExposure", c.MaxExposure);
			r.Field("ExposureCompensation", c.ExposureCompensation);
			r.Field("AdaptationSpeed", c.AdaptationSpeed);
			r.Field("SSAOEnabled", c.SSAOEnabled);
			r.Field("SSAORadius", c.SSAORadius);
			r.Field("SSAOIntensity", c.SSAOIntensity);
			r.Field("SSAOBias", c.SSAOBias);
			r.Field("FXAAEnabled", c.FXAAEnabled);
			r.Field("TAAEnabled", c.TAAEnabled);
			r.Field("BloomEnabled", c.BloomEnabled);
			r.Field("BloomThreshold", c.BloomThreshold);
			r.Field("BloomIntensity", c.BloomIntensity);
			r.Field("BloomScatter", c.BloomScatter);
		}

		void Fields(FieldReader& r, SkyLightComponent& c)
		{
			r.AssetField("EnvironmentMap", c.EnvironmentMap);
			r.Field("Intensity", c.Intensity);
			r.Field("Rotation", c.Rotation);
			r.Field("ShowBackground", c.ShowBackground);
		}

		void Fields(FieldReader& r, RigidBodyComponent& c)
		{
			r.EnumField("Type", c.Type, s_RigidBodyTypeNames);
			r.Field("Mass", c.Mass);
			r.Field("LinearDamping", c.LinearDamping);
			r.Field("AngularDamping", c.AngularDamping);
			r.Field("GravityFactor", c.GravityFactor);
			r.Field("Friction", c.Friction);
			r.Field("Restitution", c.Restitution);
			r.EnumField("FrictionCombine", c.FrictionCombine, s_CombineModeNames);
			r.EnumField("RestitutionCombine", c.RestitutionCombine, s_CombineModeNames);
			r.Field("IsTrigger", c.IsTrigger);
			r.Field("FixedRotation", c.FixedRotation);
			// Before named layers, files stored Layer as an index and a per-body CollisionMask (16 bits used).
			// A body whose mask excluded no layer collides exactly like one on Default, so it converts
			// losslessly; any other mask cannot be expressed without project layers and is an error.
			const bool legacyMask = r.LegacyField("CollisionMask", [](const json&) { return true; }, [](const json& mask) {
				if (!mask.is_number_unsigned() && !mask.is_number_integer())
					throw std::runtime_error("field 'CollisionMask' must be an integer");
				if ((mask.get<int64_t>() & 0xFFFF) != 0xFFFF)
					throw std::runtime_error("field 'CollisionMask' was replaced by the project's collision matrix: name the layers with project.set physicsLayers, set Layer to a name and remove CollisionMask"); });
			const bool legacyLayer = r.LegacyField("Layer", [](const json& layer) { return layer.is_number(); }, [&c](const json& layer) {
				if (!layer.is_number_unsigned() && !layer.is_number_integer())
					throw std::runtime_error("field 'Layer' must be a layer name");
				c.Layer = PhysicsLayers::DefaultLayerName; });
			if (!legacyLayer)
				r.Field("Layer", c.Layer);
			r.Field("Continuous", c.Continuous);
			r.Field("AllowSleeping", c.AllowSleeping);
			if (legacyMask || legacyLayer)
			{
				static bool s_Warned = false;
				if (!std::exchange(s_Warned, true))
					BS_CORE_WARN("Scene: converted RigidBody Layer/CollisionMask from before named physics layers to Layer \"Default\"; save the scene or prefab to update the file");
			}
		}

		// The material override every collider has.
		template<typename Collider>
		void MaterialFields(FieldReader& r, Collider& c)
		{
			r.Field("OverrideMaterial", c.OverrideMaterial);
			r.Field("Friction", c.Friction);
			r.Field("Restitution", c.Restitution);
		}

		void Fields(FieldReader& r, BoxColliderComponent& c)
		{
			r.Field("HalfExtents", c.HalfExtents);
			r.Field("Offset", c.Offset);
			MaterialFields(r, c);
		}

		void Fields(FieldReader& r, SphereColliderComponent& c)
		{
			r.Field("Radius", c.Radius);
			r.Field("Offset", c.Offset);
			MaterialFields(r, c);
		}

		void Fields(FieldReader& r, CapsuleColliderComponent& c)
		{
			r.Field("Radius", c.Radius);
			r.Field("HalfHeight", c.HalfHeight);
			r.Field("Offset", c.Offset);
			MaterialFields(r, c);
		}

		void Fields(FieldReader& r, MeshColliderComponent& c)
		{
			r.AssetField("Mesh", c.Mesh);
			r.Field("MeshIndex", c.MeshIndex);
			r.Field("Convex", c.Convex);
			MaterialFields(r, c);
		}

		void Fields(FieldReader& r, JointComponent& c)
		{
			r.EnumField("Type", c.Type, s_JointTypeNames);
			r.Field("BodyEntity", c.BodyEntity);
			r.Field("ConnectedEntity", c.ConnectedEntity);
			r.Field("Anchor", c.Anchor);
			r.Field("ConnectedAnchor", c.ConnectedAnchor);
			r.Field("Axis", c.Axis);
			r.Field("SecondaryAxis", c.SecondaryAxis);
			r.Field("UseLimits", c.UseLimits);
			r.Field("LimitMin", c.LimitMin);
			r.Field("LimitMax", c.LimitMax);
			r.Field("LimitSpringFrequency", c.LimitSpringFrequency);
			r.Field("LimitSpringDamping", c.LimitSpringDamping);
			r.Field("LinearLimitMin", c.LinearLimitMin);
			r.Field("LinearLimitMax", c.LinearLimitMax);
			r.Field("AngularLimitMin", c.AngularLimitMin);
			r.Field("AngularLimitMax", c.AngularLimitMax);
			r.Field("FreeLinearAxes", c.FreeLinearAxes);
			r.EnumField("MotorMode", c.MotorMode, s_JointMotorModeNames);
			r.Field("MotorTarget", c.MotorTarget);
			r.EnumArrayField("LinearMotorMode", c.LinearMotorMode, s_JointMotorModeNames);
			r.EnumArrayField("AngularMotorMode", c.AngularMotorMode, s_JointMotorModeNames);
			r.Field("LinearMotorTarget", c.LinearMotorTarget);
			r.Field("AngularMotorTarget", c.AngularMotorTarget);
			r.Field("MotorMaxForce", c.MotorMaxForce);
			r.Field("MotorMaxTorque", c.MotorMaxTorque);
			r.Field("MotorSpringFrequency", c.MotorSpringFrequency);
			r.Field("MotorSpringDamping", c.MotorSpringDamping);
			r.Field("BreakForce", c.BreakForce);
			r.Field("BreakTorque", c.BreakTorque);
			r.Field("EnableCollision", c.EnableCollision);
		}

		void Fields(FieldReader& r, CharacterControllerComponent& c)
		{
			r.Field("SlopeLimit", c.SlopeLimit);
			r.Field("StepHeight", c.StepHeight);
			r.Field("MaxStrength", c.MaxStrength);
			r.Field("Mass", c.Mass);
			r.Field("GravityFactor", c.GravityFactor);
			r.Field("JumpGrace", c.JumpGrace);
			r.Field("Layer", c.Layer);
		}

		void Fields(FieldReader& r, ScriptComponent& c)
		{
			r.AssetField("Script", c.Script);
			r.Field("Properties", c.Properties);
		}

		void Fields(FieldReader& r, AudioSourceComponent& c)
		{
			r.AssetField("Clip", c.Clip);
			r.Field("Volume", c.Volume);
			r.Field("Pitch", c.Pitch);
			r.Field("Loop", c.Loop);
			r.Field("PlayOnStart", c.PlayOnStart);
			r.Field("Spatial", c.Spatial);
			r.Field("MinDistance", c.MinDistance);
			r.Field("MaxDistance", c.MaxDistance);
			r.EnumField("Bus", c.Bus, AudioBusNames);
			r.Field("Stream", c.Stream);
			r.Field("Doppler", c.Doppler);
			r.Field("LowPass", c.LowPass);
			r.Field("ReverbSend", c.ReverbSend);
			r.Field("HighPass", c.HighPass);
			r.Field("Echo", c.Echo);
			r.Field("Distortion", c.Distortion);
		}

		void Fields(FieldReader& r, AudioReverbZoneComponent& c)
		{
			r.Field("Enabled", c.Enabled);
			r.Field("Priority", c.Priority);
			r.Field("Radius", c.Radius);
			r.Field("BlendDistance", c.BlendDistance);
			r.Field("RoomSize", c.RoomSize);
			r.Field("Damping", c.Damping);
			r.Field("Width", c.Width);
		}

		void Fields(FieldReader& r, AudioListenerComponent& c)
		{
			r.Field("Active", c.Active);
		}

		void Fields(FieldReader& r, ParticleSizeKey& c)
		{
			r.Field("Time", c.Time);
			r.Field("Value", c.Value);
		}
		void Fields(FieldReader& r, ParticleColorKey& c)
		{
			r.Field("Time", c.Time);
			r.Field("Value", c.Value);
		}
		json Write(const ParticleSizeKey& c)
		{
			return { { "Time", c.Time }, { "Value", c.Value } };
		}
		json Write(const ParticleColorKey& c)
		{
			return { { "Time", c.Time }, { "Value", ToJson(c.Value) } };
		}

		void Fields(FieldReader& r, ParticleEmitterComponent& c)
		{
			r.Field("PlayOnStart", c.PlayOnStart);
			r.Field("Looping", c.Looping);
			r.Field("Duration", c.Duration);
			r.Field("MaxParticles", c.MaxParticles);
			r.Field("Rate", c.Rate);
			r.Field("BurstCount", c.BurstCount);
			r.EnumField("Shape", c.Shape, s_ParticleShapeNames);
			r.Field("ShapeRadius", c.ShapeRadius);
			r.Field("BoxExtents", c.BoxExtents);
			r.Field("ConeAngle", c.ConeAngle);
			r.Field("LifetimeMin", c.LifetimeMin);
			r.Field("LifetimeMax", c.LifetimeMax);
			r.Field("SpeedMin", c.SpeedMin);
			r.Field("SpeedMax", c.SpeedMax);
			r.Field("Velocity", c.Velocity);
			r.Field("GravityScale", c.GravityScale);
			r.Field("Drag", c.Drag);
			r.EnumField("SimulationSpace", c.SimulationSpace, s_ParticleSpaceNames);
			r.Field("StartColor", c.StartColor);
			r.Field("EndColor", c.EndColor);
			r.Field("StartSize", c.StartSize);
			r.Field("EndSize", c.EndSize);
			r.Field("StartSizeMin", c.StartSizeMin);
			r.Field("StartSizeMax", c.StartSizeMax);
			r.Field("StartColorMin", c.StartColorMin);
			r.Field("StartColorMax", c.StartColorMax);
			r.AngleField("RotationMin", c.RotationMin);
			r.AngleField("RotationMax", c.RotationMax);
			r.AngleField("AngularVelocityMin", c.AngularVelocityMin);
			r.AngleField("AngularVelocityMax", c.AngularVelocityMax);
			r.Field("SoftDistance", c.SoftDistance);
			r.Field("Lit", c.Lit);
			r.Field("Preview", c.Preview);
			r.ListField("SizeCurve", c.SizeCurve, ParticleEmitterComponent::MaxCurveKeys);
			r.ListField("ColorCurve", c.ColorCurve, ParticleEmitterComponent::MaxCurveKeys);
			r.EnumField("Blend", c.Blend, s_ParticleBlendNames);
			r.AssetField("Texture", c.Texture);
		}

		void Fields(FieldReader& r, UIRectComponent& c)
		{
			r.Field("Visible", c.Visible);
			r.Field("ClipChildren", c.ClipChildren);
			r.AngleField("Rotation", c.Rotation);
			r.Field("Scale", c.Scale);
			r.Field("AnchorMin", c.AnchorMin);
			r.Field("AnchorMax", c.AnchorMax);
			r.Field("Pivot", c.Pivot);
			r.Field("Position", c.Position);
			r.Field("Size", c.Size);
		}

		void Fields(FieldReader& r, UIImageComponent& c)
		{
			r.AssetField("Texture", c.Texture);
			r.Field("Color", c.Color);
			r.Field("Slice", c.Slice);
			r.Field("SliceScale", c.SliceScale);
			r.Field("UVMin", c.UVMin);
			r.Field("UVMax", c.UVMax);
			r.Field("SpriteColumns", c.SpriteColumns);
			r.Field("SpriteRows", c.SpriteRows);
			r.Field("SpriteFrame", c.SpriteFrame);
		}

		void Fields(FieldReader& r, UITextComponent& c)
		{
			r.Field("Text", c.Text);
			r.AssetField("Font", c.Font);
			r.Field("FontSize", c.FontSize);
			r.Field("Color", c.Color);
			r.EnumField("HorizontalAlign", c.HorizontalAlign, s_UIHorizontalAlignNames);
			r.EnumField("VerticalAlign", c.VerticalAlign, s_UIVerticalAlignNames);
			r.Field("Wrap", c.Wrap);
			r.Field("Shadow", c.Shadow);
		}

		// The tints, target and callback every widget has, after its own fields.
		template<typename T>
		void WidgetFields(FieldReader& r, T& c, const char* callbackField, std::string& callback)
		{
			r.Field("NormalTint", c.NormalTint);
			r.Field("HighlightTint", c.HighlightTint);
			r.Field("PressedTint", c.PressedTint);
			r.Field("DisabledTint", c.DisabledTint);
			r.Field("Target", c.Target);
			r.Field(callbackField, callback);
		}

		void Fields(FieldReader& r, UIButtonComponent& c)
		{
			r.Field("Interactable", c.Interactable);
			WidgetFields(r, c, "OnClick", c.OnClick);
		}

		void Fields(FieldReader& r, UISliderComponent& c)
		{
			r.Field("Interactable", c.Interactable);
			r.Field("Value", c.Value);
			r.Field("Min", c.Min);
			r.Field("Max", c.Max);
			r.Field("Step", c.Step);
			r.Field("TrackColor", c.TrackColor);
			r.Field("FillColor", c.FillColor);
			r.Field("HandleColor", c.HandleColor);
			r.Field("HandleWidth", c.HandleWidth);
			r.Field("Vertical", c.Vertical);
			WidgetFields(r, c, "OnValueChanged", c.OnValueChanged);
		}

		void Fields(FieldReader& r, UICheckboxComponent& c)
		{
			r.Field("Interactable", c.Interactable);
			r.Field("Checked", c.Checked);
			r.Field("BoxColor", c.BoxColor);
			r.Field("CheckColor", c.CheckColor);
			WidgetFields(r, c, "OnValueChanged", c.OnValueChanged);
		}

		void Fields(FieldReader& r, UITextInputComponent& c)
		{
			r.Field("Interactable", c.Interactable);
			r.Field("ReadOnly", c.ReadOnly);
			r.Field("Text", c.Text);
			r.Field("Placeholder", c.Placeholder);
			r.Field("MaxLength", c.MaxLength);
			WidgetFields(r, c, "OnValueChanged", c.OnValueChanged);
		}

		void Fields(FieldReader& r, UIDropdownComponent& c)
		{
			r.Field("Interactable", c.Interactable);
			r.Field("Options", c.Options);
			r.Field("Selected", c.Selected);
			WidgetFields(r, c, "OnValueChanged", c.OnValueChanged);
		}

		void Fields(FieldReader& r, UIScrollViewComponent& c)
		{
			r.Field("Interactable", c.Interactable);
			r.Field("ContentSize", c.ContentSize);
			r.Field("Offset", c.Offset);
			r.Field("ScrollSpeed", c.ScrollSpeed);
		}

		void Fields(FieldReader& r, UIPanelComponent& c)
		{
			r.Field("BlockInput", c.BlockInput);
		}

		void Fields(FieldReader& r, UINavigationComponent& c)
		{
			r.Field("Up", c.Up);
			r.Field("Down", c.Down);
			r.Field("Left", c.Left);
			r.Field("Right", c.Right);
		}

		void Fields(FieldReader& r, UIFocusRingComponent& c)
		{
			r.Field("Color", c.Color);
			r.Field("Thickness", c.Thickness);
		}

		void Fields(FieldReader& r, PrefabComponent& c)
		{
			r.AssetField("Prefab", c.Prefab);
		}

		// --- Writers -----------------------------------------------------------------------------

		json Write(const TagComponent& c)
		{
			return { { "Tag", c.Tag } };
		}

		json Write(const TransformComponent& c)
		{
			return { { "Translation", ToJson(c.Translation) }, { "Rotation", RotationToJson(c.Rotation) }, { "Scale", ToJson(c.Scale) } };
		}

		json Write(const CameraFields& c)
		{
			return {
				{ "Projection", s_ProjectionTypeNames[static_cast<int>(c.Projection)] },
				{ "FOV", glm::degrees(c.Fov) },
				{ "Near", c.Near },
				{ "Far", c.Far },
				{ "OrthoSize", c.OrthoSize },
				{ "OrthoNear", c.OrthoNear },
				{ "OrthoFar", c.OrthoFar },
				{ "Primary", c.Primary },
				{ "FixedAspectRatio", c.FixedAspectRatio },
			};
		}

		json Write(const MeshComponent& c)
		{
			return { { "Mesh", c.Mesh }, { "MeshIndex", c.MeshIndex }, { "CastShadows", c.CastShadows }, { "LODDistance", c.LODDistance } };
		}

		json Write(const PropertyAnimationKey& c)
		{
			return { { "Time", c.Time }, { "Value", { c.Value.x, c.Value.y, c.Value.z, c.Value.w } } };
		}
		json Write(const PropertyAnimationTrack& c)
		{
			return { { "Component", c.Component }, { "Field", c.Field }, { "Stepped", c.Stepped }, { "Keys", ListToJson(c.Keys) } };
		}
		json Write(const PropertyAnimationComponent& c)
		{
			return { { "Duration", c.Duration }, { "Speed", c.Speed }, { "Looping", c.Looping }, { "PlayOnStart", c.PlayOnStart }, { "Tracks", ListToJson(c.Tracks) } };
		}
		json Write(const AnimationEvent& c)
		{
			return { { "Clip", c.Clip }, { "Time", c.Time }, { "Name", c.Name } };
		}

		json Write(const AnimationBlendSample& c)
		{
			return { { "Clip", c.Clip }, { "X", c.X }, { "Y", c.Y }, { "Speed", c.Speed } };
		}

		json Write(const AnimationBlendSpace& c)
		{
			return { { "Name", c.Name }, { "Type", EnumName(s_BlendSpaceTypeNames, c.Type) }, { "Samples", ListToJson(c.Samples) } };
		}

		json Write(const AnimatorComponent& c)
		{
			return {
				{ "Mesh", c.Mesh },
				{ "Clip", c.Clip },
				{ "Speed", c.Speed },
				{ "Looping", c.Looping },
				{ "PlayOnStart", c.PlayOnStart },
				{ "CastShadows", c.CastShadows },
				{ "MatchVelocity", c.MatchVelocity },
				{ "StateMachine", c.StateMachine },
				{ "RootMotion", c.RootMotion },
				{ "RootJoint", c.RootJoint },
				{ "Events", ListToJson(c.Events) },
				{ "BlendSpaces", ListToJson(c.BlendSpaces) },
				{ "Layers", ListToJson(c.Layers) },
				{ "LODDistance", c.LODDistance },
				{ "LODInterval", c.LODInterval },
				{ "ClipLibraries", ListToJson(c.ClipLibraries) },
			};
		}

		json Write(const AnimationClipLibrary& c)
		{
			return { { "Mesh", c.Mesh }, { "Joints", ListToJson(c.Joints) } };
		}

		json Write(const AnimationRetargetJoint& c)
		{
			return { { "Source", c.Source }, { "Target", c.Target } };
		}

		json Write(const AnimationMaskJoint& c)
		{
			return { { "Joint", c.Joint }, { "Weight", c.Weight } };
		}

		json Write(const AnimationLayer& c)
		{
			return {
				{ "Name", c.Name },
				{ "Clip", c.Clip },
				{ "Blend", EnumName(s_LayerBlendNames, c.Blend) },
				{ "Weight", c.Weight },
				{ "Speed", c.Speed },
				{ "Looping", c.Looping },
				{ "ReferenceClip", c.ReferenceClip },
				{ "ReferenceTime", c.ReferenceTime },
				{ "Mask", ListToJson(c.Mask) },
			};
		}

		json Write(const IKChain& c)
		{
			return {
				{ "Name", c.Name },
				{ "Type", EnumName(s_IKChainTypeNames, c.Type) },
				{ "Joint", c.Joint },
				{ "ChainLength", c.ChainLength },
				{ "Axis", ToJson(c.Axis) },
				{ "MaxAngle", c.MaxAngle },
				{ "Pole", ToJson(c.Pole) },
				{ "Ground", c.Ground },
				{ "Weight", c.Weight },
			};
		}

		json Write(const InverseKinematicsComponent& c)
		{
			return { { "Chains", ListToJson(c.Chains) }, { "Pelvis", c.Pelvis }, { "RayHeight", c.RayHeight }, { "RayDepth", c.RayDepth }, { "Smoothing", c.Smoothing } };
		}

		json Write(const SpringChain& c)
		{
			return {
				{ "Name", c.Name },
				{ "Joint", c.Joint },
				{ "ChainLength", c.ChainLength },
				{ "Stiffness", c.Stiffness },
				{ "Damping", c.Damping },
				{ "GravityFactor", c.GravityFactor },
				{ "Radius", c.Radius },
				{ "Weight", c.Weight },
			};
		}

		json Write(const SpringCollider& c)
		{
			return { { "Joint", c.Joint }, { "Offset", ToJson(c.Offset) }, { "Radius", c.Radius }, { "HalfHeight", c.HalfHeight } };
		}

		json Write(const SpringBoneComponent& c)
		{
			return { { "Chains", ListToJson(c.Chains) }, { "Colliders", ListToJson(c.Colliders) }, { "Wind", ToJson(c.Wind) } };
		}

		json Write(const BoneAttachmentComponent& c)
		{
			return { { "Target", static_cast<uint64_t>(c.Target) }, { "Joint", c.Joint }, { "Translation", ToJson(c.Translation) }, { "Rotation", RotationToJson(c.Rotation) }, { "Scale", ToJson(c.Scale) } };
		}

		json Write(const MaterialComponent& c)
		{
			return {
				{ "Material", c.Material },
				{ "AlbedoColor", ToJson(c.AlbedoColor) },
				{ "Metallic", c.Metallic },
				{ "Roughness", c.Roughness },
				{ "EmissiveColor", ToJson(c.EmissiveColor) },
				{ "EmissiveIntensity", c.EmissiveIntensity },
				{ "AlbedoMap", c.AlbedoMap },
				{ "NormalMap", c.NormalMap },
				{ "MetallicRoughnessMap", c.MetallicRoughnessMap },
				{ "OcclusionMap", c.OcclusionMap },
				{ "EmissiveMap", c.EmissiveMap },
				{ "Clearcoat", c.Clearcoat },
				{ "ClearcoatRoughness", c.ClearcoatRoughness },
				{ "Transmission", c.Transmission },
				{ "ClearcoatMap", c.ClearcoatMap },
				{ "ClearcoatRoughnessMap", c.ClearcoatRoughnessMap },
				{ "ClearcoatNormalMap", c.ClearcoatNormalMap },
				{ "TransmissionMap", c.TransmissionMap },
				{ "UVOffset", ToJson(c.UVOffset) },
				{ "UVScale", ToJson(c.UVScale) },
				{ "UVRotation", c.UVRotation },
			};
		}

		json Write(const DirectionalLightComponent& c)
		{
			return { { "Color", ToJson(c.Color) }, { "Intensity", c.Intensity }, { "CastShadows", c.CastShadows }, { "ShadowSoftness", c.ShadowSoftness } };
		}

		json Write(const PointLightComponent& c)
		{
			return { { "Color", ToJson(c.Color) }, { "Intensity", c.Intensity }, { "Radius", c.Radius }, { "CastShadows", c.CastShadows }, { "ShadowSoftness", c.ShadowSoftness } };
		}

		json Write(const SpotLightComponent& c)
		{
			return { { "Color", ToJson(c.Color) }, { "Intensity", c.Intensity }, { "Range", c.Range }, { "InnerConeAngle", c.InnerConeAngle }, { "OuterConeAngle", c.OuterConeAngle }, { "CastShadows", c.CastShadows }, { "ShadowSoftness", c.ShadowSoftness } };
		}

		json Write(const PostProcessComponent& c)
		{
			return {
				{ "Enabled", c.Enabled },
				{ "Priority", c.Priority },
				{ "Tonemapper", EnumName(TonemapperNames, c.Tonemap) },
				{ "Exposure", c.Exposure },
				{ "AutoExposure", c.AutoExposure },
				{ "MinExposure", c.MinExposure },
				{ "MaxExposure", c.MaxExposure },
				{ "ExposureCompensation", c.ExposureCompensation },
				{ "AdaptationSpeed", c.AdaptationSpeed },
				{ "SSAOEnabled", c.SSAOEnabled },
				{ "SSAORadius", c.SSAORadius },
				{ "SSAOIntensity", c.SSAOIntensity },
				{ "SSAOBias", c.SSAOBias },
				{ "FXAAEnabled", c.FXAAEnabled },
				{ "TAAEnabled", c.TAAEnabled },
				{ "BloomEnabled", c.BloomEnabled },
				{ "BloomThreshold", c.BloomThreshold },
				{ "BloomIntensity", c.BloomIntensity },
				{ "BloomScatter", c.BloomScatter },
			};
		}

		json Write(const SkyLightComponent& c)
		{
			return { { "EnvironmentMap", c.EnvironmentMap }, { "Intensity", c.Intensity }, { "Rotation", c.Rotation }, { "ShowBackground", c.ShowBackground } };
		}

		json Write(const RigidBodyComponent& c)
		{
			return {
				{ "Type", s_RigidBodyTypeNames[static_cast<int>(c.Type)] },
				{ "Mass", c.Mass },
				{ "LinearDamping", c.LinearDamping },
				{ "AngularDamping", c.AngularDamping },
				{ "GravityFactor", c.GravityFactor },
				{ "Friction", c.Friction },
				{ "Restitution", c.Restitution },
				{ "FrictionCombine", s_CombineModeNames[static_cast<int>(c.FrictionCombine)] },
				{ "RestitutionCombine", s_CombineModeNames[static_cast<int>(c.RestitutionCombine)] },
				{ "IsTrigger", c.IsTrigger },
				{ "FixedRotation", c.FixedRotation },
				{ "Layer", c.Layer },
				{ "Continuous", c.Continuous },
				{ "AllowSleeping", c.AllowSleeping },
			};
		}

		template<typename Collider>
		json WithMaterial(json data, const Collider& c)
		{
			data["OverrideMaterial"] = c.OverrideMaterial;
			data["Friction"] = c.Friction;
			data["Restitution"] = c.Restitution;
			return data;
		}

		json Write(const BoxColliderComponent& c)
		{
			return WithMaterial({ { "HalfExtents", ToJson(c.HalfExtents) }, { "Offset", ToJson(c.Offset) } }, c);
		}
		json Write(const SphereColliderComponent& c)
		{
			return WithMaterial({ { "Radius", c.Radius }, { "Offset", ToJson(c.Offset) } }, c);
		}
		json Write(const CapsuleColliderComponent& c)
		{
			return WithMaterial({ { "Radius", c.Radius }, { "HalfHeight", c.HalfHeight }, { "Offset", ToJson(c.Offset) } }, c);
		}
		json Write(const MeshColliderComponent& c)
		{
			return WithMaterial({ { "Mesh", c.Mesh }, { "MeshIndex", c.MeshIndex }, { "Convex", c.Convex } }, c);
		}
		json Write(const JointComponent& c)
		{
			return {
				{ "Type", s_JointTypeNames[static_cast<int>(c.Type)] },
				{ "BodyEntity", static_cast<uint64_t>(c.BodyEntity) },
				{ "ConnectedEntity", static_cast<uint64_t>(c.ConnectedEntity) },
				{ "Anchor", ToJson(c.Anchor) },
				{ "ConnectedAnchor", ToJson(c.ConnectedAnchor) },
				{ "Axis", ToJson(c.Axis) },
				{ "SecondaryAxis", ToJson(c.SecondaryAxis) },
				{ "UseLimits", c.UseLimits },
				{ "LimitMin", c.LimitMin },
				{ "LimitMax", c.LimitMax },
				{ "LimitSpringFrequency", c.LimitSpringFrequency },
				{ "LimitSpringDamping", c.LimitSpringDamping },
				{ "LinearLimitMin", ToJson(c.LinearLimitMin) },
				{ "LinearLimitMax", ToJson(c.LinearLimitMax) },
				{ "AngularLimitMin", ToJson(c.AngularLimitMin) },
				{ "AngularLimitMax", ToJson(c.AngularLimitMax) },
				{ "FreeLinearAxes", ToJson(c.FreeLinearAxes) },
				{ "MotorMode", s_JointMotorModeNames[static_cast<int>(c.MotorMode)] },
				{ "MotorTarget", c.MotorTarget },
				{ "LinearMotorMode", EnumArrayToJson(c.LinearMotorMode, s_JointMotorModeNames) },
				{ "AngularMotorMode", EnumArrayToJson(c.AngularMotorMode, s_JointMotorModeNames) },
				{ "LinearMotorTarget", ToJson(c.LinearMotorTarget) },
				{ "AngularMotorTarget", ToJson(c.AngularMotorTarget) },
				{ "MotorMaxForce", c.MotorMaxForce },
				{ "MotorMaxTorque", c.MotorMaxTorque },
				{ "MotorSpringFrequency", c.MotorSpringFrequency },
				{ "MotorSpringDamping", c.MotorSpringDamping },
				{ "BreakForce", c.BreakForce },
				{ "BreakTorque", c.BreakTorque },
				{ "EnableCollision", c.EnableCollision },
			};
		}

		json Write(const CharacterControllerComponent& c)
		{
			return {
				{ "SlopeLimit", c.SlopeLimit },
				{ "StepHeight", c.StepHeight },
				{ "MaxStrength", c.MaxStrength },
				{ "Mass", c.Mass },
				{ "GravityFactor", c.GravityFactor },
				{ "JumpGrace", c.JumpGrace },
				{ "Layer", c.Layer },
			};
		}

		json Write(const ScriptComponent& c)
		{
			return { { "Script", c.Script }, { "Properties", c.Properties } };
		}

		json Write(const AudioSourceComponent& c)
		{
			return {
				{ "Clip", c.Clip },
				{ "Volume", c.Volume },
				{ "Pitch", c.Pitch },
				{ "Loop", c.Loop },
				{ "PlayOnStart", c.PlayOnStart },
				{ "Spatial", c.Spatial },
				{ "MinDistance", c.MinDistance },
				{ "MaxDistance", c.MaxDistance },
				{ "Bus", EnumName(AudioBusNames, c.Bus) },
				{ "Stream", c.Stream },
				{ "Doppler", c.Doppler },
				{ "LowPass", c.LowPass },
				{ "ReverbSend", c.ReverbSend },
				{ "HighPass", c.HighPass },
				{ "Echo", c.Echo },
				{ "Distortion", c.Distortion },
			};
		}

		json Write(const AudioReverbZoneComponent& c)
		{
			return { { "Enabled", c.Enabled }, { "Priority", c.Priority }, { "Radius", c.Radius }, { "BlendDistance", c.BlendDistance }, { "RoomSize", c.RoomSize }, { "Damping", c.Damping }, { "Width", c.Width } };
		}

		json Write(const AudioListenerComponent& c)
		{
			return { { "Active", c.Active } };
		}
		json Write(const ParticleEmitterComponent& c)
		{
			return {
				{ "PlayOnStart", c.PlayOnStart },
				{ "Looping", c.Looping },
				{ "Duration", c.Duration },
				{ "MaxParticles", c.MaxParticles },
				{ "Rate", c.Rate },
				{ "BurstCount", c.BurstCount },
				{ "Shape", EnumName(s_ParticleShapeNames, c.Shape) },
				{ "ShapeRadius", c.ShapeRadius },
				{ "BoxExtents", ToJson(c.BoxExtents) },
				{ "ConeAngle", c.ConeAngle },
				{ "LifetimeMin", c.LifetimeMin },
				{ "LifetimeMax", c.LifetimeMax },
				{ "SpeedMin", c.SpeedMin },
				{ "SpeedMax", c.SpeedMax },
				{ "Velocity", ToJson(c.Velocity) },
				{ "GravityScale", c.GravityScale },
				{ "Drag", c.Drag },
				{ "SimulationSpace", EnumName(s_ParticleSpaceNames, c.SimulationSpace) },
				{ "StartColor", ToJson(c.StartColor) },
				{ "EndColor", ToJson(c.EndColor) },
				{ "StartSize", c.StartSize },
				{ "EndSize", c.EndSize },
				{ "StartSizeMin", c.StartSizeMin },
				{ "StartSizeMax", c.StartSizeMax },
				{ "StartColorMin", ToJson(c.StartColorMin) },
				{ "StartColorMax", ToJson(c.StartColorMax) },
				{ "RotationMin", glm::degrees(c.RotationMin) },
				{ "RotationMax", glm::degrees(c.RotationMax) },
				{ "AngularVelocityMin", glm::degrees(c.AngularVelocityMin) },
				{ "AngularVelocityMax", glm::degrees(c.AngularVelocityMax) },
				{ "SoftDistance", c.SoftDistance },
				{ "Lit", c.Lit },
				{ "Preview", c.Preview },
				{ "SizeCurve", ListToJson(c.SizeCurve) },
				{ "ColorCurve", ListToJson(c.ColorCurve) },
				{ "Blend", EnumName(s_ParticleBlendNames, c.Blend) },
				{ "Texture", c.Texture },
			};
		}

		json Write(const UIRectComponent& c)
		{
			return {
				{ "Visible", c.Visible },
				{ "ClipChildren", c.ClipChildren },
				{ "Rotation", glm::degrees(c.Rotation) },
				{ "Scale", ToJson(c.Scale) },
				{ "AnchorMin", ToJson(c.AnchorMin) },
				{ "AnchorMax", ToJson(c.AnchorMax) },
				{ "Pivot", ToJson(c.Pivot) },
				{ "Position", ToJson(c.Position) },
				{ "Size", ToJson(c.Size) },
			};
		}

		json Write(const UIImageComponent& c)
		{
			return {
				{ "Texture", c.Texture },
				{ "Color", ToJson(c.Color) },
				{ "Slice", ToJson(c.Slice) },
				{ "SliceScale", c.SliceScale },
				{ "UVMin", ToJson(c.UVMin) },
				{ "UVMax", ToJson(c.UVMax) },
				{ "SpriteColumns", c.SpriteColumns },
				{ "SpriteRows", c.SpriteRows },
				{ "SpriteFrame", c.SpriteFrame },
			};
		}

		json Write(const UITextComponent& c)
		{
			return {
				{ "Text", c.Text },
				{ "Font", c.Font },
				{ "FontSize", c.FontSize },
				{ "Color", ToJson(c.Color) },
				{ "HorizontalAlign", EnumName(s_UIHorizontalAlignNames, c.HorizontalAlign) },
				{ "VerticalAlign", EnumName(s_UIVerticalAlignNames, c.VerticalAlign) },
				{ "Wrap", c.Wrap },
				{ "Shadow", c.Shadow },
			};
		}

		template<typename T>
		json WithWidgetFields(json data, const T& c, const char* callbackField, const std::string& callback)
		{
			data["NormalTint"] = ToJson(c.NormalTint);
			data["HighlightTint"] = ToJson(c.HighlightTint);
			data["PressedTint"] = ToJson(c.PressedTint);
			data["DisabledTint"] = ToJson(c.DisabledTint);
			data["Target"] = static_cast<uint64_t>(c.Target);
			data[callbackField] = callback;
			return data;
		}

		json Write(const UIButtonComponent& c)
		{
			return WithWidgetFields({ { "Interactable", c.Interactable } }, c, "OnClick", c.OnClick);
		}

		json Write(const UISliderComponent& c)
		{
			return WithWidgetFields({
										{ "Interactable", c.Interactable },
										{ "Value", c.Value },
										{ "Min", c.Min },
										{ "Max", c.Max },
										{ "Step", c.Step },
										{ "TrackColor", ToJson(c.TrackColor) },
										{ "FillColor", ToJson(c.FillColor) },
										{ "HandleColor", ToJson(c.HandleColor) },
										{ "HandleWidth", c.HandleWidth },
										{ "Vertical", c.Vertical },
									},
									c, "OnValueChanged", c.OnValueChanged);
		}

		json Write(const UICheckboxComponent& c)
		{
			return WithWidgetFields({
										{ "Interactable", c.Interactable },
										{ "Checked", c.Checked },
										{ "BoxColor", ToJson(c.BoxColor) },
										{ "CheckColor", ToJson(c.CheckColor) },
									},
									c, "OnValueChanged", c.OnValueChanged);
		}

		json Write(const UITextInputComponent& c)
		{
			return WithWidgetFields({ { "Interactable", c.Interactable }, { "ReadOnly", c.ReadOnly }, { "Text", c.Text }, { "Placeholder", c.Placeholder }, { "MaxLength", c.MaxLength } }, c, "OnValueChanged", c.OnValueChanged);
		}

		json Write(const UIDropdownComponent& c)
		{
			return WithWidgetFields({ { "Interactable", c.Interactable }, { "Options", c.Options }, { "Selected", c.Selected } }, c, "OnValueChanged", c.OnValueChanged);
		}

		json Write(const UIScrollViewComponent& c)
		{
			return { { "Interactable", c.Interactable }, { "ContentSize", ToJson(c.ContentSize) }, { "Offset", ToJson(c.Offset) }, { "ScrollSpeed", c.ScrollSpeed } };
		}

		json Write(const UIPanelComponent& c)
		{
			return { { "BlockInput", c.BlockInput } };
		}

		json Write(const UINavigationComponent& c)
		{
			return { { "Up", static_cast<uint64_t>(c.Up) }, { "Down", static_cast<uint64_t>(c.Down) }, { "Left", static_cast<uint64_t>(c.Left) }, { "Right", static_cast<uint64_t>(c.Right) } };
		}

		json Write(const UIFocusRingComponent& c)
		{
			return { { "Color", ToJson(c.Color) }, { "Thickness", c.Thickness } };
		}

		json Write(const PrefabComponent& c)
		{
			return { { "Prefab", c.Prefab } };
		}

		// --- Camera adapter: SceneCamera keeps its state behind setters ---------------------------

		CameraFields ToFields(const CameraComponent& c)
		{
			const SceneCamera& camera = c.Camera;
			return { camera.GetProjectionType(), camera.GetPerspectiveVerticalFov(), camera.GetPerspectiveNearClip(), camera.GetPerspectiveFarClip(),
					 camera.GetOrthographicSize(), camera.GetOrthographicNearClip(), camera.GetOrthographicFarClip(), c.Primary, c.FixedAspectRatio };
		}

		void ApplyFields(CameraComponent& c, const CameraFields& f)
		{
			c.Camera.SetPerspective(f.Fov, f.Near, f.Far);
			c.Camera.SetOrthographicSize(f.OrthoSize);
			c.Camera.SetOrthographicNearClip(f.OrthoNear);
			c.Camera.SetOrthographicFarClip(f.OrthoFar);
			c.Camera.SetProjectionType(f.Projection);
			c.Primary = f.Primary;
			c.FixedAspectRatio = f.FixedAspectRatio;
		}

		// Fills the info's field names, types, enum options and entity fields from the component's field list.
		template<typename T>
		void CollectFields(ComponentInfo& info)
		{
			FieldReader collector(nullptr);
			T dummy{};
			Fields(collector, dummy);
			info.Fields = collector.GetFieldNames();
			info.FieldTypes = collector.GetFieldTypes();
			info.EnumOptions = collector.GetEnumOptions();
			info.EntityFields = collector.GetEntityFields();
			info.AssetFields = collector.GetAssetFields();
		}

		// Rewrites a component's entity references through its JSON form, so every component with UUID
		// fields gets remapping from its field list alone.
		void InstallEntityRemap(ComponentInfo& info)
		{
			if (info.EntityFields.empty())
				return;
			info.RemapEntityReferences = [name = info.Name, fields = info.EntityFields, has = info.Has, serialize = info.Serialize,
										  deserialize = info.Deserialize](Entity entity, const std::unordered_map<UUID, UUID>& remap) {
				if (!has(entity))
					return;
				json patch = json::object();
				const json data = serialize(entity);
				for (const std::string& field : fields)
				{
					auto it = remap.find(UUID(data[field].get<uint64_t>()));
					if (it != remap.end())
						patch[field] = static_cast<uint64_t>(it->second);
				}
				// Deserialize replaces through the registry, so a running physics world rebuilds joints.
				std::string error;
				if (!patch.empty() && !deserialize(entity, patch, error))
					BS_CORE_ERROR("Remapping entity references of {} on '{}' failed: {}", name, entity.GetName(), error);
			};
		}

		// Checks a component's values after a read: the problem, or an empty string when they are valid.
		// Components without an overload accept whatever their fields' types accept. Overloads must be
		// declared here, before MakeInfo: its unqualified call does not find later ones (they live in this
		// unnamed namespace, outside argument-dependent lookup) and would silently use this template.
		template<typename T>
		std::string Validate(const T&)
		{
			return {};
		}

		std::string Validate(const MaterialComponent& c)
		{
			// The material file and importer rules, so an inline material is valid exactly when it would be as a file. (Its UV
			// values are named as TextureTransforms there; JSON readers reject the only invalid ones, non-finite numbers.)
			if (auto problem = MaterialAsset::Validate(MaterialAsset::ScalarsFromComponent(c)))
				return *problem;
			// Unresolved UUID references stay opaque keys so the project validator can report them.
			if (!c.Material.empty() && !MaterialAsset::IsMaterialKey(c.Material) && !AssetRegistry::IsUnresolvedKey(c.Material))
				return fmt::format("Material '{}' must be a .bmaterial file or <.gltf/.glb model>#material/<n>", c.Material);
			return {};
		}

		std::string Validate(const MeshComponent& c)
		{
			return !std::isfinite(c.LODDistance) || c.LODDistance < 0 || c.LODDistance > MeshComponent::MaxLODDistance ? "LODDistance must be in [0, 1000000]" : "";
		}

		std::string Validate(const PostProcessComponent& c)
		{
			return ValidatePostProcess(c);
		}

		std::string Validate(const PointLightComponent& light)
		{
			if (!std::isfinite(light.Radius) || light.Radius < 0.0f)
				return fmt::format("Radius {} must be finite and nonnegative", light.Radius);
			return ValidateLocalShadowSoftness(light.ShadowSoftness);
		}

		std::string Validate(const SpotLightComponent& light)
		{
			if (!std::isfinite(light.Range) || light.Range < 0.0f)
				return fmt::format("Range {} must be finite and nonnegative", light.Range);
			return ValidateLocalShadowSoftness(light.ShadowSoftness);
		}

		std::string Validate(const AudioReverbZoneComponent& c)
		{
			return ValidateAudioReverbZone(c);
		}
		std::string Validate(const AudioSourceComponent& source)
		{
			return ValidateAudioSource(source);
		}

		std::string Validate(const PropertyAnimationComponent& c)
		{
			return ValidatePropertyAnimation(c);
		}
		std::string Validate(const AnimatorComponent& c)
		{
			if (!std::isfinite(c.LODDistance) || c.LODDistance < 0)
				return fmt::format("LODDistance must be finite and nonnegative (got {})", c.LODDistance);
			if (c.LODInterval < 1 || c.LODInterval > AnimatorComponent::MaxLODInterval)
				return fmt::format("LODInterval must be between 1 and {} (got {})", AnimatorComponent::MaxLODInterval, c.LODInterval);
			if (!std::isfinite(c.Speed) || c.Speed < 0.0f || c.Speed > MaxAnimatorSpeed)
				return "Speed must be finite and between 0 and 100";
			// C++ code can grow the lists past what the reader accepts.
			if (c.Events.size() > AnimatorComponent::MaxEvents || c.BlendSpaces.size() > AnimatorComponent::MaxBlendSpaces || c.Layers.size() > AnimatorComponent::MaxLayers)
				return fmt::format("Animator holds at most {} events, {} blend spaces and {} layers", AnimatorComponent::MaxEvents, AnimatorComponent::MaxBlendSpaces, AnimatorComponent::MaxLayers);
			if (c.ClipLibraries.size() > AnimatorComponent::MaxClipLibraries)
				return fmt::format("Animator holds at most {} clip libraries", AnimatorComponent::MaxClipLibraries);
			for (size_t i = 0; i < c.ClipLibraries.size(); i++)
			{
				const auto& joints = c.ClipLibraries[i].Joints;
				if (joints.size() > AnimationClipLibrary::MaxJoints)
					return fmt::format("ClipLibraries[{}] holds at most {} joints", i, AnimationClipLibrary::MaxJoints);
				// One source drives one target. Empty names are the inspector's unnamed additions.
				std::unordered_map<std::string_view, size_t> targets, sources;
				for (size_t j = 0; j < joints.size(); j++)
				{
					if (!joints[j].Target.empty())
						if (const auto [entry, added] = targets.try_emplace(joints[j].Target, j); !added)
							return fmt::format("ClipLibraries[{}].Joints[{}].Target '{}' is already used by Joints[{}]", i, j, joints[j].Target, entry->second);
					if (!joints[j].Source.empty())
						if (const auto [entry, added] = sources.try_emplace(joints[j].Source, j); !added)
							return fmt::format("ClipLibraries[{}].Joints[{}].Source '{}' is already used by Joints[{}]", i, j, joints[j].Source, entry->second);
				}
			}
			for (size_t i = 0; i < c.Events.size(); i++)
			{
				const AnimationEvent& event = c.Events[i];
				if (event.Name.empty())
					return fmt::format("Events[{}].Name must not be empty", i);
				if (!std::isfinite(event.Time) || event.Time < 0.0f)
					return fmt::format("Events[{}].Time {} must be finite and nonnegative", i, event.Time);
			}
			for (size_t i = 0; i < c.BlendSpaces.size(); i++)
			{
				const AnimationBlendSpace& space = c.BlendSpaces[i];
				if (space.Name.empty())
					return fmt::format("BlendSpaces[{}].Name must not be empty", i);
				for (size_t j = 0; j < i; j++)
				{
					if (c.BlendSpaces[j].Name == space.Name)
						return fmt::format("BlendSpaces[{}].Name '{}' is already used by BlendSpaces[{}]", i, space.Name, j);
				}
				if (space.Samples.size() > AnimatorComponent::MaxBlendSamples)
					return fmt::format("BlendSpaces[{}] holds at most {} samples", i, AnimatorComponent::MaxBlendSamples);
				// Samples may share a position (they share its weight), so an editor can add one at the default.
				for (size_t j = 0; j < space.Samples.size(); j++)
				{
					const AnimationBlendSample& sample = space.Samples[j];
					if (!std::isfinite(sample.X) || !std::isfinite(sample.Y))
						return fmt::format("BlendSpaces[{}].Samples[{}] position ({}, {}) must be finite", i, j, sample.X, sample.Y);
					if (!std::isfinite(sample.Speed) || sample.Speed < 0.0f)
						return fmt::format("BlendSpaces[{}].Samples[{}].Speed {} must be finite and nonnegative", i, j, sample.Speed);
				}
			}
			for (size_t i = 0; i < c.Layers.size(); i++)
			{
				const AnimationLayer& layer = c.Layers[i];
				if (layer.Name.empty())
					return fmt::format("Layers[{}].Name must not be empty", i);
				for (size_t j = 0; j < i; j++)
				{
					if (c.Layers[j].Name == layer.Name)
						return fmt::format("Layers[{}].Name '{}' is already used by Layers[{}]", i, layer.Name, j);
				}
				if (!std::isfinite(layer.Weight) || layer.Weight < 0.0f || layer.Weight > 1.0f)
					return fmt::format("Layers[{}].Weight {} must be between 0 and 1", i, layer.Weight);
				if (!std::isfinite(layer.Speed) || layer.Speed < 0.0f || layer.Speed > MaxAnimatorSpeed)
					return fmt::format("Layers[{}].Speed {} must be finite and between 0 and {}", i, layer.Speed, MaxAnimatorSpeed);
				if (!std::isfinite(layer.ReferenceTime) || layer.ReferenceTime < 0.0f)
					return fmt::format("Layers[{}].ReferenceTime {} must be finite and nonnegative", i, layer.ReferenceTime);
				if (layer.Mask.size() > AnimationLayer::MaxMaskJoints)
					return fmt::format("Layers[{}] mask holds at most {} joints", i, AnimationLayer::MaxMaskJoints);
				for (size_t j = 0; j < layer.Mask.size(); j++)
				{
					const AnimationMaskJoint& joint = layer.Mask[j];
					if (!std::isfinite(joint.Weight) || joint.Weight < 0.0f || joint.Weight > 1.0f)
						return fmt::format("Layers[{}].Mask[{}].Weight {} must be between 0 and 1", i, j, joint.Weight);
					// Empty entries are the inspector's unnamed additions.
					for (size_t k = 0; k < j && !joint.Joint.empty(); k++)
					{
						if (layer.Mask[k].Joint == joint.Joint)
							return fmt::format("Layers[{}].Mask[{}].Joint '{}' is already used by Mask[{}]", i, j, joint.Joint, k);
					}
				}
			}
			return {};
		}

		std::string Validate(const InverseKinematicsComponent& c)
		{
			if (c.Chains.size() > InverseKinematicsComponent::MaxChains)
				return fmt::format("InverseKinematics holds at most {} chains", InverseKinematicsComponent::MaxChains);
			for (const auto& [name, value] : { std::pair<const char*, float>{ "RayHeight", c.RayHeight }, { "RayDepth", c.RayDepth }, { "Smoothing", c.Smoothing } })
			{
				if (!std::isfinite(value) || value < 0.0f || value > 100.0f)
					return fmt::format("{} {} must be between 0 and 100", name, value);
			}
			for (size_t i = 0; i < c.Chains.size(); i++)
			{
				const IKChain& chain = c.Chains[i];
				if (chain.Name.empty())
					return fmt::format("Chains[{}].Name must not be empty", i);
				for (size_t j = 0; j < i; j++)
				{
					if (c.Chains[j].Name == chain.Name)
						return fmt::format("Chains[{}].Name '{}' is already used by Chains[{}]", i, chain.Name, j);
				}
				if (chain.ChainLength < 1 || chain.ChainLength > IKChain::MaxChainLength)
					return fmt::format("Chains[{}].ChainLength {} must be between 1 and {}", i, chain.ChainLength, IKChain::MaxChainLength);
				if (!Math::IsFinite(chain.Axis) || glm::length(chain.Axis) < 1e-4f)
					return fmt::format("Chains[{}].Axis must be finite and nonzero", i);
				if (!Math::IsFinite(chain.Pole))
					return fmt::format("Chains[{}].Pole must be finite", i);
				if (!std::isfinite(chain.MaxAngle) || chain.MaxAngle < 0.0f || chain.MaxAngle > 180.0f)
					return fmt::format("Chains[{}].MaxAngle {} must be between 0 and 180 degrees", i, chain.MaxAngle);
				if (!std::isfinite(chain.Weight) || chain.Weight < 0.0f || chain.Weight > 1.0f)
					return fmt::format("Chains[{}].Weight {} must be between 0 and 1", i, chain.Weight);
				if (chain.Ground && chain.Type != IKChainType::TwoBone)
					return fmt::format("Chains[{}]: only TwoBone chains can be Ground chains", i);
			}
			if (c.RayHeight + c.RayDepth <= 0.0f && std::ranges::any_of(c.Chains, &IKChain::Ground))
				return "RayHeight + RayDepth must be positive for Ground chains";
			return {};
		}

		std::string Validate(const SpringBoneComponent& c)
		{
			if (c.Chains.size() > SpringBoneComponent::MaxChains)
				return fmt::format("SpringBone holds at most {} chains", SpringBoneComponent::MaxChains);
			if (c.Colliders.size() > SpringBoneComponent::MaxColliders)
				return fmt::format("SpringBone holds at most {} colliders", SpringBoneComponent::MaxColliders);
			if (!Math::IsFinite(c.Wind))
				return "Wind must be finite";
			const auto inRange = [](float value, float low, float high) { return std::isfinite(value) && value >= low && value <= high; };
			for (size_t i = 0; i < c.Chains.size(); i++)
			{
				const SpringChain& chain = c.Chains[i];
				if (chain.Name.empty())
					return fmt::format("Chains[{}].Name must not be empty", i);
				for (size_t j = 0; j < i; j++)
				{
					if (c.Chains[j].Name == chain.Name)
						return fmt::format("Chains[{}].Name '{}' is already used by Chains[{}]", i, chain.Name, j);
				}
				if (chain.ChainLength > SpringChain::MaxChainLength)
					return fmt::format("Chains[{}].ChainLength {} must be between 0 and {}", i, chain.ChainLength, SpringChain::MaxChainLength);
				if (!inRange(chain.Stiffness, 0.0f, SpringChain::MaxStiffness))
					return fmt::format("Chains[{}].Stiffness {} must be between 0 and {}", i, chain.Stiffness, SpringChain::MaxStiffness);
				if (!inRange(chain.Damping, 0.0f, SpringChain::MaxDamping))
					return fmt::format("Chains[{}].Damping {} must be between 0 and {}", i, chain.Damping, SpringChain::MaxDamping);
				if (!inRange(chain.GravityFactor, -SpringChain::MaxGravityFactor, SpringChain::MaxGravityFactor))
					return fmt::format("Chains[{}].GravityFactor {} must be between {} and {}", i, chain.GravityFactor, -SpringChain::MaxGravityFactor, SpringChain::MaxGravityFactor);
				if (!inRange(chain.Radius, 0.0f, SpringChain::MaxRadius))
					return fmt::format("Chains[{}].Radius {} must be between 0 and {}", i, chain.Radius, SpringChain::MaxRadius);
				if (!inRange(chain.Weight, 0.0f, 1.0f))
					return fmt::format("Chains[{}].Weight {} must be between 0 and 1", i, chain.Weight);
			}
			for (size_t i = 0; i < c.Colliders.size(); i++)
			{
				const SpringCollider& collider = c.Colliders[i];
				if (!Math::IsFinite(collider.Offset))
					return fmt::format("Colliders[{}].Offset must be finite", i);
				if (!inRange(collider.Radius, 0.0f, SpringCollider::MaxSize))
					return fmt::format("Colliders[{}].Radius {} must be between 0 and {}", i, collider.Radius, SpringCollider::MaxSize);
				if (!inRange(collider.HalfHeight, 0.0f, SpringCollider::MaxSize))
					return fmt::format("Colliders[{}].HalfHeight {} must be between 0 and {}", i, collider.HalfHeight, SpringCollider::MaxSize);
			}
			return {};
		}

		std::string Validate(const BoneAttachmentComponent& c)
		{
			const auto finite = [](const auto& v) { return !glm::any(glm::isnan(v)) && !glm::any(glm::isinf(v)); };
			// The registry only writes finite rotations; C++ code may not.
			if (!finite(c.Translation) || !finite(c.Scale) || !finite(c.Rotation))
				return "Translation, Rotation and Scale must be finite";
			return {};
		}

		std::string Validate(const ParticleEmitterComponent& emitter)
		{
			return ValidateParticleEmitter(emitter);
		}

		std::string Validate(const UIRectComponent& rect)
		{
			return ValidateUIRect(rect);
		}

		std::string Validate(const UIImageComponent& image)
		{
			return ValidateUIImage(image);
		}

		std::string Validate(const UITextComponent& text)
		{
			return ValidateUIText(text);
		}

		std::string Validate(const UIButtonComponent& button)
		{
			return ValidateUIButton(button);
		}

		std::string Validate(const UISliderComponent& slider)
		{
			return ValidateUISlider(slider);
		}

		std::string Validate(const UICheckboxComponent& checkbox)
		{
			return ValidateUICheckbox(checkbox);
		}

		std::string Validate(const UITextInputComponent& c)
		{
			return ValidateUITextInput(c);
		}

		std::string Validate(const UIDropdownComponent& c)
		{
			return ValidateUIDropdown(c);
		}

		std::string Validate(const UIScrollViewComponent& c)
		{
			return ValidateUIScrollView(c);
		}

		std::string Validate(const UIFocusRingComponent& c)
		{
			return ValidateUIFocusRing(c);
		}

		template<typename T>
		ComponentInfo MakeInfo(const char* name, bool isCore = false)
		{
			ComponentInfo info;
			info.Name = name;
			info.IsCore = isCore;
			CollectFields<T>(info);
			info.Has = [](Entity entity) { return entity.HasComponent<T>(); };
			info.Add = [](Entity entity) {
				if (!entity.HasComponent<T>())
					entity.AddComponent<T>();
			};
			info.Remove = [](Entity entity) {
				if (entity.HasComponent<T>())
					entity.RemoveComponent<T>();
			};
			info.Serialize = [](Entity entity) { return Write(entity.GetComponent<T>()); };
			info.Deserialize = [fields = info.Fields](Entity entity, const json& data, std::string& outError) {
				if (!data.is_object())
				{
					outError = "component data must be a JSON object";
					return false;
				}
				// Read into a copy so a failure leaves the component (or its absence) untouched. A missing
				// component is added once, with its final values: systems that react to its construction
				// (PlayOnStart emitters, physics bodies) never see the defaults first.
				T value = entity.HasComponent<T>() ? entity.GetComponent<T>() : T{};
				try
				{
					FieldReader reader(&data);
					Fields(reader, value);
					reader.CheckUnknownKeys(fields);
				}
				catch (const std::exception& e)
				{
					outError = e.what();
					return false;
				}
				if (std::string problem = Validate(value); !problem.empty())
				{
					outError = std::move(problem);
					return false;
				}
				// Replace through the registry so on_update observers see the change.
				entity.AddOrReplaceComponent<T>(std::move(value));
				return true;
			};
			InstallEntityRemap(info);
			return info;
		}

		ComponentInfo MakeCameraInfo()
		{
			ComponentInfo info;
			info.Name = "Camera";
			CollectFields<CameraFields>(info);
			info.Has = [](Entity entity) { return entity.HasComponent<CameraComponent>(); };
			info.Add = [](Entity entity) {
				if (!entity.HasComponent<CameraComponent>())
					entity.AddComponent<CameraComponent>();
			};
			info.Remove = [](Entity entity) {
				if (entity.HasComponent<CameraComponent>())
					entity.RemoveComponent<CameraComponent>();
			};
			info.Serialize = [](Entity entity) { return Write(ToFields(entity.GetComponent<CameraComponent>())); };
			info.Deserialize = [fields = info.Fields](Entity entity, const json& data, std::string& outError) {
				if (!data.is_object())
				{
					outError = "component data must be a JSON object";
					return false;
				}
				CameraComponent camera = entity.HasComponent<CameraComponent>() ? entity.GetComponent<CameraComponent>() : CameraComponent{};
				CameraFields value = ToFields(camera);
				try
				{
					FieldReader reader(&data);
					Fields(reader, value);
					reader.CheckUnknownKeys(fields);
				}
				catch (const std::exception& e)
				{
					outError = e.what();
					return false;
				}
				// Reject values that would produce an infinite or NaN projection.
				if (value.Fov <= 0.0f || value.Fov >= glm::radians(179.0f))
				{
					outError = "FOV must be between 0 and 179 degrees";
					return false;
				}
				if (value.Near <= 0.0f || value.Far <= value.Near)
				{
					outError = "Near must be positive and Far greater than Near";
					return false;
				}
				if (value.OrthoSize <= 0.0f || value.OrthoFar <= value.OrthoNear)
				{
					outError = "OrthoSize must be positive and OrthoFar greater than OrthoNear";
					return false;
				}
				ApplyFields(camera, value);
				entity.AddOrReplaceComponent<CameraComponent>(camera);
				return true;
			};
			return info;
		}

		std::vector<ComponentInfo> BuildRegistry()
		{
			std::vector<ComponentInfo> infos;
			infos.push_back(MakeInfo<TagComponent>("Tag", true));
			infos.push_back(MakeInfo<TransformComponent>("Transform", true));
			infos.push_back(MakeCameraInfo());
			infos.push_back(MakeInfo<MeshComponent>("Mesh"));
			infos.push_back(MakeInfo<AnimatorComponent>("Animator"));
			infos.push_back(MakeInfo<PropertyAnimationComponent>("PropertyAnimation"));
			infos.push_back(MakeInfo<BoneAttachmentComponent>("BoneAttachment"));
			infos.push_back(MakeInfo<InverseKinematicsComponent>("InverseKinematics"));
			infos.push_back(MakeInfo<SpringBoneComponent>("SpringBone"));
			infos.push_back(MakeInfo<MaterialComponent>("Material"));
			infos.push_back(MakeInfo<DirectionalLightComponent>("DirectionalLight"));
			infos.push_back(MakeInfo<PointLightComponent>("PointLight"));
			infos.push_back(MakeInfo<SpotLightComponent>("SpotLight"));
			infos.push_back(MakeInfo<SkyLightComponent>("SkyLight"));
			infos.push_back(MakeInfo<PostProcessComponent>("PostProcess"));
			infos.push_back(MakeInfo<RigidBodyComponent>("RigidBody"));
			infos.push_back(MakeInfo<BoxColliderComponent>("BoxCollider"));
			infos.push_back(MakeInfo<SphereColliderComponent>("SphereCollider"));
			infos.push_back(MakeInfo<CapsuleColliderComponent>("CapsuleCollider"));
			infos.push_back(MakeInfo<MeshColliderComponent>("MeshCollider"));
			infos.push_back(MakeInfo<JointComponent>("Joint"));
			infos.push_back(MakeInfo<CharacterControllerComponent>("CharacterController"));
			infos.push_back(MakeInfo<ScriptComponent>("Script"));
			infos.push_back(MakeInfo<AudioSourceComponent>("AudioSource"));
			infos.push_back(MakeInfo<AudioListenerComponent>("AudioListener"));
			infos.push_back(MakeInfo<AudioReverbZoneComponent>("AudioReverbZone"));
			infos.push_back(MakeInfo<ParticleEmitterComponent>("ParticleEmitter"));
			infos.push_back(MakeInfo<UIRectComponent>("UIRect"));
			infos.push_back(MakeInfo<UIImageComponent>("UIImage"));
			infos.push_back(MakeInfo<UITextComponent>("UIText"));
			infos.push_back(MakeInfo<UIButtonComponent>("UIButton"));
			infos.push_back(MakeInfo<UISliderComponent>("UISlider"));
			infos.push_back(MakeInfo<UICheckboxComponent>("UICheckbox"));
			infos.push_back(MakeInfo<UITextInputComponent>("UITextInput"));
			infos.push_back(MakeInfo<UIDropdownComponent>("UIDropdown"));
			infos.push_back(MakeInfo<UIScrollViewComponent>("UIScrollView"));
			infos.push_back(MakeInfo<UIPanelComponent>("UIPanel"));
			infos.push_back(MakeInfo<UINavigationComponent>("UINavigation"));
			infos.push_back(MakeInfo<UIFocusRingComponent>("UIFocusRing"));
			infos.push_back(MakeInfo<PrefabComponent>("Prefab"));
			return infos;
		}

	}

	std::string ComponentFieldType::ToString() const
	{
		static constexpr const char* const Names[] = { "number", "integer", "boolean", "string", "object", "entity", "object[]", "enum" };
		static_assert(std::size(Names) == static_cast<size_t>(ComponentFieldKind::Enum) + 1, "one name per field kind");
		const char* name = EnumNameOrNull(Names, Kind);
		std::string text = name ? name : "unknown";
		if (Count > 0)
			text += "[" + std::to_string(Count) + "]";
		return text;
	}

	ComponentFieldType ComponentFieldType::List(uint32_t maxCount, std::shared_ptr<const ComponentObjectSchema> element)
	{
		BS_CORE_ASSERT(maxCount > 0 && element, "a list field needs a length limit and an element schema");
		return { ComponentFieldKind::List, 0, maxCount, std::move(element) };
	}

	const std::vector<ComponentInfo>& ComponentRegistry::GetAll()
	{
		static const std::vector<ComponentInfo> s_Registry = BuildRegistry();
		return s_Registry;
	}

	const ComponentInfo* ComponentRegistry::Find(std::string_view name)
	{
		for (const ComponentInfo& info : GetAll())
		{
			if (info.Name == name)
				return &info;
		}
		return nullptr;
	}

	std::string ComponentRegistry::GetNameList()
	{
		std::string names;
		for (const ComponentInfo& info : GetAll())
			names += (names.empty() ? "" : ", ") + info.Name;
		return names;
	}

	void ComponentRegistry::RemapEntityReferences(Entity entity, const std::unordered_map<UUID, UUID>& remap)
	{
		for (const ComponentInfo& info : GetAll())
		{
			if (info.RemapEntityReferences)
				info.RemapEntityReferences(entity, remap);
		}
	}

	bool ComponentRegistry::AddOrPatch(Entity entity, std::string_view name, const nlohmann::json& data, std::string& outError)
	{
		const ComponentInfo* info = Find(name);
		if (!info)
		{
			outError = "unknown component '" + std::string(name) + "' (valid: " + GetNameList() + ")";
			return false;
		}

		// Deserialize adds a missing component itself, once and only on success.
		if (!info->Deserialize(entity, data, outError))
		{
			outError = std::string(name) + ": " + outError;
			return false;
		}
		return true;
	}

}
