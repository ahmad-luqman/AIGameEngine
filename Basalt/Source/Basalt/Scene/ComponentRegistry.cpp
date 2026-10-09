#include "Basalt/Scene/ComponentRegistry.h"

#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Scene/Entity.h"

#include <glm/gtc/type_ptr.hpp>

#include <iterator>
#include <stdexcept>
#include <unordered_set>

namespace Basalt {

	namespace {

		using json = nlohmann::json;

		// Reads named fields from a JSON object into a component, tracking which keys were used so that
		// unknown keys can be reported. In collect mode it only records field names.
		class FieldReader
		{
		public:
			explicit FieldReader(const json* data)
				: m_Data(data)
			{
			}

			std::vector<std::string>& GetFieldNames() { return m_FieldNames; }
			std::map<std::string, std::vector<std::string>>& GetEnumOptions() { return m_EnumOptions; }
			std::vector<std::string>& GetEntityFields() { return m_EntityFields; }

			template<typename T>
			void Field(const char* name, T& value)
			{
				const json* element = Find(name);
				if (element)
					Read(name, *element, value);
			}

			// A field holding another entity's UUID (0 = none). Recorded so tools can resolve names and
			// copies can remap the reference.
			void Field(const char* name, UUID& value)
			{
				m_EntityFields.emplace_back(name);
				const json* element = Find(name);
				if (element)
					Read(name, *element, value);
			}

			// Rotation stored as Euler angles in degrees.
			void RotationField(const char* name, glm::quat& value)
			{
				const json* element = Find(name);
				if (!element)
					return;
				glm::vec3 degrees;
				Read(name, *element, degrees);
				value = Math::QuatFromEuler(glm::radians(degrees));
			}

			// Angle stored in degrees, held in radians.
			void AngleField(const char* name, float& radiansValue)
			{
				const json* element = Find(name);
				if (!element)
					return;
				float degrees = 0.0f;
				Read(name, *element, degrees);
				radiansValue = glm::radians(degrees);
			}

			template<typename E, size_t N>
			void EnumField(const char* name, E& value, const char* const (&names)[N])
			{
				m_EnumOptions[name] = std::vector<std::string>(std::begin(names), std::end(names));
				const json* element = Find(name);
				if (!element)
					return;
				if (!element->is_string())
					throw std::runtime_error(std::string("field '") + name + "' must be a string");
				const std::string text = element->get<std::string>();
				for (size_t i = 0; i < N; i++)
				{
					if (text == names[i])
					{
						value = static_cast<E>(i);
						return;
					}
				}
				std::string valid;
				for (size_t i = 0; i < N; i++)
					valid += (i ? ", " : "") + std::string(names[i]);
				throw std::runtime_error(std::string("field '") + name + "' has invalid value '" + text + "' (valid: " + valid + ")");
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
					throw std::runtime_error("unknown field '" + it.key() + "' (valid: " + valid + ")");
				}
			}

		private:
			const json* Find(const char* name)
			{
				m_FieldNames.emplace_back(name);
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

		private:
			const json* m_Data = nullptr;
			std::unordered_set<std::string> m_Consumed;
			std::vector<std::string> m_FieldNames;
			std::map<std::string, std::vector<std::string>> m_EnumOptions;
			std::vector<std::string> m_EntityFields;
		};

		template<glm::length_t L>
		json ToJson(const glm::vec<L, float, glm::defaultp>& value)
		{
			json array = json::array();
			for (glm::length_t i = 0; i < L; i++)
				array.push_back(value[i]);
			return array;
		}

		json RotationToJson(const glm::quat& rotation)
		{
			return ToJson(glm::degrees(Math::EulerFromQuat(rotation)));
		}

		constexpr const char* s_RigidBodyTypeNames[] = { "Static", "Dynamic", "Kinematic" };
		constexpr const char* s_ProjectionTypeNames[] = { "Perspective", "Orthographic" };
		constexpr const char* s_JointTypeNames[] = { "Fixed", "Point", "Hinge", "Slider", "Distance" };
		constexpr const char* s_JointMotorModeNames[] = { "Off", "Velocity", "Position" };
		// Names are indexed by enum value; a new enum value needs a name here.
		static_assert(std::size(s_RigidBodyTypeNames) == static_cast<size_t>(RigidBodyType::Kinematic) + 1);
		static_assert(std::size(s_JointTypeNames) == static_cast<size_t>(JointType::Distance) + 1);
		static_assert(std::size(s_JointMotorModeNames) == static_cast<size_t>(JointMotorMode::Position) + 1);

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
			r.Field("Mesh", c.Mesh);
			r.Field("MeshIndex", c.MeshIndex);
			r.Field("CastShadows", c.CastShadows);
		}

		void Fields(FieldReader& r, MaterialComponent& c)
		{
			r.Field("AlbedoColor", c.AlbedoColor);
			r.Field("Metallic", c.Metallic);
			r.Field("Roughness", c.Roughness);
			r.Field("EmissiveColor", c.EmissiveColor);
			r.Field("EmissiveIntensity", c.EmissiveIntensity);
			r.Field("AlbedoMap", c.AlbedoMap);
			r.Field("NormalMap", c.NormalMap);
			r.Field("MetallicRoughnessMap", c.MetallicRoughnessMap);
			r.Field("OcclusionMap", c.OcclusionMap);
			r.Field("EmissiveMap", c.EmissiveMap);
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
		}

		void Fields(FieldReader& r, SpotLightComponent& c)
		{
			r.Field("Color", c.Color);
			r.Field("Intensity", c.Intensity);
			r.Field("Range", c.Range);
			r.Field("InnerConeAngle", c.InnerConeAngle);
			r.Field("OuterConeAngle", c.OuterConeAngle);
		}

		void Fields(FieldReader& r, SkyLightComponent& c)
		{
			r.Field("EnvironmentMap", c.EnvironmentMap);
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
			r.Field("IsTrigger", c.IsTrigger);
			r.Field("FixedRotation", c.FixedRotation);
			r.Field("Layer", c.Layer);
			r.Field("CollisionMask", c.CollisionMask);
		}

		void Fields(FieldReader& r, BoxColliderComponent& c)
		{
			r.Field("HalfExtents", c.HalfExtents);
			r.Field("Offset", c.Offset);
		}

		void Fields(FieldReader& r, SphereColliderComponent& c)
		{
			r.Field("Radius", c.Radius);
			r.Field("Offset", c.Offset);
		}

		void Fields(FieldReader& r, CapsuleColliderComponent& c)
		{
			r.Field("Radius", c.Radius);
			r.Field("HalfHeight", c.HalfHeight);
			r.Field("Offset", c.Offset);
		}

		void Fields(FieldReader& r, JointComponent& c)
		{
			r.EnumField("Type", c.Type, s_JointTypeNames);
			r.Field("ConnectedEntity", c.ConnectedEntity);
			r.Field("Anchor", c.Anchor);
			r.Field("ConnectedAnchor", c.ConnectedAnchor);
			r.Field("Axis", c.Axis);
			r.Field("UseLimits", c.UseLimits);
			r.Field("LimitMin", c.LimitMin);
			r.Field("LimitMax", c.LimitMax);
			r.EnumField("MotorMode", c.MotorMode, s_JointMotorModeNames);
			r.Field("MotorTarget", c.MotorTarget);
			r.Field("MotorMaxForce", c.MotorMaxForce);
			r.Field("BreakForce", c.BreakForce);
			r.Field("BreakTorque", c.BreakTorque);
			r.Field("EnableCollision", c.EnableCollision);
		}

		void Fields(FieldReader& r, ScriptComponent& c)
		{
			r.Field("Script", c.Script);
			r.Field("Properties", c.Properties);
		}

		void Fields(FieldReader& r, AudioSourceComponent& c)
		{
			r.Field("Clip", c.Clip);
			r.Field("Volume", c.Volume);
			r.Field("Pitch", c.Pitch);
			r.Field("Loop", c.Loop);
			r.Field("PlayOnStart", c.PlayOnStart);
			r.Field("Spatial", c.Spatial);
			r.Field("MinDistance", c.MinDistance);
			r.Field("MaxDistance", c.MaxDistance);
		}

		void Fields(FieldReader& r, AudioListenerComponent& c)
		{
			r.Field("Active", c.Active);
		}

		void Fields(FieldReader& r, PrefabComponent& c)
		{
			r.Field("Prefab", c.Prefab);
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
			return { { "Mesh", c.Mesh }, { "MeshIndex", c.MeshIndex }, { "CastShadows", c.CastShadows } };
		}

		json Write(const MaterialComponent& c)
		{
			return {
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
			};
		}

		json Write(const DirectionalLightComponent& c)
		{
			return { { "Color", ToJson(c.Color) }, { "Intensity", c.Intensity }, { "CastShadows", c.CastShadows }, { "ShadowSoftness", c.ShadowSoftness } };
		}

		json Write(const PointLightComponent& c)
		{
			return { { "Color", ToJson(c.Color) }, { "Intensity", c.Intensity }, { "Radius", c.Radius } };
		}

		json Write(const SpotLightComponent& c)
		{
			return { { "Color", ToJson(c.Color) }, { "Intensity", c.Intensity }, { "Range", c.Range }, { "InnerConeAngle", c.InnerConeAngle }, { "OuterConeAngle", c.OuterConeAngle } };
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
				{ "IsTrigger", c.IsTrigger },
				{ "FixedRotation", c.FixedRotation },
				{ "Layer", c.Layer },
				{ "CollisionMask", c.CollisionMask },
			};
		}

		json Write(const BoxColliderComponent& c)
		{
			return { { "HalfExtents", ToJson(c.HalfExtents) }, { "Offset", ToJson(c.Offset) } };
		}
		json Write(const SphereColliderComponent& c)
		{
			return { { "Radius", c.Radius }, { "Offset", ToJson(c.Offset) } };
		}
		json Write(const CapsuleColliderComponent& c)
		{
			return { { "Radius", c.Radius }, { "HalfHeight", c.HalfHeight }, { "Offset", ToJson(c.Offset) } };
		}
		json Write(const JointComponent& c)
		{
			return {
				{ "Type", s_JointTypeNames[static_cast<int>(c.Type)] },
				{ "ConnectedEntity", static_cast<uint64_t>(c.ConnectedEntity) },
				{ "Anchor", ToJson(c.Anchor) },
				{ "ConnectedAnchor", ToJson(c.ConnectedAnchor) },
				{ "Axis", ToJson(c.Axis) },
				{ "UseLimits", c.UseLimits },
				{ "LimitMin", c.LimitMin },
				{ "LimitMax", c.LimitMax },
				{ "MotorMode", s_JointMotorModeNames[static_cast<int>(c.MotorMode)] },
				{ "MotorTarget", c.MotorTarget },
				{ "MotorMaxForce", c.MotorMaxForce },
				{ "BreakForce", c.BreakForce },
				{ "BreakTorque", c.BreakTorque },
				{ "EnableCollision", c.EnableCollision },
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
			};
		}

		json Write(const AudioListenerComponent& c)
		{
			return { { "Active", c.Active } };
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

		template<typename T>
		std::vector<std::string> CollectFieldNames(std::map<std::string, std::vector<std::string>>* outEnumOptions = nullptr, std::vector<std::string>* outEntityFields = nullptr)
		{
			FieldReader collector(nullptr);
			T dummy{};
			Fields(collector, dummy);
			if (outEnumOptions)
				*outEnumOptions = collector.GetEnumOptions();
			if (outEntityFields)
				*outEntityFields = collector.GetEntityFields();
			return collector.GetFieldNames();
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

		template<typename T>
		ComponentInfo MakeInfo(const char* name, bool isCore = false)
		{
			ComponentInfo info;
			info.Name = name;
			info.IsCore = isCore;
			info.Fields = CollectFieldNames<T>(&info.EnumOptions, &info.EntityFields);
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
				// Read into a copy so a failure leaves the component untouched.
				T value = entity.GetComponent<T>();
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
			info.Fields = CollectFieldNames<CameraFields>(&info.EnumOptions);
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
				CameraFields value = ToFields(entity.GetComponent<CameraComponent>());
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
				CameraComponent camera = entity.GetComponent<CameraComponent>();
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
			infos.push_back(MakeInfo<MaterialComponent>("Material"));
			infos.push_back(MakeInfo<DirectionalLightComponent>("DirectionalLight"));
			infos.push_back(MakeInfo<PointLightComponent>("PointLight"));
			infos.push_back(MakeInfo<SpotLightComponent>("SpotLight"));
			infos.push_back(MakeInfo<SkyLightComponent>("SkyLight"));
			infos.push_back(MakeInfo<RigidBodyComponent>("RigidBody"));
			infos.push_back(MakeInfo<BoxColliderComponent>("BoxCollider"));
			infos.push_back(MakeInfo<SphereColliderComponent>("SphereCollider"));
			infos.push_back(MakeInfo<CapsuleColliderComponent>("CapsuleCollider"));
			infos.push_back(MakeInfo<JointComponent>("Joint"));
			infos.push_back(MakeInfo<ScriptComponent>("Script"));
			infos.push_back(MakeInfo<AudioSourceComponent>("AudioSource"));
			infos.push_back(MakeInfo<AudioListenerComponent>("AudioListener"));
			infos.push_back(MakeInfo<PrefabComponent>("Prefab"));
			return infos;
		}

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

		const bool existed = info->Has(entity);
		if (!existed)
			info->Add(entity);
		if (!info->Deserialize(entity, data, outError))
		{
			if (!existed)
				info->Remove(entity);
			outError = std::string(name) + ": " + outError;
			return false;
		}
		return true;
	}

}
