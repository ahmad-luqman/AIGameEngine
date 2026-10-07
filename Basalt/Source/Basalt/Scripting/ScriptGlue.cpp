#include "Basalt/Scripting/ScriptGlue.h"

#include "Basalt/Audio/AudioSystem.h"
#include "Basalt/Core/Input.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Physics/PhysicsWorld.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Renderer/DebugDraw.h"
#include "Basalt/Renderer/GameUI.h"
#include "Basalt/Scene/ComponentRegistry.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/Scene/SceneSerializer.h"
#include "Basalt/Scripting/LuaJson.h"
#include "Basalt/Scripting/ScriptEngine.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <limits>
#include <sstream>
#include <stdexcept>

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

		Scene& RequireScene(ScriptContext& context)
		{
			if (!context.SceneContext)
				throw std::runtime_error("this function is only available while the scene is running");
			return *context.SceneContext;
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

		AudioSystem& RequireAudio(ScriptContext& context)
		{
			AudioSystem* audio = RequireScene(context).GetAudioSystem();
			if (!audio)
				throw std::runtime_error("audio is not running");
			return *audio;
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
			// Deterministic generator; Math.Seed makes runs reproducible.
			math["Seed"] = [&context](uint32_t seed) { context.Rng.Seed(seed); };
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
			luaMath["randomseed"] = [&context](sol::optional<int64_t> seed) { context.Rng.Seed(static_cast<uint32_t>(seed.value_or(Random::DefaultSeed))); };
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

			lua.new_usertype<ScriptEntity>("Entity", sol::no_constructor, "ID", sol::readonly_property([](const ScriptEntity& self) { return static_cast<int64_t>(static_cast<uint64_t>(self.ID)); }), "Name", sol::property([](const ScriptEntity& self) { return self.ResolveOrThrow().GetName(); }, [](const ScriptEntity& self, const std::string& name) { self.ResolveOrThrow().GetComponent<TagComponent>().Tag = name; }), "IsValid", [](const ScriptEntity& self) { return static_cast<bool>(self.Resolve()); },

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
					if (name == "Script" && context.Engine)
						context.Engine->EnsureInstance(entity); }, "RemoveComponent", [](const ScriptEntity& self, const std::string& name) {
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

										   "AddForce", [&context](const ScriptEntity& self, const glm::vec3& force) { RequirePhysics(context).AddForce(self.ResolveOrThrow(), force); }, "AddImpulse", [&context](const ScriptEntity& self, const glm::vec3& impulse) { RequirePhysics(context).AddImpulse(self.ResolveOrThrow(), impulse); }, "AddTorque", [&context](const ScriptEntity& self, const glm::vec3& torque) { RequirePhysics(context).AddTorque(self.ResolveOrThrow(), torque); }, "SetLinearVelocity", [&context](const ScriptEntity& self, const glm::vec3& velocity) { RequirePhysics(context).SetLinearVelocity(self.ResolveOrThrow(), velocity); }, "GetLinearVelocity", [&context](const ScriptEntity& self) { return RequirePhysics(context).GetLinearVelocity(self.ResolveOrThrow()); }, "SetAngularVelocity", [&context](const ScriptEntity& self, const glm::vec3& velocity) { RequirePhysics(context).SetAngularVelocity(self.ResolveOrThrow(), velocity); }, "GetAngularVelocity", [&context](const ScriptEntity& self) { return RequirePhysics(context).GetAngularVelocity(self.ResolveOrThrow()); },

										   "PlayAudio", [&context](const ScriptEntity& self) { return RequireAudio(context).Play(self.ResolveOrThrow()); }, "StopAudio", [&context](const ScriptEntity& self) { RequireAudio(context).Stop(self.ResolveOrThrow()); }, "IsAudioPlaying", [&context](const ScriptEntity& self) { return RequireAudio(context).IsPlaying(self.ResolveOrThrow()); },

										   "GetScript", [&context](const ScriptEntity& self) -> sol::object {
					self.ResolveOrThrow();
					return context.GetInstance ? context.GetInstance(self.ID) : sol::object(sol::lua_nil); }, "Destroy", [&context](const ScriptEntity& self) {
					if (Entity entity = self.Resolve())
						RequireScene(context).DestroyEntity(entity); },

										   sol::meta_function::equal_to, [](const ScriptEntity& a, const ScriptEntity& b) { return a == b; }, sol::meta_function::to_string, [](const ScriptEntity& self) {
					Entity entity = self.Resolve();
					return "Entity(" + (entity ? entity.GetName() : std::string("<destroyed>")) + ", " + std::to_string(static_cast<uint64_t>(self.ID)) + ")"; });
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
				if (context.Engine)
				{
					// Root first, then descendants depth-first.
					std::vector<Entity> tree;
					std::function<void(Entity)> collect = [&](Entity entity) {
						tree.push_back(entity);
						for (Entity child : entity.GetChildren())
							collect(child);
					};
					collect(root);
					context.Engine->EnsureInstances(tree);
				}
				return MakeEntity(state, context, root);
			};
			scene["Destroy"] = [&context](const ScriptEntity& entity) {
				if (Entity resolved = entity.Resolve())
					RequireScene(context).DestroyEntity(resolved);
			};
			scene["GetPrimaryCamera"] = [&context](sol::this_state state) { return MakeEntity(state, context, RequireScene(context).GetPrimaryCameraEntity()); };
			scene["GetEntityCount"] = [&context]() { return RequireScene(context).GetEntityCount(); };
		}

		void RegisterSystems(sol::state& lua, ScriptContext& context)
		{
			sol::table input = lua.create_named_table("Input");
			input["IsKeyDown"] = [](const std::string& key) { return Input::IsKeyDown(ParseKey(key)); };
			input["IsKeyPressed"] = [](const std::string& key) { return Input::IsKeyPressed(ParseKey(key)); };
			input["IsKeyReleased"] = [](const std::string& key) { return Input::IsKeyReleased(ParseKey(key)); };
			input["IsMouseButtonDown"] = [](const std::string& button) { return Input::IsMouseButtonDown(ParseMouseButton(button)); };
			input["IsMouseButtonPressed"] = [](const std::string& button) { return Input::IsMouseButtonPressed(ParseMouseButton(button)); };
			input["IsMouseButtonReleased"] = [](const std::string& button) { return Input::IsMouseButtonReleased(ParseMouseButton(button)); };
			input["GetMousePosition"] = []() { return Input::GetMousePosition(); };
			input["GetMouseDelta"] = []() { return Input::GetMouseDelta(); };
			input["GetMouseScroll"] = []() { return Input::GetMouseScroll(); };

			sol::table physics = lua.create_named_table("Physics");
			physics["Raycast"] = [&context](sol::this_state state, const glm::vec3& origin, const glm::vec3& direction, float maxDistance, sol::optional<ScriptEntity> ignore) -> sol::object {
				const auto hit = RequirePhysics(context).Raycast(origin, direction, maxDistance, ignore ? ignore->ID : UUID(0));
				if (!hit)
					return sol::lua_nil;
				sol::state_view view(state);
				sol::table result = view.create_table();
				result["Entity"] = MakeEntity(state, context, RequireScene(context).GetEntityByUUID(hit->EntityID));
				result["Point"] = hit->Point;
				result["Normal"] = hit->Normal;
				result["Distance"] = hit->Distance;
				return result;
			};
			physics["GetGravity"] = [&context]() { return RequirePhysics(context).GetGravity(); };
			physics["SetGravity"] = [&context](const glm::vec3& gravity) { RequirePhysics(context).SetGravity(gravity); };

			sol::table audio = lua.create_named_table("Audio");
			audio["PlayOneShot"] = [&context](const std::string& clip, sol::optional<glm::vec3> position, sol::optional<float> volume) {
				const glm::vec3* positionPointer = position ? &*position : nullptr;
				return RequireAudio(context).PlayOneShot(clip, positionPointer, volume.value_or(1.0f));
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

			sol::table ui = lua.create_named_table("UI");
			ui["Text"] = [](const std::string& text, float x, float y, sol::optional<float> size, sol::optional<glm::vec4> color, const sol::optional<std::string>& align) {
				UIAlign alignment = UIAlign::Left;
				const std::string alignName = align.value_or("Left");
				if (alignName == "Center")
					alignment = UIAlign::Center;
				else if (alignName == "Right")
					alignment = UIAlign::Right;
				else if (alignName != "Left")
					throw std::runtime_error("UI.Text align must be 'Left', 'Center' or 'Right'");
				GameUI::Text(text, { x, y }, size.value_or(32.0f), color.value_or(glm::vec4(1.0f)), alignment);
			};
			ui["Rect"] = [](float x, float y, float width, float height, sol::optional<glm::vec4> color) { GameUI::Rect({ x, y }, { width, height }, color.value_or(glm::vec4(0.0f, 0.0f, 0.0f, 0.5f))); };
			ui["GetSize"] = []() { return GameUI::GetCanvasSize(GameUI::GetViewportSize()); };

			sol::table game = lua.create_named_table("Game");
			game["Quit"] = [&context]() { RequireScene(context).RequestQuit(); };
		}

	}

	void RegisterScriptBindings(sol::state& lua, ScriptContext& context)
	{
		RegisterMath(lua, context);
		RegisterEntity(lua, context);
		RegisterScene(lua, context);
		RegisterSystems(lua, context);
	}

}
