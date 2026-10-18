#include "Basalt/Scripting/ScriptGlue.h"

#include "Basalt/Animation/AnimationSystem.h"
#include "Basalt/Audio/AudioRules.h"
#include "Basalt/Audio/AudioSystem.h"
#include "Basalt/Core/Assert.h"
#include "Basalt/Core/EnumNames.h"
#include "Basalt/Core/Input.h"
#include "Basalt/Core/InputActions.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Particles/ParticleSystem.h"
#include "Basalt/Physics/PhysicsWorld.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Project/SaveData.h"
#include "Basalt/Renderer/DebugDraw.h"
#include "Basalt/Scene/ComponentRegistry.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/Scene/SceneSerializer.h"
#include "Basalt/Scripting/LuaJson.h"
#include "Basalt/Scripting/ScriptEngine.h"
#include "Basalt/Scripting/ScriptEvents.h"
#include "Basalt/Scripting/ScriptTasks.h"
#include "Basalt/Scripting/ScriptUI.h"
#include "Basalt/UI/GameUI.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <spdlog/fmt/ranges.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

namespace Basalt {

	Entity ScriptEntity::Resolve() const
	{
		return SceneContext ? SceneContext->GetEntityByUUID(ID) : Entity{};
	}

	Entity ScriptEntity::ResolveOrThrow() const
	{
		Entity entity = Resolve();
		if (!entity)
			throw std::runtime_error("entity " + std::to_string(static_cast<uint64_t>(ID)) + " no longer exists");
		return entity;
	}

	namespace {

		constexpr const char* NotRunningError = "this function is only available while the scene is running";

		Scene& RequireScene(ScriptContext& context)
		{
			if (!context.SceneContext)
				throw std::runtime_error(NotRunningError);
			return *context.SceneContext;
		}

		// An omitted argument or an explicit nil.
		bool IsNil(const sol::object& value)
		{
			return !value.valid() || value.get_type() == sol::type::lua_nil;
		}

		// A Lua integer, or a float with an integer value in int64 range ([-2^63, 2^63)); nullopt otherwise.
		std::optional<int64_t> ToInteger(const sol::object& value)
		{
			if (value.get_type() != sol::type::number)
				return std::nullopt;
			if (value.is<int64_t>())
				return value.as<int64_t>();
			const double number = value.as<double>();
			if (std::trunc(number) != number || number < -0x1p63 || number >= 0x1p63)
				return std::nullopt;
			return static_cast<int64_t>(number);
		}

		sol::object MakeEntity(const sol::state_view& lua, ScriptContext& context, Entity entity)
		{
			if (!entity)
				return sol::lua_nil;
			return sol::make_object(lua, ScriptEntity{ entity.GetUUID(), context.SceneContext });
		}

		std::string JoinArguments(sol::variadic_args args)
		{
			std::ostringstream stream;
			bool first = true;
			for (const sol::stack_proxy argument : args)
			{
				if (!first)
					stream << ' ';
				first = false;
				sol::state_view lua(args.lua_state());
				const sol::protected_function toString = lua["tostring"];
				const sol::protected_function_result result = toString(argument.get<sol::object>());
				stream << (result.valid() ? result.get<std::string>() : std::string("?"));
			}
			return stream.str();
		}

		KeyCode ParseKey(const std::string& name)
		{
			const auto key = KeyCodeFromString(name);
			if (!key)
				throw std::runtime_error("unknown key '" + name + "'");
			return *key;
		}

		MouseCode ParseMouseButton(const std::string& name)
		{
			const auto button = MouseCodeFromString(name);
			if (!button)
				throw std::runtime_error("unknown mouse button '" + name + "' (valid: Left, Right, Middle, Button3..Button7)");
			return *button;
		}

		GamepadButton ParseGamepadButton(const std::string& name)
		{
			const auto button = GamepadButtonFromString(name);
			if (!button)
				throw std::runtime_error(fmt::format("unknown gamepad button '{}' (valid: {})", name, fmt::join(GetGamepadButtonNames(), ", ")));
			return *button;
		}

		GamepadAxis ParseGamepadAxis(const std::string& name)
		{
			const auto axis = GamepadAxisFromString(name);
			if (!axis)
				throw std::runtime_error(fmt::format("unknown gamepad axis '{}' (valid: {})", name, fmt::join(GetGamepadAxisNames(), ", ")));
			return *axis;
		}

		// A gamepad index argument: an integer from 0 to Input::MaxGamepads - 1.
		uint32_t ParseGamepadIndex(const sol::object& value)
		{
			const std::optional<int64_t> index = ToInteger(value);
			if (!index || *index < 0 || *index >= static_cast<int64_t>(Input::MaxGamepads))
				throw std::runtime_error("the gamepad index must be an integer from 0 to " + std::to_string(Input::MaxGamepads - 1));
			return static_cast<uint32_t>(*index);
		}

		const ComponentInfo& RequireComponent(const std::string& name)
		{
			const ComponentInfo* info = ComponentRegistry::Find(name);
			if (!info)
				throw std::runtime_error("unknown component '" + name + "' (valid: " + ComponentRegistry::GetNameList() + ")");
			return *info;
		}

		PhysicsWorld& RequirePhysics(ScriptContext& context)
		{
			PhysicsWorld* physics = RequireScene(context).GetPhysicsWorld();
			if (!physics)
				throw std::runtime_error("physics is not running");
			return *physics;
		}

		struct QueryOptions
		{
			PhysicsQueryFilter Filter;
			bool All = false;
		};

		// The optional last argument of every Physics query: an Entity to ignore (Raycast's original
		// argument) or a table { Ignore = entity, Layers = { "Default", ... }, IncludeTriggers = bool,
		// All = bool }. Strict, so a misspelled option, an option the query does not take (All on an
		// overlap) or an empty Layers list is an error rather than silently ignored.
		QueryOptions ParseQueryOptions(const PhysicsWorld& physics, const sol::object& options, bool allowAll)
		{
			QueryOptions result;
			if (IsNil(options))
				return result;
			if (options.is<ScriptEntity>())
			{
				result.Filter.IgnoreEntity = options.as<ScriptEntity>().ID;
				return result;
			}
			if (options.get_type() != sol::type::table)
				throw std::runtime_error("query options must be an Entity or a table");

			constexpr const char* LayersError = "query option 'Layers' must be a non-empty table of layer names";
			for (const auto& [key, value] : options.as<sol::table>())
			{
				if (key.get_type() != sol::type::string)
					throw std::runtime_error("query options must use named keys (Ignore, Layers, IncludeTriggers, All)");
				const std::string name = key.as<std::string>();
				if (name == "Ignore")
				{
					if (!value.is<ScriptEntity>())
						throw std::runtime_error("query option 'Ignore' must be an Entity");
					result.Filter.IgnoreEntity = value.as<ScriptEntity>().ID;
				}
				else if (name == "Layers")
				{
					if (value.get_type() != sol::type::table)
						throw std::runtime_error(LayersError);
					std::vector<std::string> names;
					for (const auto& [index, layer] : value.as<sol::table>())
					{
						if (layer.get_type() != sol::type::string)
							throw std::runtime_error(LayersError);
						names.push_back(layer.as<std::string>());
					}
					// An empty list would match nothing, which is never what a script means.
					if (names.empty())
						throw std::runtime_error(LayersError);
					std::string error;
					const auto mask = physics.GetLayers().MaskFromNames(names, error);
					if (!mask)
						throw std::runtime_error(error);
					result.Filter.LayerMask = *mask;
				}
				else if (name == "IncludeTriggers")
				{
					if (value.get_type() != sol::type::boolean)
						throw std::runtime_error("query option 'IncludeTriggers' must be a boolean");
					result.Filter.IncludeTriggers = value.as<bool>();
				}
				else if (name == "All" && allowAll)
				{
					if (value.get_type() != sol::type::boolean)
						throw std::runtime_error("query option 'All' must be a boolean");
					result.All = value.as<bool>();
				}
				else if (name == "All")
				{
					throw std::runtime_error("query option 'All' only applies to casts; overlaps always return every entity");
				}
				else
				{
					throw std::runtime_error("unknown query option '" + name + "' (valid: Ignore, Layers, IncludeTriggers, All)");
				}
			}
			return result;
		}

		// Lua queries reject arguments the C++ API would quietly treat as "nothing hit", since a script
		// with a bad vector would otherwise just see empty results; audio positions too, for a clearer error.
		void RequireFiniteVector(const char* query, const char* argument, const glm::vec3& v)
		{
			if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
				throw std::runtime_error(std::string(query) + ": " + argument + " must be finite");
		}

		void RequirePositive(const char* query, const char* argument, float value)
		{
			if (!std::isfinite(value) || value <= 0.0f)
				throw std::runtime_error(std::string(query) + ": " + argument + " must be a positive number");
		}

		void RequireCast(const char* query, const glm::vec3& origin, const glm::vec3& direction, float maxDistance)
		{
			RequireFiniteVector(query, "origin", origin);
			RequireFiniteVector(query, "direction", direction);
			if (glm::length(direction) <= 0.0f)
				throw std::runtime_error(std::string(query) + ": direction must not be zero");
			RequirePositive(query, "maxDistance", maxDistance);
		}

		void RequireBox(const char* query, const glm::vec3& halfExtents, const glm::quat& rotation)
		{
			RequireFiniteVector(query, "halfExtents", halfExtents);
			if (halfExtents.x <= 0.0f || halfExtents.y <= 0.0f || halfExtents.z <= 0.0f)
				throw std::runtime_error(std::string(query) + ": halfExtents must be positive");
			const float length = glm::length(rotation);
			if (!std::isfinite(length) || length <= 1e-6f)
				throw std::runtime_error(std::string(query) + ": rotation must be a non-zero quaternion");
		}

		// A hit as a Lua table, or nil when its entity no longer exists (hits and overlaps treat a missing
		// entity the same way: it is left out).
		sol::object MakeHit(sol::state_view lua, ScriptContext& context, const RaycastHit& hit)
		{
			Entity entity = RequireScene(context).GetEntityByUUID(hit.EntityID);
			if (!entity)
				return sol::lua_nil;
			sol::table result = lua.create_table();
			result["Entity"] = MakeEntity(lua, context, entity);
			result["Point"] = hit.Point;
			result["Normal"] = hit.Normal;
			result["Distance"] = hit.Distance;
			return result;
		}

		// A cast's result: the closest hit (or nil), or an array of every hit when options.All is set.
		template<typename CastAll, typename CastClosest>
		sol::object CastResult(sol::state_view lua, ScriptContext& context, const QueryOptions& options, CastAll&& castAll, CastClosest&& castClosest)
		{
			if (options.All)
			{
				sol::table hits = lua.create_table();
				for (const RaycastHit& hit : castAll(options.Filter))
				{
					if (sol::object table = MakeHit(lua, context, hit); table.valid() && table.get_type() != sol::type::lua_nil)
						hits.add(table);
				}
				return hits;
			}
			const std::optional<RaycastHit> hit = castClosest(options.Filter);
			if (!hit)
				return sol::lua_nil;
			return MakeHit(lua, context, *hit);
		}

		sol::table MakeEntityList(sol::state_view lua, ScriptContext& context, const std::vector<UUID>& entities)
		{
			sol::table result = lua.create_table();
			for (UUID id : entities)
			{
				if (Entity entity = RequireScene(context).GetEntityByUUID(id))
					result.add(MakeEntity(lua, context, entity));
			}
			return result;
		}

		AudioSystem& RequireAudio(ScriptContext& context)
		{
			AudioSystem* audio = RequireScene(context).GetAudioSystem();
			if (!audio)
				throw std::runtime_error("audio is not running");
			return *audio;
		}

		AudioBus ParseAudioBus(const char* function, const std::string& name)
		{
			if (const std::optional<AudioBus> bus = EnumFromName<AudioBus>(AudioBusNames, name))
				return *bus;
			throw std::runtime_error(fmt::format("{}: unknown audio bus '{}' (valid: {})", function, name, fmt::join(AudioBusNames, ", ")));
		}

		// A linear gain (see IsValidAudioVolume): checked here so a script gets an error at the call.
		float RequireAudioVolume(const char* function, float volume)
		{
			if (!IsValidAudioVolume(volume))
				throw std::runtime_error(fmt::format("{}: volume must be a finite number >= 0 (got {})", function, volume));
			return volume;
		}

		// Audio.SetReverb's table patched onto the current settings: only RoomSize, Damping and Width, each a number
		// from 0 to 1 (checked here so a script gets an error at the call).
		AudioReverbSettings ReadReverbSettings(const sol::table& table, AudioReverbSettings settings)
		{
			const auto names = AudioReverbFields | std::views::transform(&AudioReverbField::Name);
			for (const auto& [key, value] : table)
			{
				if (key.get_type() != sol::type::string)
					throw std::runtime_error(fmt::format("Audio.SetReverb: settings must use named keys ({}), not a {} key", fmt::join(names, ", "), sol::type_name(key.lua_state(), key.get_type())));
				const std::string name = key.as<std::string>();
				const auto field = std::ranges::find(AudioReverbFields, std::string_view(name), [](const AudioReverbField& f) { return std::string_view(f.Name); });
				if (field == AudioReverbFields.end())
					throw std::runtime_error(fmt::format("Audio.SetReverb: unknown setting '{}' (valid: {})", name, fmt::join(names, ", ")));
				if (value.get_type() != sol::type::number)
					throw std::runtime_error(fmt::format("Audio.SetReverb: {} must be a number", name));
				// Checked as a double: converting one outside float's range (1e300) would be undefined.
				const auto number = value.as<double>();
				if (!(number >= 0.0 && number <= 1.0))
					throw std::runtime_error(fmt::format("Audio.SetReverb: {} must be a number from 0 to 1 (got {})", name, number));
				settings.*field->Member = static_cast<float>(number);
			}
			if (const std::string problem = ValidateAudioReverb(settings); !problem.empty())
				throw std::runtime_error("Audio.SetReverb: " + problem);
			return settings;
		}

		ParticleSystem& RequireParticles(ScriptContext& context)
		{
			ParticleSystem* particles = RequireScene(context).GetParticleSystem();
			if (!particles)
				throw std::runtime_error("particles are not running");
			return *particles;
		}

		// The entity, when it has component T: silently ignoring a call that needs T (Move, PlayParticles) on
		// an entity without it would hide the missing component.
		template<typename T>
		Entity RequireEntityWith(const ScriptEntity& self, const char* function, const char* component)
		{
			Entity entity = self.ResolveOrThrow();
			if (!entity.HasComponent<T>())
				throw std::runtime_error(std::string(function) + " needs the " + component + " component on '" + entity.GetName() + "'");
			return entity;
		}

		// An optional numeric argument: nil gives `fallback`; anything but a number is an error (a sol::optional would
		// silently treat a mistyped argument as missing).
		float OptionalNumber(const sol::object& value, const char* what, float fallback)
		{
			if (IsNil(value))
				return fallback;
			if (value.get_type() != sol::type::number)
				throw std::runtime_error(std::string(what) + " must be a number or nil");
			return value.as<float>();
		}

		Entity RequireAnimator(const ScriptEntity& self, const char* operation)
		{
			return RequireEntityWith<AnimatorComponent>(self, operation, "Animator");
		}

		AnimationSystem& RequireAnimations(ScriptContext& context)
		{
			auto* system = RequireScene(context).GetAnimationSystem();
			if (!system)
				throw std::runtime_error("animation requires a running scene");
			return *system;
		}

		// The entity, with a ParticleEmitter the particle system knows. Every emitter registers when its
		// component is added, so a missing registration is an engine bug, reported rather than ignored.
		Entity RequireEmitter(ScriptContext& context, const ScriptEntity& self, const char* function)
		{
			Entity entity = RequireEntityWith<ParticleEmitterComponent>(self, function, "ParticleEmitter");
			if (!RequireParticles(context).HasEmitter(entity))
				throw std::runtime_error("the ParticleEmitter on '" + entity.GetName() + "' is not registered with the particle system");
			return entity;
		}

		// A seed argument: a Lua integer or a float with an integer value (stock Lua also converts numeric
		// strings; seeds here must be numbers). Seeds are 32-bit, so other integers (negative, or 2^32 and
		// up) use their low 32 bits.
		uint32_t ToSeed(const sol::object& value, const char* function)
		{
			if (value.get_type() != sol::type::number)
				throw std::runtime_error(std::string("bad argument #1 to '") + function + "' (number expected, got " + sol::type_name(value.lua_state(), value.get_type()) + ")");
			const std::optional<int64_t> integer = ToInteger(value);
			if (!integer)
				throw std::runtime_error(std::string("bad argument #1 to '") + function + "' (number has no integer representation)");
			return static_cast<uint32_t>(static_cast<uint64_t>(*integer));
		}

		void RegisterMath(sol::state& lua, ScriptContext& context)
		{
			lua.new_usertype<glm::vec2>("Vec2", sol::call_constructor, sol::constructors<glm::vec2(), glm::vec2(float), glm::vec2(float, float)>(), "x", sol::property([](const glm::vec2& v) { return v.x; }, [](glm::vec2& v, float value) { v.x = value; }), "y", sol::property([](const glm::vec2& v) { return v.y; }, [](glm::vec2& v, float value) { v.y = value; }), "Length", [](const glm::vec2& v) { return glm::length(v); }, "Normalized", [](const glm::vec2& v) { return glm::length(v) > 0.0f ? glm::normalize(v) : v; }, "Dot", [](const glm::vec2& a, const glm::vec2& b) { return glm::dot(a, b); }, sol::meta_function::addition, [](const glm::vec2& a, const glm::vec2& b) { return a + b; }, sol::meta_function::subtraction, [](const glm::vec2& a, const glm::vec2& b) { return a - b; }, sol::meta_function::multiplication, sol::overload([](const glm::vec2& a, const glm::vec2& b) { return a * b; }, [](const glm::vec2& a, float s) { return a * s; }, [](float s, const glm::vec2& a) { return a * s; }), sol::meta_function::division, sol::overload([](const glm::vec2& a, const glm::vec2& b) { return a / b; }, [](const glm::vec2& a, float s) { return a / s; }), sol::meta_function::unary_minus, [](const glm::vec2& a) { return -a; }, sol::meta_function::equal_to, [](const glm::vec2& a, const glm::vec2& b) { return a == b; }, sol::meta_function::to_string, [](const glm::vec2& v) { return "Vec2(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ")"; });

			lua.new_usertype<glm::vec3>("Vec3", sol::call_constructor, sol::constructors<glm::vec3(), glm::vec3(float), glm::vec3(float, float, float)>(), "x", sol::property([](const glm::vec3& v) { return v.x; }, [](glm::vec3& v, float value) { v.x = value; }), "y", sol::property([](const glm::vec3& v) { return v.y; }, [](glm::vec3& v, float value) { v.y = value; }), "z", sol::property([](const glm::vec3& v) { return v.z; }, [](glm::vec3& v, float value) { v.z = value; }), "Length", [](const glm::vec3& v) { return glm::length(v); }, "Normalized", [](const glm::vec3& v) { return glm::length(v) > 0.0f ? glm::normalize(v) : v; }, "Dot", [](const glm::vec3& a, const glm::vec3& b) { return glm::dot(a, b); }, "Cross", [](const glm::vec3& a, const glm::vec3& b) { return glm::cross(a, b); }, "Distance", [](const glm::vec3& a, const glm::vec3& b) { return glm::distance(a, b); }, "Lerp", [](const glm::vec3& a, const glm::vec3& b, float t) { return glm::mix(a, b, t); }, sol::meta_function::addition, [](const glm::vec3& a, const glm::vec3& b) { return a + b; }, sol::meta_function::subtraction, [](const glm::vec3& a, const glm::vec3& b) { return a - b; }, sol::meta_function::multiplication, sol::overload([](const glm::vec3& a, const glm::vec3& b) { return a * b; }, [](const glm::vec3& a, float s) { return a * s; }, [](float s, const glm::vec3& a) { return a * s; }), sol::meta_function::division, sol::overload([](const glm::vec3& a, const glm::vec3& b) { return a / b; }, [](const glm::vec3& a, float s) { return a / s; }), sol::meta_function::unary_minus, [](const glm::vec3& a) { return -a; }, sol::meta_function::equal_to, [](const glm::vec3& a, const glm::vec3& b) { return a == b; }, sol::meta_function::to_string, [](const glm::vec3& v) { return "Vec3(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + ")"; });

			lua.new_usertype<glm::vec4>("Vec4", sol::call_constructor, sol::constructors<glm::vec4(), glm::vec4(float), glm::vec4(float, float, float, float)>(), "x", sol::property([](const glm::vec4& v) { return v.x; }, [](glm::vec4& v, float value) { v.x = value; }), "y", sol::property([](const glm::vec4& v) { return v.y; }, [](glm::vec4& v, float value) { v.y = value; }), "z", sol::property([](const glm::vec4& v) { return v.z; }, [](glm::vec4& v, float value) { v.z = value; }), "w", sol::property([](const glm::vec4& v) { return v.w; }, [](glm::vec4& v, float value) { v.w = value; }), sol::meta_function::addition, [](const glm::vec4& a, const glm::vec4& b) { return a + b; }, sol::meta_function::subtraction, [](const glm::vec4& a, const glm::vec4& b) { return a - b; }, sol::meta_function::multiplication, sol::overload([](const glm::vec4& a, const glm::vec4& b) { return a * b; }, [](const glm::vec4& a, float s) { return a * s; }, [](float s, const glm::vec4& a) { return a * s; }), sol::meta_function::equal_to, [](const glm::vec4& a, const glm::vec4& b) { return a == b; }, sol::meta_function::to_string, [](const glm::vec4& v) { return "Vec4(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + ", " + std::to_string(v.w) + ")"; });

			lua.new_usertype<glm::quat>("Quat", sol::call_constructor, sol::factories([]() { return glm::quat(1.0f, 0.0f, 0.0f, 0.0f); }, [](float w, float x, float y, float z) { return glm::quat(w, x, y, z); }), "w", sol::property([](const glm::quat& q) { return q.w; }, [](glm::quat& q, float value) { q.w = value; }), "x", sol::property([](const glm::quat& q) { return q.x; }, [](glm::quat& q, float value) { q.x = value; }), "y", sol::property([](const glm::quat& q) { return q.y; }, [](glm::quat& q, float value) { q.y = value; }), "z", sol::property([](const glm::quat& q) { return q.z; }, [](glm::quat& q, float value) { q.z = value; }), "Identity", []() { return glm::quat(1.0f, 0.0f, 0.0f, 0.0f); }, "FromEuler", [](const glm::vec3& euler) { return Math::QuatFromEuler(euler); }, "AngleAxis", [](float angle, const glm::vec3& axis) { return glm::angleAxis(angle, glm::normalize(axis)); }, "LookRotation", [](const glm::vec3& forward, sol::optional<glm::vec3> up) { return glm::quatLookAt(glm::normalize(forward), up.value_or(glm::vec3(0.0f, 1.0f, 0.0f))); }, "ToEuler", [](const glm::quat& q) { return Math::EulerFromQuat(q); }, "Normalized", [](const glm::quat& q) { return glm::normalize(q); }, "Inverse", [](const glm::quat& q) { return glm::inverse(q); }, "Slerp", [](const glm::quat& a, const glm::quat& b, float t) { return glm::slerp(a, b, t); }, sol::meta_function::multiplication, sol::overload([](const glm::quat& a, const glm::quat& b) { return a * b; }, [](const glm::quat& q, const glm::vec3& v) { return q * v; }), sol::meta_function::equal_to, [](const glm::quat& a, const glm::quat& b) { return a == b; }, sol::meta_function::to_string, [](const glm::quat& q) { return "Quat(" + std::to_string(q.w) + ", " + std::to_string(q.x) + ", " + std::to_string(q.y) + ", " + std::to_string(q.z) + ")"; });

			sol::table math = lua.create_named_table("Math");
			math["Pi"] = glm::pi<float>();
			math["Radians"] = [](float degrees) { return glm::radians(degrees); };
			math["Degrees"] = [](float radians) { return glm::degrees(radians); };
			math["Clamp"] = [](float value, float min, float max) { return glm::clamp(value, min, max); };
			math["Lerp"] = [](float a, float b, float t) { return a + (b - a) * t; };
			math["Sign"] = [](float value) { return value > 0.0f ? 1.0f : (value < 0.0f ? -1.0f : 0.0f); };
			// Deterministic generator, seeded from the session's seed (the project's RandomSeed); Math.Seed
			// picks another sequence.
			math["Seed"] = [&context](const sol::object& seed) { context.Rng.Seed(ToSeed(seed, "Seed")); };
			math["Random"] = sol::overload([&context]() { return context.Rng.Float(); }, [&context](float min, float max) { return context.Rng.Range(min, max); });
			math["RandomInt"] = [&context](int64_t min, int64_t max) { return context.Rng.Int(min, max); };

			// Lua's own math.random is seeded from the clock at startup, which would make any script that
			// uses it nondeterministic; it shares the engine generator instead (same results as Lua's API
			// shape: random(), random(m) in [1, m], random(m, n) in [m, n]).
			sol::table luaMath = lua["math"];
			luaMath["random"] = sol::overload(
				[&context]() { return static_cast<double>(context.Rng.Float()); },
				[&context](int64_t max) {
					if (max == 0)
						return context.Rng.Int(std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max());
					if (max < 1)
						throw std::runtime_error("bad argument #1 to 'random' (interval is empty)");
					return context.Rng.Int(1, max);
				},
				[&context](int64_t min, int64_t max) {
					if (min > max)
						throw std::runtime_error("bad argument #2 to 'random' (interval is empty)");
					return context.Rng.Int(min, max);
				});
			// Without an argument (or with nil) it returns to the session's starting point, the seed play started
			// with; see ToSeed for the values it takes.
			luaMath["randomseed"] = [&context](const sol::object& seed) { context.Rng.Seed(seed.valid() ? ToSeed(seed, "randomseed") : context.Seed); };
		}

		void RegisterEntity(sol::state& lua, ScriptContext& context)
		{
			auto worldRotation = [&context](const ScriptEntity& self) {
				Entity entity = self.ResolveOrThrow();
				glm::vec3 position;
				glm::quat rotation;
				glm::vec3 scale;
				Math::DecomposeTransform(RequireScene(context).GetWorldTransform(entity), position, rotation, scale);
				return rotation;
			};

			sol::usertype<ScriptEntity> entityType = lua.new_usertype<ScriptEntity>("Entity", sol::no_constructor, "ID", sol::readonly_property([](const ScriptEntity& self) { return static_cast<int64_t>(static_cast<uint64_t>(self.ID)); }), "Name", sol::property([](const ScriptEntity& self) { return self.ResolveOrThrow().GetName(); }, [](const ScriptEntity& self, const std::string& name) { self.ResolveOrThrow().GetComponent<TagComponent>().Tag = name; }), "IsValid", [](const ScriptEntity& self) { return static_cast<bool>(self.Resolve()); },

																					"Translation", sol::property([](const ScriptEntity& self) { return self.ResolveOrThrow().GetTransform().Translation; }, [](const ScriptEntity& self, const glm::vec3& value) { self.ResolveOrThrow().GetTransform().Translation = value; }), "Rotation", sol::property([](const ScriptEntity& self) { return self.ResolveOrThrow().GetTransform().Rotation; }, [](const ScriptEntity& self, const glm::quat& value) { self.ResolveOrThrow().GetTransform().Rotation = glm::normalize(value); }), "EulerAngles", sol::property([](const ScriptEntity& self) { return self.ResolveOrThrow().GetTransform().GetRotationEuler(); }, [](const ScriptEntity& self, const glm::vec3& value) { self.ResolveOrThrow().GetTransform().SetRotationEuler(value); }), "Scale", sol::property([](const ScriptEntity& self) { return self.ResolveOrThrow().GetTransform().Scale; }, [](const ScriptEntity& self, const glm::vec3& value) { self.ResolveOrThrow().GetTransform().Scale = value; }), "WorldPosition", sol::property([&context](const ScriptEntity& self) { return glm::vec3(RequireScene(context).GetWorldTransform(self.ResolveOrThrow())[3]); }, [&context](const ScriptEntity& self, const glm::vec3& value) {
					Entity entity = self.ResolveOrThrow();
					Scene& scene = RequireScene(context);
					glm::mat4 world = scene.GetWorldTransform(entity);
					world[3] = glm::vec4(value, 1.0f);
					scene.SetWorldTransform(entity, world); }), "GetForward", [worldRotation](const ScriptEntity& self) { return worldRotation(self) * glm::vec3(0.0f, 0.0f, -1.0f); }, "GetRight", [worldRotation](const ScriptEntity& self) { return worldRotation(self) * glm::vec3(1.0f, 0.0f, 0.0f); }, "GetUp", [worldRotation](const ScriptEntity& self) { return worldRotation(self) * glm::vec3(0.0f, 1.0f, 0.0f); }, "LookAt", [&context](const ScriptEntity& self, const glm::vec3& target, sol::optional<glm::vec3> up) {
					Entity entity = self.ResolveOrThrow();
					Scene& scene = RequireScene(context);
					glm::vec3 position;
					glm::quat rotation;
					glm::vec3 scale;
					Math::DecomposeTransform(scene.GetWorldTransform(entity), position, rotation, scale);
					const glm::vec3 direction = target - position;
					if (glm::length(direction) < 1e-6f)
						return;
					rotation = glm::quatLookAt(glm::normalize(direction), up.value_or(glm::vec3(0.0f, 1.0f, 0.0f)));
					scene.SetWorldTransform(entity, Math::ComposeTransform(position, rotation, scale)); },

																					"HasComponent", [](const ScriptEntity& self, const std::string& name) { return RequireComponent(name).Has(self.ResolveOrThrow()); }, "AddComponent", [&context](const ScriptEntity& self, const std::string& name, sol::optional<sol::table> data) {
					Entity entity = self.ResolveOrThrow();
					const ComponentInfo& info = RequireComponent(name);
					if (info.IsCore)
						throw std::runtime_error("component '" + name + "' always exists and cannot be added");
					std::string error;
					const nlohmann::json json = data ? LuaToJson(*data) : nlohmann::json::object();
					if (!ComponentRegistry::AddOrPatch(entity, name, json, error))
						throw std::runtime_error(error);
					if (name == "Script" && context.Host)
						context.Host->EnsureInstances({ entity }); }, "RemoveComponent", [](const ScriptEntity& self, const std::string& name) {
					const ComponentInfo& info = RequireComponent(name);
					if (info.IsCore)
						throw std::runtime_error("component '" + name + "' cannot be removed");
					info.Remove(self.ResolveOrThrow()); }, "GetComponent", [](sol::this_state state, const ScriptEntity& self, const std::string& name) -> sol::object {
					Entity entity = self.ResolveOrThrow();
					const ComponentInfo& info = RequireComponent(name);
					if (!info.Has(entity))
						return sol::lua_nil;
					return JsonToLua(state, info.Serialize(entity)); }, "SetComponent", [](const ScriptEntity& self, const std::string& name, const sol::table& data) {
					Entity entity = self.ResolveOrThrow();
					const ComponentInfo& info = RequireComponent(name);
					if (!info.Has(entity))
						throw std::runtime_error("entity '" + entity.GetName() + "' has no " + name + " component");
					std::string error;
					if (!info.Deserialize(entity, LuaToJson(data), error))
						throw std::runtime_error(name + ": " + error); },

																					"GetParent", [&context](sol::this_state state, const ScriptEntity& self) { return MakeEntity(state, context, self.ResolveOrThrow().GetParent()); }, "SetParent", [&context](const ScriptEntity& self, sol::optional<ScriptEntity> parent) {
					Entity entity = self.ResolveOrThrow();
					Entity parentEntity = parent ? parent->ResolveOrThrow() : Entity {};
					if (!RequireScene(context).SetParent(entity, parentEntity, true))
						throw std::runtime_error("cannot parent an entity to itself or its descendant"); }, "GetChildren", [&context](sol::this_state state, const ScriptEntity& self) {
					sol::state_view view(state);
					sol::table children = view.create_table();
					int index = 1;
					for (Entity child : self.ResolveOrThrow().GetChildren())
						children[index++] = ScriptEntity { child.GetUUID(), context.SceneContext };
					return children; }, "FindChild", [&context](sol::this_state state, const ScriptEntity& self, const std::string& name) -> sol::object {
					std::function<Entity(Entity)> find = [&](Entity parent) -> Entity {
						for (Entity child : parent.GetChildren())
						{
							if (child.GetName() == name)
								return child;
							if (Entity found = find(child))
								return found;
						}
						return {};
					};
					return MakeEntity(state, context, find(self.ResolveOrThrow())); },

																					"AddForce", [&context](const ScriptEntity& self, const glm::vec3& force) { RequirePhysics(context).AddForce(self.ResolveOrThrow(), force); }, "AddImpulse", [&context](const ScriptEntity& self, const glm::vec3& impulse) { RequirePhysics(context).AddImpulse(self.ResolveOrThrow(), impulse); }, "AddTorque", [&context](const ScriptEntity& self, const glm::vec3& torque) { RequirePhysics(context).AddTorque(self.ResolveOrThrow(), torque); }, "SetLinearVelocity", [&context](const ScriptEntity& self, const glm::vec3& velocity) { RequirePhysics(context).SetLinearVelocity(self.ResolveOrThrow(), velocity); }, "GetLinearVelocity", [&context](const ScriptEntity& self) { return RequirePhysics(context).GetLinearVelocity(self.ResolveOrThrow()); }, "SetAngularVelocity", [&context](const ScriptEntity& self, const glm::vec3& velocity) { RequirePhysics(context).SetAngularVelocity(self.ResolveOrThrow(), velocity); }, "GetAngularVelocity", [&context](const ScriptEntity& self) { return RequirePhysics(context).GetAngularVelocity(self.ResolveOrThrow()); }, "IsSleeping", [&context](const ScriptEntity& self) -> sol::optional<bool> { PhysicsWorld& physics = RequirePhysics(context); const Entity entity = self.ResolveOrThrow(); if (!physics.HasBody(entity)) return sol::nullopt; return physics.IsBodySleeping(entity); }, "GetJointPosition", [&context](const ScriptEntity& self) {
				const std::optional<float> position = RequirePhysics(context).GetJointPosition(self.ResolveOrThrow());
				return position ? sol::optional<float>(*position) : sol::optional<float>(); }, "GetJointRotation", [&context](const ScriptEntity& self) {
				const std::optional<glm::vec3> rotation = RequirePhysics(context).GetJointRotation(self.ResolveOrThrow());
				return rotation ? sol::optional<glm::vec3>(*rotation) : sol::optional<glm::vec3>(); }, "HasJoint", [&context](const ScriptEntity& self) { return RequirePhysics(context).HasJoint(self.ResolveOrThrow()); },

																					"Move", [&context](const ScriptEntity& self, const glm::vec3& velocity) {
					Entity entity = RequireEntityWith<CharacterControllerComponent>(self, "Move", "CharacterController");
					// Ignoring it would keep the character walking at its last velocity (e.g. after normalizing a zero vector).
					if (!std::isfinite(velocity.x) || !std::isfinite(velocity.y) || !std::isfinite(velocity.z))
						throw std::runtime_error("Move velocity must be finite");
					RequirePhysics(context).MoveCharacter(entity, velocity); }, "IsGrounded", [&context](const ScriptEntity& self) { return RequirePhysics(context).IsCharacterGrounded(self.ResolveOrThrow()); }, "GetGroundNormal", [&context](const ScriptEntity& self) {
					const std::optional<glm::vec3> normal = RequirePhysics(context).GetCharacterGroundNormal(self.ResolveOrThrow());
					return normal ? sol::optional<glm::vec3>(*normal) : sol::optional<glm::vec3>(); },

																					"PlayAudio", [&context](const ScriptEntity& self) { return RequireAudio(context).Play(RequireEntityWith<AudioSourceComponent>(self, "PlayAudio", "AudioSource")); }, "StopAudio", [&context](const ScriptEntity& self) { RequireAudio(context).Stop(RequireEntityWith<AudioSourceComponent>(self, "StopAudio", "AudioSource")); }, "IsAudioPlaying", [&context](const ScriptEntity& self) { return RequireAudio(context).IsPlaying(self.ResolveOrThrow()); }, "GetAudioDopplerPitch", [&context](const ScriptEntity& self) { return RequireAudio(context).GetDopplerPitch(self.ResolveOrThrow()); },

																					"PlayAnimation", [&context](const ScriptEntity& self, const sol::object& clip) {
					if (clip.valid() && clip.get_type() != sol::type::string)
						throw std::runtime_error("PlayAnimation: clip must be a string or nil");
					return RequireAnimations(context).Play(RequireAnimator(self, "PlayAnimation"), clip.valid() ? clip.as<std::string>() : ""); }, "CrossFadeAnimation", [&context](const ScriptEntity& self, const std::string& clip, float seconds) { return RequireAnimations(context).CrossFade(RequireAnimator(self, "CrossFadeAnimation"), clip, seconds); }, "BlendAnimations", [&context](const ScriptEntity& self, const std::string& first, const std::string& second, float weight) { return RequireAnimations(context).Blend(RequireAnimator(self, "BlendAnimations"), first, second, weight); }, "PauseAnimation", [&context](const ScriptEntity& self, const sol::object& paused) {
					if (paused.valid() && paused.get_type() != sol::type::boolean)
						throw std::runtime_error("PauseAnimation: paused must be true, false or nil");
					RequireAnimations(context).Pause(RequireAnimator(self, "PauseAnimation"), !paused.valid() || paused.as<bool>()); }, "StopAnimation", [&context](const ScriptEntity& self) { RequireAnimations(context).Stop(RequireAnimator(self, "StopAnimation")); }, "SetAnimationTime", [&context](const ScriptEntity& self, float seconds) { return RequireAnimations(context).Seek(RequireAnimator(self, "SetAnimationTime"), seconds); }, "GetAnimationTime", [&context](const ScriptEntity& self) { return RequireAnimations(context).GetTime(RequireAnimator(self, "GetAnimationTime")); }, "GetAnimationDuration", [&context](const ScriptEntity& self) { return RequireAnimations(context).GetDuration(RequireAnimator(self, "GetAnimationDuration")); }, "IsAnimationPlaying", [&context](const ScriptEntity& self) { return RequireAnimations(context).IsPlaying(RequireAnimator(self, "IsAnimationPlaying")); }, "GetAnimationClips", [&context](const ScriptEntity& self) { return sol::as_table(RequireAnimations(context).GetClips(RequireAnimator(self, "GetAnimationClips"))); }, "PlayParticles", [&context](const ScriptEntity& self) { RequireParticles(context).Play(RequireEmitter(context, self, "PlayParticles")); }, "StopParticles", [&context](const ScriptEntity& self, const sol::object& clear) {
					Entity entity = RequireEmitter(context, self, "StopParticles");
					// Only booleans: through sol::optional<bool>, a truthy 1 or "yes" would silently have meant false.
					if (clear.valid() && clear.get_type() != sol::type::boolean)
						throw std::runtime_error("StopParticles: clear must be true, false or nil");
					RequireParticles(context).Stop(entity, clear.valid() && clear.as<bool>()); }, "EmitParticles", [&context](const ScriptEntity& self, int64_t count) {
					Entity entity = RequireEmitter(context, self, "EmitParticles");
					if (count < 0 || count > static_cast<int64_t>(ParticleEmitterComponent::MaxParticlesLimit))
						throw std::runtime_error("EmitParticles count must be between 0 and " + std::to_string(ParticleEmitterComponent::MaxParticlesLimit));
					return RequireParticles(context).Emit(entity, static_cast<uint32_t>(count)); }, "IsEmittingParticles", [&context](const ScriptEntity& self) { return RequireParticles(context).IsPlaying(self.ResolveOrThrow()); }, "GetParticleCount", [&context](const ScriptEntity& self) { return RequireParticles(context).GetParticleCount(self.ResolveOrThrow()); },

																					"GetScript", [&context](const ScriptEntity& self) -> sol::object {
					self.ResolveOrThrow();
					return context.Host ? context.Host->GetInstance(self.ID) : sol::object(sol::lua_nil); }, "Destroy", [&context](const ScriptEntity& self) {
					if (Entity entity = self.Resolve())
						RequireScene(context).DestroyEntity(entity); },

																					sol::meta_function::equal_to, [](const ScriptEntity& a, const ScriptEntity& b) { return a == b; }, sol::meta_function::to_string, [](const ScriptEntity& self) {
					Entity entity = self.Resolve();
					return "Entity(" + (entity ? entity.GetName() : std::string("<destroyed>")) + ", " + std::to_string(static_cast<uint64_t>(self.ID)) + ")"; });

			// Bones and blend spaces.
			entityType["GetBoneTransform"] = [&context](sol::this_state state, const ScriptEntity& self, const std::string& joint) -> std::tuple<sol::object, sol::object, sol::object> {
				Entity entity = RequireAnimator(self, "GetBoneTransform");
				AnimationSystem& animations = RequireAnimations(context);
				glm::mat4 bone(1.0f);
				std::string error;
				if (!animations.GetBoneTransform(entity, joint, bone, error))
				{
					// Logged once: scripts typically poll a joint every frame.
					animations.ReportBoneProblem(entity, joint, error);
					return { sol::lua_nil, sol::lua_nil, sol::lua_nil };
				}
				glm::vec3 position;
				glm::quat rotation;
				glm::vec3 scale;
				Math::DecomposeTransform(RequireScene(context).GetWorldTransform(entity) * bone, position, rotation, scale);
				const sol::state_view view(state);
				return { sol::make_object(view, position), sol::make_object(view, rotation), sol::make_object(view, scale) };
			};
			entityType["PlayBlendSpace"] = [&context](const ScriptEntity& self, const std::string& space, float x, const sol::object& y, const sol::object& fadeSeconds) {
				Entity entity = RequireAnimator(self, "PlayBlendSpace");
				return RequireAnimations(context).PlayBlendSpace(entity, space, { x, OptionalNumber(y, "PlayBlendSpace: y", 0.0f) }, OptionalNumber(fadeSeconds, "PlayBlendSpace: fadeSeconds", 0.0f));
			};
			entityType["SetBlendSpaceParameter"] = [&context](const ScriptEntity& self, float x, const sol::object& y) {
				Entity entity = RequireAnimator(self, "SetBlendSpaceParameter");
				return RequireAnimations(context).SetBlendSpaceParameter(entity, { x, OptionalNumber(y, "SetBlendSpaceParameter: y", 0.0f) });
			};
			entityType["GetBlendSpaceWeights"] = [&context](sol::this_state state, const ScriptEntity& self) {
				Entity entity = RequireAnimator(self, "GetBlendSpaceWeights");
				sol::table weights = sol::state_view(state).create_table();
				for (const auto& [clip, weight] : RequireAnimations(context).GetBlendSpaceWeights(entity))
					weights[clip] = weight;
				return weights;
			};
		}

		void RegisterGraphics(sol::state& lua, ScriptContext& context)
		{
			sol::table graphics = lua.create_named_table("Graphics");
			graphics["GetSettings"] = [&context](sol::this_state state) {
				return JsonToLua(state, SerializeGraphicsSettings(RequireScene(context).GetGraphicsSettings()));
			};
			graphics["SetSettings"] = [&context](const sol::table& settings) {
				std::string error;
				if (!RequireScene(context).SetGraphicsSettings(LuaToJson(settings), error))
					throw std::runtime_error(error);
			};
		}

		void RegisterScene(sol::state& lua, ScriptContext& context)
		{
			sol::table scene = lua.create_named_table("Scene");
			scene["CreateEntity"] = [&context](sol::this_state state, const sol::optional<std::string>& name) {
				return MakeEntity(state, context, RequireScene(context).CreateEntity(name.value_or("Entity")));
			};
			scene["FindEntityByName"] = [&context](sol::this_state state, const std::string& name) {
				return MakeEntity(state, context, RequireScene(context).FindEntityByName(name));
			};
			scene["FindEntitiesByName"] = [&context](sol::this_state state, const std::string& name) {
				sol::state_view view(state);
				sol::table result = view.create_table();
				int index = 1;
				for (Entity entity : RequireScene(context).FindEntitiesByName(name))
					result[index++] = ScriptEntity{ entity.GetUUID(), context.SceneContext };
				return result;
			};
			scene["GetEntityByID"] = [&context](sol::this_state state, int64_t id) {
				return MakeEntity(state, context, RequireScene(context).GetEntityByUUID(static_cast<uint64_t>(id)));
			};
			scene["GetEntitiesWith"] = [&context](sol::this_state state, const std::string& componentName) {
				const ComponentInfo& info = RequireComponent(componentName);
				sol::state_view view(state);
				sol::table result = view.create_table();
				int index = 1;
				for (Entity entity : RequireScene(context).GetAllEntitiesOrdered())
				{
					if (info.Has(entity))
						result[index++] = ScriptEntity{ entity.GetUUID(), context.SceneContext };
				}
				return result;
			};
			scene["Instantiate"] = [&context](sol::this_state state, const std::string& prefab, sol::optional<glm::vec3> position, sol::optional<ScriptEntity> parent) -> sol::object {
				Scene& activeScene = RequireScene(context);
				Entity parentEntity = parent ? parent->ResolveOrThrow() : Entity{};
				std::string error;
				Entity root = SceneSerializer::InstantiatePrefab(activeScene, Project::ResolvePath(prefab), prefab, parentEntity, error);
				if (!root)
					throw std::runtime_error(error);
				if (position)
					root.GetTransform().Translation = *position;
				if (context.Host)
				{
					// Root first, then descendants depth-first.
					std::vector<Entity> tree;
					std::function<void(Entity)> collect = [&](Entity entity) {
						tree.push_back(entity);
						for (Entity child : entity.GetChildren())
							collect(child);
					};
					collect(root);
					context.Host->EnsureInstances(tree);
				}
				return MakeEntity(state, context, root);
			};
			scene["Destroy"] = [&context](const ScriptEntity& entity) {
				if (Entity resolved = entity.Resolve())
					RequireScene(context).DestroyEntity(resolved);
			};
			scene["GetPrimaryCamera"] = [&context](sol::this_state state) { return MakeEntity(state, context, RequireScene(context).GetPrimaryCameraEntity()); };
			scene["GetEntityCount"] = [&context]() { return RequireScene(context).GetEntityCount(); };
			// Replaces the scene at the end of the frame (see Scene::RequestLoad); errors in the file are raised now.
			scene["Load"] = [&context](const std::string& path) {
				std::string error;
				if (!RequireScene(context).RequestLoad(path, error))
					throw std::runtime_error("Scene.Load: " + error);
			};
			scene["DontDestroyOnLoad"] = [&context](const ScriptEntity& entity) {
				Entity resolved = entity.ResolveOrThrow();
				if (!RequireScene(context).SetPersistent(resolved))
					throw std::runtime_error("Scene.DontDestroyOnLoad: '" + resolved.GetName() + "' is not a root entity (a root keeps its whole tree, so mark the root)");
			};
			scene["GetName"] = [&context]() { return RequireScene(context).GetName(); };
			scene["GetPath"] = [&context]() -> sol::optional<std::string> {
				const std::string& path = RequireScene(context).GetPath();
				return path.empty() ? sol::optional<std::string>() : sol::optional<std::string>(path);
			};
		}

		// The session's action called `name`; unknown names are errors that list the defined ones.
		const InputAction& RequireAction(ScriptContext& context, const std::string& name)
		{
			const InputActionMap& actions = RequireScene(context).GetInputActions();
			if (const InputAction* action = actions.Find(name))
				return *action;
			if (actions.IsEmpty())
				throw std::runtime_error(fmt::format("unknown input action '{}' (the project defines none: add them to {} or with project.set inputActions)", name, Project::InputFileName));
			throw std::runtime_error(fmt::format("unknown input action '{}' (defined: {})", name, fmt::join(actions.GetActions() | std::views::transform(&InputAction::Name), ", ")));
		}

		void RegisterInputActions(sol::table& input, ScriptContext& context)
		{
			input["GetAction"] = [&context](const std::string& name) { return InputActionMap::GetValue(RequireAction(context, name)); };
			input["IsActionDown"] = [&context](const std::string& name) { return InputActionMap::IsDown(RequireAction(context, name)); };
			input["IsActionPressed"] = [&context](const std::string& name) { return InputActionMap::IsPressed(RequireAction(context, name)); };
			input["IsActionReleased"] = [&context](const std::string& name) { return InputActionMap::IsReleased(RequireAction(context, name)); };
			input["GetActions"] = [&context](sol::this_state state) { return JsonToLua(state, RequireScene(context).GetInputActions().ActionsToJson()); };
			input["GetBindings"] = [&context](sol::this_state state, const std::string& name) { return JsonToLua(state, InputActionMap::BindingsToJson(RequireAction(context, name).Bindings)); };
			input["SetBindings"] = [&context](const std::string& name, const sol::table& bindings) {
				RequireAction(context, name);
				const std::string where = "Input.SetBindings('" + name + "'): ";
				nlohmann::json data;
				try
				{
					data = LuaToJson(bindings);
				}
				catch (const std::exception& e)
				{
					throw std::runtime_error(where + e.what());
				}
				if (data.is_object())
				{
					if (!data.empty())
						throw std::runtime_error(where + "expected a list of bindings, such as { { Key = 'Space' } }");
					// An empty Lua table converts to an empty object: no bindings.
					data = nlohmann::json::array();
				}
				std::string error;
				auto parsed = InputActionMap::BindingsFromJson(data, error);
				if (!parsed || !RequireScene(context).SetInputBindings(name, std::move(*parsed), error))
					throw std::runtime_error(where + error);
			};
			input["ResetBindings"] = [&context](const sol::optional<std::string>& name) {
				std::string error;
				bool reset = false;
				if (name)
				{
					RequireAction(context, *name);
					reset = RequireScene(context).ResetInputBindings(*name, error);
				}
				else
					reset = RequireScene(context).ResetAllInputBindings(error);
				if (!reset)
					throw std::runtime_error("Input.ResetBindings: " + error);
			};
		}

		void RegisterSystems(sol::state& lua, ScriptContext& context)
		{
			// The game's view of the input: only what the host routes to the game (GameInput), with the mouse in game
			// coordinates (GameUI::ToGamePosition).
			sol::table input = lua.create_named_table("Input");
			input["IsKeyDown"] = [](const std::string& key) { return GameInput::IsKeyDown(ParseKey(key)); };
			input["IsKeyPressed"] = [](const std::string& key) { return GameInput::IsKeyPressed(ParseKey(key)); };
			input["IsKeyReleased"] = [](const std::string& key) { return GameInput::IsKeyReleased(ParseKey(key)); };
			input["IsMouseButtonDown"] = [](const std::string& button) { return GameInput::IsMouseButtonDown(ParseMouseButton(button)); };
			input["IsMouseButtonPressed"] = [](const std::string& button) { return GameInput::IsMouseButtonPressed(ParseMouseButton(button)); };
			input["IsMouseButtonReleased"] = [](const std::string& button) { return GameInput::IsMouseButtonReleased(ParseMouseButton(button)); };
			input["GetMousePosition"] = []() { return GameUI::ToGamePosition(Input::GetMousePosition()); };
			input["GetMouseDelta"] = []() { return GameUI::ToGameDelta(GameInput::GetMouseDelta()); };
			input["GetMouseScroll"] = []() { return GameInput::GetMouseScroll(); };
			input["GetViewSize"] = []() { return GameUI::GetResolution(); };
			input["SetCursorMode"] = [](const std::string& mode) {
				const auto parsed = CursorModeFromString(mode);
				if (!parsed)
					throw std::runtime_error("unknown cursor mode '" + mode + "' (valid: Normal, Hidden, Locked)");
				Input::SetCursorMode(*parsed);
			};
			input["GetCursorMode"] = []() { return std::string(CursorModeToString(Input::GetCursorMode())); };
			RegisterInputActions(input, context);
			input["IsGamepadConnected"] = [](const sol::object& index) { return Input::IsGamepadConnected(ParseGamepadIndex(index)); };
			input["GetGamepadName"] = [](const sol::object& index) -> sol::optional<std::string> {
				const uint32_t gamepad = ParseGamepadIndex(index);
				if (!Input::IsGamepadConnected(gamepad))
					return sol::nullopt;
				return Input::GetGamepadName(gamepad);
			};
			input["GetGamepads"] = [](sol::this_state state) {
				sol::table gamepads = sol::state_view(state).create_table();
				for (uint32_t index = 0; index < Input::MaxGamepads; index++)
				{
					if (Input::IsGamepadConnected(index))
						gamepads.add(index);
				}
				return gamepads;
			};
			input["IsGamepadButtonDown"] = [](const sol::object& index, const std::string& button) { return GameInput::IsGamepadButtonDown(ParseGamepadIndex(index), ParseGamepadButton(button)); };
			input["IsGamepadButtonPressed"] = [](const sol::object& index, const std::string& button) { return GameInput::IsGamepadButtonPressed(ParseGamepadIndex(index), ParseGamepadButton(button)); };
			input["IsGamepadButtonReleased"] = [](const sol::object& index, const std::string& button) { return GameInput::IsGamepadButtonReleased(ParseGamepadIndex(index), ParseGamepadButton(button)); };
			input["GetGamepadAxis"] = [](const sol::object& index, const std::string& axis) { return GameInput::GetGamepadAxis(ParseGamepadIndex(index), ParseGamepadAxis(axis)); };

			sol::table physics = lua.create_named_table("Physics");
			physics["Raycast"] = [&context](sol::this_state state, const glm::vec3& origin, const glm::vec3& direction, float maxDistance, const sol::object& options) -> sol::object {
				const PhysicsWorld& world = RequirePhysics(context);
				RequireCast("Raycast", origin, direction, maxDistance);
				return CastResult(
					state, context, ParseQueryOptions(world, options, true), [&](const PhysicsQueryFilter& filter) { return world.RaycastAll(origin, direction, maxDistance, filter); }, [&](const PhysicsQueryFilter& filter) { return world.Raycast(origin, direction, maxDistance, filter); });
			};
			physics["SphereCast"] = [&context](sol::this_state state, const glm::vec3& origin, float radius, const glm::vec3& direction, float maxDistance, const sol::object& options) -> sol::object {
				const PhysicsWorld& world = RequirePhysics(context);
				RequireCast("SphereCast", origin, direction, maxDistance);
				RequirePositive("SphereCast", "radius", radius);
				return CastResult(
					state, context, ParseQueryOptions(world, options, true), [&](const PhysicsQueryFilter& filter) { return world.SphereCastAll(origin, radius, direction, maxDistance, filter); }, [&](const PhysicsQueryFilter& filter) { return world.SphereCast(origin, radius, direction, maxDistance, filter); });
			};
			physics["BoxCast"] = [&context](sol::this_state state, const glm::vec3& origin, const glm::vec3& halfExtents, const glm::quat& rotation, const glm::vec3& direction, float maxDistance, const sol::object& options) -> sol::object {
				const PhysicsWorld& world = RequirePhysics(context);
				RequireCast("BoxCast", origin, direction, maxDistance);
				RequireBox("BoxCast", halfExtents, rotation);
				return CastResult(
					state, context, ParseQueryOptions(world, options, true), [&](const PhysicsQueryFilter& filter) { return world.BoxCastAll(origin, halfExtents, rotation, direction, maxDistance, filter); }, [&](const PhysicsQueryFilter& filter) { return world.BoxCast(origin, halfExtents, rotation, direction, maxDistance, filter); });
			};
			physics["OverlapSphere"] = [&context](sol::this_state state, const glm::vec3& center, float radius, const sol::object& options) {
				const PhysicsWorld& world = RequirePhysics(context);
				RequireFiniteVector("OverlapSphere", "center", center);
				RequirePositive("OverlapSphere", "radius", radius);
				return MakeEntityList(state, context, world.OverlapSphere(center, radius, ParseQueryOptions(world, options, false).Filter));
			};
			physics["OverlapBox"] = [&context](sol::this_state state, const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, const sol::object& options) {
				const PhysicsWorld& world = RequirePhysics(context);
				RequireFiniteVector("OverlapBox", "center", center);
				RequireBox("OverlapBox", halfExtents, rotation);
				return MakeEntityList(state, context, world.OverlapBox(center, halfExtents, rotation, ParseQueryOptions(world, options, false).Filter));
			};
			physics["GetLayers"] = [&context](sol::this_state state) {
				sol::state_view view(state);
				sol::table names = view.create_table();
				for (const std::string& name : RequirePhysics(context).GetLayers().GetNames())
					names.add(name);
				return names;
			};
			physics["GetGravity"] = [&context]() { return RequirePhysics(context).GetGravity(); };
			physics["SetGravity"] = [&context](const glm::vec3& gravity) { RequirePhysics(context).SetGravity(gravity); };

			sol::table audio = lua.create_named_table("Audio");
			audio["PlayOneShot"] = [&context](const std::string& clip, sol::optional<glm::vec3> position, sol::optional<float> volume, sol::optional<std::string> bus) {
				const glm::vec3* positionPointer = position ? &*position : nullptr;
				if (position)
					RequireFiniteVector("Audio.PlayOneShot", "position", *position);
				const float gain = RequireAudioVolume("Audio.PlayOneShot", volume.value_or(1.0f));
				const AudioBus target = bus ? ParseAudioBus("Audio.PlayOneShot", *bus) : AudioBus::SFX;
				return RequireAudio(context).PlayOneShot(clip, positionPointer, gain, target);
			};
			audio["SetBusVolume"] = [&context](const std::string& bus, float volume) {
				const AudioBus target = ParseAudioBus("Audio.SetBusVolume", bus);
				// The volume and bus are checked above; false then only means the bus failed to create (logged).
				return RequireAudio(context).SetBusVolume(target, RequireAudioVolume("Audio.SetBusVolume", volume));
			};
			audio["GetBusVolume"] = [&context](const std::string& bus) { return RequireAudio(context).GetBusVolume(ParseAudioBus("Audio.GetBusVolume", bus)); };
			audio["SetReverb"] = [&context](const sol::table& settings) {
				AudioSystem& system = RequireAudio(context);
				// ReadReverbSettings raises an error for anything SetReverb refuses today; this catches a rule it adds later.
				if (!system.SetReverb(ReadReverbSettings(settings, system.GetReverb())))
					throw std::runtime_error("Audio.SetReverb: the reverb was not changed (see the log)");
			};
			audio["GetReverb"] = [&context](sol::this_state state) {
				const AudioReverbSettings& reverb = RequireAudio(context).GetReverb();
				sol::table result = sol::state_view(state).create_table();
				for (const AudioReverbField& field : AudioReverbFields)
					result[field.Name] = reverb.*field.Member;
				return result;
			};

			sol::table log = lua.create_named_table("Log");
			log["Trace"] = [](sol::variadic_args args) { BS_TRACE("{}", JoinArguments(args)); };
			log["Info"] = [](sol::variadic_args args) { BS_INFO("{}", JoinArguments(args)); };
			log["Warn"] = [](sol::variadic_args args) { BS_WARN("{}", JoinArguments(args)); };
			log["Error"] = [](sol::variadic_args args) { BS_ERROR("{}", JoinArguments(args)); };
			// Lua's print goes to the engine log too.
			lua["print"] = [](sol::variadic_args args) { BS_INFO("{}", JoinArguments(args)); };

			sol::table time = lua.create_named_table("Time");
			time["GetDelta"] = [&context]() { return context.DeltaTime; };
			time["GetElapsed"] = [&context]() { return RequireScene(context).GetTime(); };
			time["GetFrame"] = [&context]() { return RequireScene(context).GetFrameCount(); };

			sol::table debug = lua.create_named_table("Debug");
			const glm::vec4 defaultColor(0.2f, 1.0f, 0.3f, 1.0f);
			debug["DrawLine"] = [defaultColor](const glm::vec3& from, const glm::vec3& to, sol::optional<glm::vec4> color) { DebugDraw::Line(from, to, color.value_or(defaultColor)); };
			debug["DrawArrow"] = [defaultColor](const glm::vec3& from, const glm::vec3& to, sol::optional<glm::vec4> color) { DebugDraw::Arrow(from, to, color.value_or(defaultColor)); };
			debug["DrawBox"] = [defaultColor](const glm::vec3& center, const glm::vec3& halfExtents, sol::optional<glm::vec4> color) {
				DebugDraw::Box(glm::translate(glm::mat4(1.0f), center), halfExtents, color.value_or(defaultColor));
			};
			debug["DrawSphere"] = [defaultColor](const glm::vec3& center, float radius, sol::optional<glm::vec4> color) { DebugDraw::Sphere(center, radius, color.value_or(defaultColor)); };

			RegisterUIBindings(lua, context);

			sol::table game = lua.create_named_table("Game");
			game["Quit"] = [&context]() { RequireScene(context).RequestQuit(); };
		}

		ScriptHost& RequireHost(ScriptContext& context)
		{
			if (!context.Host)
				throw std::runtime_error(NotRunningError);
			return *context.Host;
		}

		// The owner of a timer or event handler the running code creates. An owner that is no longer active
		// (for example in OnDestroy) would never run it, which is an error rather than a silent no-op.
		UUID RequireActiveOwner(ScriptContext& context, const char* function)
		{
			if (!RequireHost(context).IsOwnerActive(context.GetCurrentOwner()))
				throw std::runtime_error(std::string(function) + ": the calling script instance is being destroyed or has failed, so this would never run");
			return context.GetCurrentOwner();
		}

		std::string TypeName(const sol::object& value)
		{
			return sol::type_name(value.lua_state(), value.get_type());
		}

		sol::protected_function RequireFunction(const sol::object& value, const char* function)
		{
			if (value.get_type() != sol::type::function)
				throw std::runtime_error(std::string(function) + ": expected a function, got " + TypeName(value));
			return value.as<sol::protected_function>();
		}

		// A duration in seconds: a finite number, > 0 when positive is set, otherwise >= 0.
		double RequireSeconds(const sol::object& value, const char* function, bool positive)
		{
			const double seconds = value.get_type() == sol::type::number ? value.as<double>() : std::nan("");
			if (!std::isfinite(seconds) || seconds < 0.0 || (positive && seconds == 0.0))
				throw std::runtime_error(std::string(function) + ": seconds must be a finite number " + (positive ? "> 0" : ">= 0"));
			return seconds;
		}

		int64_t RequireInteger(const sol::object& value, const char* function, const char* argument, int64_t min, int64_t max)
		{
			const std::optional<int64_t> integer = ToInteger(value);
			if (!integer || *integer < min || *integer > max)
				throw std::runtime_error(std::string(function) + ": " + argument + " must be an integer from " + std::to_string(min) + " to " + std::to_string(max));
			return *integer;
		}

		std::string RequireName(const sol::object& value, const char* function, const char* what)
		{
			if (value.get_type() != sol::type::string || value.as<std::string>().empty())
				throw std::runtime_error(std::string(function) + ": " + what + " must be a non-empty string");
			return value.as<std::string>();
		}

		// Raw identity of two Lua values. sol's operator== compares on each reference's own thread, which is wrong
		// when one was obtained inside a coroutine; both are pushed onto one stack here.
		bool RawEqual(const sol::object& a, const sol::object& b)
		{
			lua_State* state = b.lua_state();
			a.push(state);
			b.push(state);
			const bool equal = lua_rawequal(state, -1, -2) != 0;
			lua_pop(state, 2);
			return equal;
		}

		// The instance an instance method was called on (self:StartCoroutine(...)), by its entity's UUID. With
		// requireActive, an instance that is being destroyed or has failed is an error (what it would start
		// could never run); otherwise such an instance is accepted (stopping its coroutines is harmless).
		UUID RequireInstance(ScriptContext& context, const sol::object& self, const char* function, bool requireActive)
		{
			ScriptHost& host = RequireHost(context);
			const auto usage = std::string(function) + " must be called on a script instance (self:" + function + "(...))";
			if (self.get_type() != sol::type::table)
				throw std::runtime_error(usage);
			const sol::object entity = self.as<sol::table>()["Entity"];
			if (!entity.is<ScriptEntity>())
				throw std::runtime_error(usage);
			const UUID id = entity.as<ScriptEntity>().ID;
			if (requireActive && !host.IsOwnerActive(id))
				throw std::runtime_error(std::string(function) + ": this instance is being destroyed or has failed, so the coroutine would never run");
			// Only the instance itself may act for its entity. GetInstance returns nil (which has no Lua state, so
			// it must not be compared) for an instance that is gone.
			const sol::object instance = host.GetInstance(id);
			if (instance.valid())
			{
				if (!RawEqual(instance, self))
					throw std::runtime_error(usage);
			}
			else if (requireActive)
			{
				throw std::runtime_error(usage);
			}
			return id;
		}

		// Wait(seconds) and WaitFrames(n) suspend the coroutine the scheduler is running. Plain C functions:
		// lua_yield must be the return of a C function, and since Lua is compiled as C, its errors longjmp, so
		// nothing here may own resources with destructors. WaitTarget returns the scheduler only when the caller may
		// wait; otherwise it raises the Lua error (and never returns).
		ScriptTasks* WaitTarget(lua_State* state, const char* function)
		{
			const auto* context = static_cast<const ScriptContext*>(lua_touserdata(state, lua_upvalueindex(1)));
			if (!context->Host || !context->Host->GetTasks().IsRunningCoroutine(state))
			{
				luaL_error(state, "%s can only be called in a coroutine started with self:StartCoroutine", function);
				return nullptr;
			}
			if (!lua_isyieldable(state))
			{
				luaL_error(state, "%s cannot suspend the coroutine here (inside a function called from C, such as an event handler run by Events.Emit)", function);
				return nullptr;
			}
			return &context->Host->GetTasks();
		}

		int LuaWait(lua_State* state)
		{
			const lua_Number seconds = lua_type(state, 1) == LUA_TNUMBER ? lua_tonumber(state, 1) : -1.0;
			if (!std::isfinite(seconds) || seconds < 0.0)
				return luaL_error(state, "Wait: seconds must be a finite number >= 0");
			ScriptTasks* tasks = WaitTarget(state, "Wait");
			if (!tasks)
				return 0;
			tasks->RequestWaitSeconds(state, seconds);
			return lua_yield(state, 0);
		}

		int LuaWaitFrames(lua_State* state)
		{
			int isInteger = 0;
			const lua_Integer frames = lua_type(state, 1) == LUA_TNUMBER ? lua_tointegerx(state, 1, &isInteger) : 0;
			if (!isInteger || frames < 1)
				return luaL_error(state, "WaitFrames: frames must be an integer >= 1");
			ScriptTasks* tasks = WaitTarget(state, "WaitFrames");
			if (!tasks)
				return 0;
			tasks->RequestWaitFrames(state, static_cast<uint64_t>(frames));
			return lua_yield(state, 0);
		}

		void RegisterWaitFunction(sol::state& lua, ScriptContext& context, const char* name, lua_CFunction function)
		{
			lua_State* state = lua.lua_state();
			lua_pushlightuserdata(state, &context);
			lua_pushcclosure(state, function, 1);
			lua_setglobal(state, name);
		}

		// TaskHandle/EventHandle: Cancel and IsActive act on the running session's registry and report false
		// once play has stopped.
		template<typename Handle, typename Registry>
		void RegisterHandleType(sol::state& lua, ScriptContext& context, const char* name, Registry& (ScriptHost::*registry)())
		{
			auto active = [&context, registry]() -> Registry* { return context.Host ? &(context.Host->*registry)() : nullptr; };
			sol::usertype<Handle> type = lua.new_usertype<Handle>(name, sol::no_constructor);
			type["Cancel"] = [active](const Handle& handle) {
				Registry* found = active();
				return found != nullptr && found->Cancel(handle.ID);
			};
			type["IsActive"] = [active](const Handle& handle) {
				Registry* found = active();
				return found != nullptr && found->IsActive(handle.ID);
			};
			type[sol::meta_function::equal_to] = [](const Handle& a, const Handle& b) { return a.ID == b.ID; };
			type[sol::meta_function::to_string] = [name](const Handle& handle) { return std::string(name) + "(" + std::to_string(handle.ID) + ")"; };
		}

		// Storage.Save/Load results: true, or false and the reason (also logged), so a game can tell the player.
		std::tuple<bool, sol::optional<std::string>> StorageResult(const char* function, bool succeeded, const std::string& error)
		{
			if (succeeded)
				return { true, sol::nullopt };
			BS_CORE_ERROR("{}: {}", function, error);
			return { false, error };
		}

		// Key under which the registry holds the instance-metatable factory (MakeInstanceMetatable).
		constexpr const char* InstanceMetatableFactoryKey = "Basalt.InstanceMetatableFactory";

		void RegisterGameplay(sol::state& lua, ScriptContext& context)
		{
			RegisterHandleType<ScriptTaskHandle>(lua, context, "TaskHandle", &ScriptHost::GetTasks);
			RegisterHandleType<ScriptEventHandle>(lua, context, "EventHandle", &ScriptHost::GetEvents);

			sol::table timer = lua.create_named_table("Timer");
			timer["After"] = [&context](const sol::object& seconds, const sol::object& callback) {
				ScriptTasks& tasks = RequireHost(context).GetTasks();
				const double delay = RequireSeconds(seconds, "Timer.After", false);
				const sol::protected_function function = RequireFunction(callback, "Timer.After");
				return ScriptTaskHandle{ tasks.AddOneShot(RequireActiveOwner(context, "Timer.After"), function, delay) };
			};
			timer["Every"] = [&context](const sol::object& seconds, const sol::object& callback, const sol::object& count) {
				ScriptTasks& tasks = RequireHost(context).GetTasks();
				const double interval = RequireSeconds(seconds, "Timer.Every", true);
				const sol::protected_function function = RequireFunction(callback, "Timer.Every");
				const auto times = IsNil(count) ? 0u : static_cast<uint32_t>(RequireInteger(count, "Timer.Every", "count", 1, std::numeric_limits<uint32_t>::max()));
				return ScriptTaskHandle{ tasks.AddRepeating(RequireActiveOwner(context, "Timer.Every"), function, interval, times) };
			};

			RegisterWaitFunction(lua, context, "Wait", &LuaWait);
			RegisterWaitFunction(lua, context, "WaitFrames", &LuaWaitFrames);

			// Methods every script instance has (self:StartCoroutine(...)), unless its class defines its own.
			sol::table methods = lua.create_table();
			methods["StartCoroutine"] = [&context](sol::this_state state, const sol::object& self, const sol::object& function, sol::variadic_args args) {
				ScriptTasks& tasks = RequireHost(context).GetTasks();
				const UUID owner = RequireInstance(context, self, "StartCoroutine", true);
				const sol::protected_function body = RequireFunction(function, "StartCoroutine");
				return ScriptTaskHandle{ tasks.StartCoroutine(state, owner, body, args) };
			};
			methods["StopCoroutine"] = [&context](const sol::object& self, const sol::object& handle) {
				ScriptTasks& tasks = RequireHost(context).GetTasks();
				const UUID owner = RequireInstance(context, self, "StopCoroutine", false);
				if (!handle.is<ScriptTaskHandle>())
					throw std::runtime_error("StopCoroutine: expected the TaskHandle returned by StartCoroutine, got " + TypeName(handle));
				switch (tasks.StopCoroutine(handle.as<ScriptTaskHandle>().ID, owner))
				{
					case ScriptTasks::StopResult::Stopped:
						return true;
					case ScriptTasks::StopResult::NotActive:
						return false;
					case ScriptTasks::StopResult::NotOwned:
						break;
				}
				throw std::runtime_error("StopCoroutine: the handle is a timer or another instance's coroutine (use handle:Cancel())");
			};
			methods["StopAllCoroutines"] = [&context](const sol::object& self) { RequireHost(context).GetTasks().CancelCoroutinesOwnedBy(RequireInstance(context, self, "StopAllCoroutines", false)); };

			// Instances look a key up in their class first (which keeps class inheritance through metatables
			// working), then in these methods.
			sol::load_result chunk = lua.load(R"(
				local methods = ...
				return function(class)
					return { __index = function(_, key)
						local value = class[key]
						if value == nil then
							value = methods[key]
						end
						return value
					end }
				end)",
											  "=instance metatable");
			BS_CORE_ASSERT(chunk.valid(), "the instance metatable factory must compile");
			const sol::protected_function makeFactory = chunk;
			const sol::protected_function_result factory = makeFactory(methods);
			BS_CORE_ASSERT(factory.valid(), "the instance metatable factory must load");
			lua.registry()[InstanceMetatableFactoryKey] = factory.get<sol::protected_function>();

			sol::table events = lua.create_named_table("Events");
			auto subscribe = [&context](const sol::object& name, const sol::object& handler, bool once, const char* function) {
				ScriptEvents& bus = RequireHost(context).GetEvents();
				const std::string eventName = RequireName(name, function, "the event name");
				const sol::protected_function callback = RequireFunction(handler, function);
				return ScriptEventHandle{ bus.Subscribe(RequireActiveOwner(context, function), eventName, callback, once) };
			};
			events["On"] = [subscribe](const sol::object& name, const sol::object& handler) { return subscribe(name, handler, false, "Events.On"); };
			events["Once"] = [subscribe](const sol::object& name, const sol::object& handler) { return subscribe(name, handler, true, "Events.Once"); };
			events["Emit"] = [&context](sol::this_state state, const sol::object& name, sol::variadic_args args) {
				ScriptEvents& bus = RequireHost(context).GetEvents();
				return bus.Emit(state, RequireName(name, "Events.Emit", "the event name"), args);
			};

			sol::table storage = lua.create_named_table("Storage");
			storage["Get"] = [&context](sol::this_state state, const sol::object& key, const sol::object& fallback) -> sol::object {
				const std::string name = RequireName(key, "Storage.Get", "the key");
				if (const nlohmann::json* value = RequireHost(context).GetStorage().Find(name))
					return JsonToLua(state, *value);
				return IsNil(fallback) ? sol::object(sol::lua_nil) : fallback;
			};
			storage["Set"] = [&context](const sol::object& key, const sol::object& value) {
				const std::string name = RequireName(key, "Storage.Set", "the key");
				const std::string function = "Storage.Set('" + name + "')";
				SaveData& data = RequireHost(context).GetStorage();
				// nil is JSON null, which Set treats as removing the key.
				nlohmann::json json = nullptr;
				try
				{
					if (!IsNil(value))
						json = LuaToJson(value);
				}
				catch (const std::runtime_error& e)
				{
					throw std::runtime_error(function + ": " + e.what());
				}
				std::string error;
				if (!data.Set(name, std::move(json), error))
					throw std::runtime_error(function + ": " + error);
			};
			storage["Has"] = [&context](const sol::object& key) { return RequireHost(context).GetStorage().Find(RequireName(key, "Storage.Has", "the key")) != nullptr; };
			storage["Delete"] = [&context](const sol::object& key) { return RequireHost(context).GetStorage().Remove(RequireName(key, "Storage.Delete", "the key")); };
			storage["Clear"] = [&context]() { RequireHost(context).GetStorage().Clear(); };
			storage["GetKeys"] = [&context](sol::this_state state) {
				sol::state_view view(state);
				sol::table keys = view.create_table();
				for (const std::string& key : RequireHost(context).GetStorage().GetKeys())
					keys.add(key);
				return keys;
			};
			// Save and Load report every failure as false and the reason, including save data that cannot be
			// located (no active project or user data directory), so `local ok, err = Storage.Save()` always works.
			storage["Save"] = [&context]() {
				ScriptHost& host = RequireHost(context);
				std::string error;
				bool saved = false;
				try
				{
					saved = host.GetStorage().Save(error);
				}
				catch (const std::runtime_error& e)
				{
					error = e.what();
				}
				return StorageResult("Storage.Save", saved, error);
			};
			storage["Load"] = [&context]() {
				ScriptHost& host = RequireHost(context);
				std::string error;
				bool loaded = false;
				try
				{
					loaded = host.LoadStorage(error);
				}
				catch (const std::runtime_error& e)
				{
					error = e.what();
				}
				return StorageResult("Storage.Load", loaded, error);
			};
		}
	}

	void RegisterScriptBindings(sol::state& lua, ScriptContext& context)
	{
		RegisterMath(lua, context);
		RegisterEntity(lua, context);
		RegisterScene(lua, context);
		RegisterGraphics(lua, context);
		RegisterSystems(lua, context);
		RegisterGameplay(lua, context);
	}

	sol::table MakeInstanceMetatable(const sol::state_view& lua, const sol::table& scriptClass)
	{
		const sol::protected_function factory = lua.registry()[InstanceMetatableFactoryKey];
		const sol::protected_function_result metatable = factory(scriptClass);
		BS_CORE_ASSERT(metatable.valid(), "the instance metatable factory cannot fail");
		return metatable.get<sol::table>();
	}

}
