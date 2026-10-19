#include "Basalt/Scripting/ScriptEngine.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Input.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Core/MemoryTracker.h"
#include "Basalt/Core/Profiler.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Project/SaveData.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/Scene/SceneEventListener.h"
#include "Basalt/Scripting/LuaJson.h"
#include "Basalt/Scripting/ScriptEvents.h"
#include "Basalt/Scripting/ScriptGlue.h"
#include "Basalt/Scripting/ScriptTasks.h"

#include <sol/sol.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace Basalt {

	namespace {

		// C++ exceptions thrown by bindings become ordinary Lua errors; sol2's default handler would also
		// print them to stderr, duplicating what the engine already reports.
		int QuietExceptionHandler(lua_State* state, sol::optional<const std::exception&>, std::string_view description)
		{
			return sol::stack::push(state, description);
		}

		// Routes enabled Lua warnings to the host log; outlives callbacks from lua_close finalizers.
		struct LuaWarnings
		{
			bool Enabled = false;
			bool Continuing = false;
			std::string Message;
			static void Receive(void* user, const char* message, int continued) noexcept
			{
				auto& warnings = *static_cast<LuaWarnings*>(user);
				try
				{
					if (!warnings.Continuing && !continued && message[0] == '@')
					{
						if (std::string_view(message) == "@on")
							warnings.Enabled = true;
						else if (std::string_view(message) == "@off")
							warnings.Enabled = false;
						return;
					}
					if (!warnings.Enabled)
						return;
					warnings.Message += message;
					warnings.Continuing = continued != 0;
					if (!continued)
					{
						BS_WARN("Lua warning: {}", warnings.Message);
						warnings.Message.clear();
					}
				}
				catch (...)
				{
					warnings.Continuing = false;
					warnings.Message.clear();
					std::fputs("Lua warning logging failed\n", stderr);
				}
			}
		};

		void OpenSafeLibraries(sol::state& lua, LuaWarnings& warnings)
		{
			lua_setwarnf(lua.lua_state(), &LuaWarnings::Receive, &warnings);
			lua.set_exception_handler(&QuietExceptionHandler);
			lua.open_libraries(sol::lib::base, sol::lib::package, sol::lib::string, sol::lib::math, sol::lib::table, sol::lib::utf8, sol::lib::coroutine);
			// Scripts load code only through require (resolved inside the project).
			lua["dofile"] = sol::lua_nil;
			lua["loadfile"] = sol::lua_nil;

			const std::filesystem::path root = Project::ResolvePath(".");
			const std::string base = root.lexically_normal().generic_string();
			lua["package"]["path"] = base + "/?.lua;" + base + "/?/init.lua";
			lua["package"]["cpath"] = "";
		}

		// Loads a script file and returns its class table.
		std::optional<sol::table> LoadScriptClass(sol::state& lua, const std::string& scriptPath, std::string& outError)
		{
			const auto source = FileSystem::ReadTextFile(Project::ResolvePath(scriptPath));
			if (!source)
			{
				outError = "cannot read script '" + scriptPath + "'";
				return std::nullopt;
			}

			sol::load_result chunk = lua.load(*source, "@" + scriptPath);
			if (!chunk.valid())
			{
				const sol::error error = chunk;
				outError = error.what();
				return std::nullopt;
			}

			sol::protected_function function = chunk;
			sol::protected_function_result result = function();
			if (!result.valid())
			{
				const sol::error error = result;
				outError = error.what();
				return std::nullopt;
			}
			if (result.get_type() != sol::type::table)
			{
				outError = scriptPath + ": script must return a table (its class)";
				return std::nullopt;
			}
			return result.get<sol::table>();
		}

		sol::object ConvertProperty(const sol::state_view& lua, const sol::object& defaultValue, const nlohmann::json& value)
		{
			// Keep vector-typed properties as vectors when overridden from JSON arrays of numbers.
			float components[4] = {};
			bool numeric = value.is_array() && value.size() <= 4;
			for (size_t i = 0; numeric && i < value.size(); i++)
				numeric = JsonToFloat(value[i], components[i]);
			if (numeric && value.size() == 3 && defaultValue.is<glm::vec3>())
				return sol::make_object(lua, glm::vec3(components[0], components[1], components[2]));
			if (numeric && value.size() == 2 && defaultValue.is<glm::vec2>())
				return sol::make_object(lua, glm::vec2(components[0], components[1]));
			if (numeric && value.size() == 4 && defaultValue.is<glm::vec4>())
				return sol::make_object(lua, glm::vec4(components[0], components[1], components[2], components[3]));
			return JsonToLua(lua, value);
		}

		// An instance's value of a property: the component's override, converted like the default, or the default (a
		// copied vector, so instances never share and mutate the class default). An override that does not convert sets
		// outError and gives the default.
		sol::object PropertyValue(const sol::state_view& lua, const std::string& name, const sol::object& defaultValue, const ScriptComponent& component, std::string& outError)
		{
			if (const auto overrideIt = component.Properties.find(name); overrideIt != component.Properties.end())
			{
				try
				{
					return ConvertProperty(lua, defaultValue, *overrideIt);
				}
				catch (const std::exception& e)
				{
					outError = component.Script + ": invalid value for property '" + name + "': " + e.what();
					return defaultValue;
				}
			}
			if (const sol::optional<glm::vec3> vector = defaultValue.as<sol::optional<glm::vec3>>())
				return sol::make_object(lua, *vector);
			return defaultValue;
		}

		// The parts of `package` require uses, unless a script replaced them.
		struct PackageView
		{
			sol::table Loaded;
			sol::protected_function SearchPath;
			std::string Path;
		};

		// What require itself uses, kept in the registry when the engine was made (ScriptEngine's constructor), so a script
		// replacing the global `package` (or its loaded and searchpath fields) hides no module from hot reload: Lua's own
		// loaded-module table, the original searchpath, and the path of the original package table, which require reads.
		// nullopt only when a script set that path to something other than text (require then fails too).
		std::optional<PackageView> GetPackage(sol::state& lua)
		{
			const sol::table registry = lua.registry();
			auto loaded = registry.get<sol::optional<sol::table>>(LUA_LOADED_TABLE);
			auto searchPath = registry.get<sol::optional<sol::protected_function>>("Basalt.SearchPath");
			const auto package = registry.get<sol::optional<sol::table>>("Basalt.Package");
			auto path = package ? package->get<sol::optional<std::string>>("path") : sol::nullopt;
			if (!loaded || !searchPath || !path)
				return std::nullopt;
			return PackageView{ *loaded, *searchPath, *path };
		}

		// The file require would load for a module name now (nullopt for the standard libraries and missing modules).
		// package.path holds native narrow text (OpenSafeLibraries), so the result decodes the same way.
		std::optional<std::filesystem::path> FindModuleFile(const PackageView& package, const std::string& name)
		{
			const sol::protected_function_result found = package.SearchPath(name, package.Path);
			if (!found.valid() || found.get_type() != sol::type::string)
				return std::nullopt;
			return std::filesystem::path(found.get<std::string>()).lexically_normal();
		}

		// Equivalent, not only equal: "require 'util'" finds Util.lua on case-insensitive file systems.
		bool IsAnyOf(const std::filesystem::path& file, const std::vector<std::filesystem::path>& files)
		{
			return std::any_of(files.begin(), files.end(), [&](const std::filesystem::path& other) {
				std::error_code error;
				return file == other || std::filesystem::equivalent(file, other, error);
			});
		}

		// Owners of the code that runs at the top level of scripts (ScriptEngine::Impl::ScriptClass::Owner) and of
		// required modules (ScriptEngine::Impl::ModuleOwners). One equals a given random entity UUID with probability
		// 2^-64, and new ones skip UUIDs the scene uses (scene files are untrusted).
		constexpr uint64_t ClassOwnerBase = 0xC1A5500000000000ull;
		// Raw key that marks the metatables MakeInstanceMetatable made for a class: a hot reload swaps only those, never
		// a metatable a script set on its instance itself.
		constexpr const char* InstanceMetatableMarker = "__basaltInstance";

	}

	// The engine's private state, and the ScriptHost the bindings use (definitions after ScriptEngineAccess).
	struct ScriptEngine::Impl final : ScriptHost
	{
		explicit Impl(ScriptEngine& engine)
			: Engine(engine)
		{
		}

		sol::object GetInstance(UUID id) override;
		void EnsureInstances(const std::vector<Entity>& entities) override;
		bool IsOwnerActive(UUID owner) const override;
		bool IsSharedOwner(UUID owner) const override { return owner == 0 || ClassOwners.contains(owner); }
		void FailOwner(UUID owner, const std::string& message, const std::string& where) override;
		ScriptTasks& GetTasks() override { return Tasks; }
		ScriptEvents& GetEvents() override { return Events; }
		SaveData& GetStorage() override;
		bool LoadStorage(std::string& outError) override;

		struct Instance
		{
			std::string ScriptPath;
			sol::table Self;
			bool Failed = false;
			// OnCreate has been called. OnDestroy is only called on created instances.
			bool Created = false;
			// OnCreate returned without an error: a hot reload re-enables an instance whose OnCreate failed with OnCreate,
			// one that failed later with OnReload.
			bool Initialized = false;
		};

		struct ScriptClass
		{
			sol::table Table;
			// Shared by the class's instances: methods come from the class, then the engine's instance methods.
			sol::table InstanceMetatable;
			// Owns what the class's top-level code started (timers, event handlers), which ends when a hot reload replaces
			// the class: otherwise every reload would add another copy.
			UUID Owner = 0;
		};

		ScriptEngine& Engine;
		// Declared before Lua so it outlives lua_close(): __gc metamethods may still call bindings.
		ScriptContext Context;
		LuaWarnings Warnings;
		sol::state Lua{ sol::default_at_panic, &MemoryTracker::LuaAllocator };
		// Declared after Lua: they hold Lua references, which must be released before the state closes.
		ScriptTasks Tasks{ Lua, Context, *this };
		ScriptEvents Events{ Lua, Context, *this };
		// Loaded on the first Storage call, so sessions that never use it never touch the disk.
		std::optional<SaveData> Storage;
		std::unordered_map<std::string, ScriptClass> Classes;
		std::unordered_set<std::string> FailedClasses;
		// Owner of a script's or module's top level -> what it is, for messages ("'Assets/Scripts/Player.lua'",
		// "module 'Assets.Scripts.Util'").
		std::unordered_map<UUID, std::string> ClassOwners;
		uint64_t ClassLoads = 0;
		// Module name -> the owner of what its top level started (it runs once, for whichever script requires it first,
		// so it belongs to the module, not to that script's class).
		std::unordered_map<std::string, UUID> ModuleOwners;
		// A module ForgetModules dropped from require's cache, until SettleForgottenModules decides its fate.
		struct ForgottenModule
		{
			std::string Name;
			// What require had cached.
			sol::object Value;
			// What its top level started (0: nothing recorded).
			UUID Owner;
		};
		std::vector<ForgottenModule> ForgottenModules;
		// Module name -> the modules whose top level required it: forgotten with it, as they hold its old table.
		std::unordered_map<std::string, std::set<std::string>> ModuleDependents;
		// The modules being loaded right now, innermost last.
		std::vector<std::string> ModuleStack;
		// Modules whose require failed (Lua does not cache those): when one of their files changes, the classes that
		// failed to load and those whose last reload failed are tried again.
		std::set<std::string> FailedRequires;
		// Classes still running their previous version because their last hot reload failed.
		std::unordered_set<std::string> StaleClasses;
		// A replaced package table was reported (once per session).
		bool ReportedPackageProblem = false;
		std::unordered_map<UUID, Instance> Instances;
		// Creation order, for deterministic callback order.
		std::vector<UUID> Order;
		std::unordered_set<UUID> Pending;
		// The gamepad connections scripts were told about (Input::GetGamepadConnection, 0 = none): Update announces
		// the difference, so gamepads connected before play, changes while the scene does not update (paused) and a
		// gamepad replaced at its index are announced too. Instances started later hear about these on start.
		std::array<uint32_t, Input::MaxGamepads> AnnouncedGamepads = {};
		bool Started = false;

		void OnScriptComponentChanged(entt::registry& registry, entt::entity entity)
		{
			if (const auto* id = registry.try_get<IDComponent>(entity))
				Pending.insert(id->ID);
		}
	};

	struct ScriptEngineAccess
	{
		static sol::object GetInstance(ScriptEngine& engine, UUID id)
		{
			auto it = engine.m_Impl->Instances.find(id);
			if (it == engine.m_Impl->Instances.end() || it->second.Failed)
				return sol::lua_nil;
			return it->second.Self;
		}

		// The instance exists, has started (OnCreate), has not failed, and its entity is not being destroyed:
		// its timers, coroutines and event handlers may run.
		static bool IsActive(const ScriptEngine& engine, UUID id)
		{
			auto it = engine.m_Impl->Instances.find(id);
			if (it == engine.m_Impl->Instances.end() || it->second.Failed || !it->second.Created)
				return false;
			Entity entity = engine.m_Scene->GetEntityByUUID(id);
			return entity && !engine.m_Scene->IsEntityPendingDestruction(entity);
		}

		// Cancels everything the instance owns (its timers, coroutines and event handlers).
		static void ReleaseOwned(ScriptEngine& engine, UUID id)
		{
			engine.m_Impl->Tasks.CancelOwnedBy(id);
			engine.m_Impl->Events.RemoveOwnedBy(id);
		}

		// Disables a live instance after an error: no more callbacks, and nothing it owns runs again.
		static void Fail(ScriptEngine& engine, UUID id)
		{
			auto it = engine.m_Impl->Instances.find(id);
			if (it == engine.m_Impl->Instances.end())
				return;
			it->second.Failed = true;
			ReleaseOwned(engine, id);
		}

		// Calls self:<name>(args...) if defined, with the instance as the current owner. Returns false (after
		// reporting) if the call raised an error.
		template<typename... Args>
		static bool Invoke(ScriptEngine& engine, UUID id, const sol::table& self, const char* name, Args&&... args)
		{
			const sol::object callback = self[name];
			if (callback.get_type() != sol::type::function)
				return true;
			sol::protected_function function = callback.as<sol::protected_function>();
			ScriptOwnerScope scope(engine.m_Impl->Context, id, name);
			const sol::protected_function_result result = function(self, std::forward<Args>(args)...);
			if (result.valid())
				return true;
			const sol::error error = result;
			engine.ReportError(std::string(error.what()) + " (in " + name + ")");
			return false;
		}

		// Calls a callback on a live instance. Disables the instance on error.
		template<typename... Args>
		static void Call(ScriptEngine& engine, UUID id, const char* name, Args&&... args)
		{
			auto it = engine.m_Impl->Instances.find(id);
			if (it == engine.m_Impl->Instances.end() || it->second.Failed)
				return;

			// Copy the table: the callback may destroy this instance (e.g. by removing its component).
			const sol::table self = it->second.Self;
			if (!Invoke(engine, id, self, name, std::forward<Args>(args)...))
				Fail(engine, id);
		}

		// Calls a callback on every live instance in creation order (instances created by a callback wait for the
		// next call), skipping entities about to be destroyed.
		template<typename... Args>
		static void CallOnEveryInstance(ScriptEngine& engine, const char* name, const Args&... args)
		{
			const std::vector<UUID> order = engine.m_Impl->Order;
			for (UUID uuid : order)
			{
				Entity entity = engine.m_Scene->GetEntityByUUID(uuid);
				if (entity && !engine.m_Scene->IsEntityPendingDestruction(entity))
					Call(engine, uuid, name, args...);
			}
		}

		// Removes the instance first, then calls OnDestroy on it, so the callback cannot re-enter teardown.
		static void Teardown(ScriptEngine& engine, UUID id)
		{
			ScriptEngine::Impl& impl = *engine.m_Impl;
			auto it = impl.Instances.find(id);
			if (it == impl.Instances.end())
				return;
			ScriptEngine::Impl::Instance instance = std::move(it->second);
			impl.Instances.erase(it);
			impl.Order.erase(std::remove(impl.Order.begin(), impl.Order.end(), id), impl.Order.end());
			// Cancel what it owns. The instance is already removed, so anything OnDestroy tries to start raises an
			// error instead of silently never running.
			ReleaseOwned(engine, id);
			if (instance.Created && !instance.Failed)
				Invoke(engine, id, instance.Self, "OnDestroy");
		}

		// Reports an error of an owner's timer, coroutine or event handler, naming the owner, and disables it.
		static void FailOwner(ScriptEngine& engine, UUID owner, const std::string& message, const std::string& where)
		{
			// What a script's or module's top level started is shared, as the session's is: only the failing timer or
			// handler ends (its caller ends it).
			if (const auto it = engine.m_Impl->ClassOwners.find(owner); it != engine.m_Impl->ClassOwners.end())
			{
				engine.ReportError(message + " (in " + where + " started by the top level of " + it->second + ")");
				return;
			}
			std::string ownerName = "the play session";
			if (owner != 0)
			{
				Entity entity = engine.m_Scene->GetEntityByUUID(owner);
				ownerName = "'" + (entity ? entity.GetName() : std::string("<destroyed>")) + "' (" + std::to_string(static_cast<uint64_t>(owner)) + ")";
			}
			engine.ReportError(message + " (in " + where + " of " + ownerName + ")");
			if (owner != 0)
				Fail(engine, owner);
		}

		// The session's save data, created (not loaded) on first use. Throws std::runtime_error when it cannot be
		// located.
		static SaveData& LocateStorage(ScriptEngine& engine)
		{
			ScriptEngine::Impl& impl = *engine.m_Impl;
			if (impl.Storage)
				return *impl.Storage;
			const Ref<Project>& project = Project::GetActive();
			if (!project)
				throw std::runtime_error("Storage needs an active project (its name names the save data directory)");
			std::string error;
			const std::optional<std::filesystem::path> path = SaveData::GetDefaultPath(project->GetConfig().Name, error);
			if (!path)
				throw std::runtime_error("Storage: " + error);
			return impl.Storage.emplace(*path);
		}

		// Calls OnCreate once on an instance that exists, has not been created, and whose entity still has
		// its script and is not about to be destroyed.
		static void StartInstance(ScriptEngine& engine, UUID id)
		{
			ScriptEngine::Impl& impl = *engine.m_Impl;
			auto it = impl.Instances.find(id);
			if (it == impl.Instances.end() || it->second.Created || it->second.Failed)
				return;
			Entity entity = engine.m_Scene->GetEntityByUUID(id);
			if (!entity || engine.m_Scene->IsEntityPendingDestruction(entity) || !entity.HasComponent<ScriptComponent>())
				return;
			it->second.Created = true;
			Call(engine, id, "OnCreate");
			// Looked up again: OnCreate may have destroyed the entity.
			if (const auto started = impl.Instances.find(id); started != impl.Instances.end() && !started->second.Failed)
				started->second.Initialized = true;
			// Gamepads announced before this instance existed (OnGamepadConnected is for every instance).
			for (uint32_t index = 0; index < Input::MaxGamepads; index++)
			{
				if (impl.AnnouncedGamepads[index] != 0)
					Call(engine, id, "OnGamepadConnected", static_cast<int>(index));
			}
		}
	};

	// Class loading and properties, shared by instance creation and hot reload.
	struct ScriptClassLoader
	{
		// Loads a script's class, its top-level code running under a new class owner. On failure (or an exception) what
		// that code started is cancelled.
		static std::optional<ScriptEngine::Impl::ScriptClass> Load(ScriptEngine& engine, const std::string& path, std::string& outError)
		{
			ScriptEngine::Impl& impl = *engine.m_Impl;
			const UUID owner = NewOwner(engine, "'" + path + "'");
			// Released unless the load succeeds, whatever leaves this function.
			struct ReleaseUnlessKept
			{
				ScriptEngine& Engine;
				UUID Owner;
				bool Kept = false;
				~ReleaseUnlessKept()
				{
					if (!Kept)
						Release(Engine, Owner);
				}
			} guard{ engine, owner };
			std::optional<sol::table> table;
			{
				ScriptOwnerScope scope(impl.Context, owner, "class load");
				table = LoadScriptClass(impl.Lua, path, outError);
			}
			if (!table)
				return std::nullopt;
			sol::table metatable = MakeInstanceMetatable(impl.Lua, *table);
			metatable.raw_set(InstanceMetatableMarker, true);
			guard.Kept = true;
			return ScriptEngine::Impl::ScriptClass{ *table, metatable, owner };
		}

		// A new owner for top-level code, described as `name` in messages.
		static UUID NewOwner(ScriptEngine& engine, std::string name)
		{
			ScriptEngine::Impl& impl = *engine.m_Impl;
			UUID owner(ClassOwnerBase | ++impl.ClassLoads);
			while (engine.m_Scene->GetEntityByUUID(owner) || impl.ClassOwners.contains(owner))
				owner = UUID(ClassOwnerBase | ++impl.ClassLoads);
			impl.ClassOwners.emplace(owner, std::move(name));
			return owner;
		}

		// Ends what a class's (or module's) top-level code started.
		static void Release(ScriptEngine& engine, UUID owner)
		{
			ScriptEngineAccess::ReleaseOwned(engine, owner);
			engine.m_Impl->ClassOwners.erase(owner);
		}

		// The owner of a module's top level, made on its first load.
		static UUID ModuleOwner(ScriptEngine& engine, const std::string& name)
		{
			ScriptEngine::Impl& impl = *engine.m_Impl;
			if (const auto it = impl.ModuleOwners.find(name); it != impl.ModuleOwners.end())
				return it->second;
			const UUID owner = NewOwner(engine, "module '" + name + "'");
			impl.ModuleOwners.emplace(name, owner);
			return owner;
		}

		// require, with what a module's top level starts owned by the module, and the modules that require it recorded.
		// Every value require returns is passed on (Lua 5.4 adds the loader data, such as the file name).
		static sol::variadic_results Require(ScriptEngine& engine, sol::this_state state, const sol::object& name)
		{
			ScriptEngine::Impl& impl = *engine.m_Impl;
			const sol::protected_function require = sol::state_view(state).registry()["Basalt.Require"];
			const std::string module = name.is<std::string>() ? name.as<std::string>() : std::string();
			if (!module.empty() && !impl.ModuleStack.empty() && impl.ModuleStack.back() != module)
				impl.ModuleDependents[module].insert(impl.ModuleStack.back());
			impl.ModuleStack.push_back(module);
			struct PopModule
			{
				std::vector<std::string>& Stack;
				~PopModule() { Stack.pop_back(); }
			} pop{ impl.ModuleStack };
			sol::protected_function_result result = [&]() {
				ScriptOwnerScope scope(impl.Context, module.empty() ? impl.Context.GetCurrentOwner() : ModuleOwner(engine, module), "module load");
				return require(name);
			}();
			if (!result.valid())
			{
				impl.FailedRequires.insert(module);
				// Not cached by require: what its top level started before the error ends, and a later attempt starts afresh.
				if (const auto owner = impl.ModuleOwners.find(module); owner != impl.ModuleOwners.end())
				{
					Release(engine, owner->second);
					impl.ModuleOwners.erase(owner);
				}
				const sol::error error = result;
				throw std::runtime_error(error.what());
			}
			impl.FailedRequires.erase(module);
			sol::variadic_results values;
			for (int i = 0; i < result.return_count(); i++)
				values.push_back(result.get<sol::object>(i));
			return values;
		}

		// Forgets the modules whose file is one of `files` (absolute, lexically normal) and those that required them,
		// directly or not, so the next require loads them again. What their old top level started stays alive until
		// SettleForgottenModules knows whether the new versions load (Impl::ForgottenModules). True if a changed module
		// was loaded, or if that cannot be told (outError then says why, and the caller reloads every class). A changed
		// file of a module whose require failed sets outFailedRequireChanged.
		static bool ForgetModules(ScriptEngine& engine, const std::vector<std::filesystem::path>& files, bool& outFailedRequireChanged, std::string& outError)
		{
			ScriptEngine::Impl& impl = *engine.m_Impl;
			std::optional<PackageView> package = GetPackage(impl.Lua);
			if (!package)
			{
				outError = "a script set package.path to something other than text, so changed modules cannot be found: every script loads again";
				return true;
			}
			outFailedRequireChanged = std::any_of(impl.FailedRequires.begin(), impl.FailedRequires.end(), [&](const std::string& name) {
				const auto file = FindModuleFile(*package, name);
				return file && IsAnyOf(*file, files);
			});
			// A snapshot of the names first: the searchpath calls below run Lua, which must not change the table being
			// traversed.
			std::vector<std::string> names;
			for (const auto& [key, value] : package->Loaded)
			{
				if (key.is<std::string>())
					names.push_back(key.as<std::string>());
			}
			std::vector<std::string> forget;
			for (const std::string& name : names)
			{
				if (const auto file = FindModuleFile(*package, name); file && IsAnyOf(*file, files))
					forget.push_back(name);
			}
			if (forget.empty())
				return false;
			std::set<std::string> forgotten;
			while (!forget.empty())
			{
				const std::string name = std::move(forget.back());
				forget.pop_back();
				if (!forgotten.insert(name).second)
					continue;
				if (const auto it = impl.ModuleDependents.find(name); it != impl.ModuleDependents.end())
					forget.insert(forget.end(), it->second.begin(), it->second.end());
			}
			for (const std::string& name : forgotten)
			{
				ScriptEngine::Impl::ForgottenModule module{ name, package->Loaded[name], 0 };
				package->Loaded[name] = sol::lua_nil;
				// Detached, not released: a new load gets an owner of its own, while the old one's work keeps running.
				if (const auto it = impl.ModuleOwners.find(name); it != impl.ModuleOwners.end())
				{
					module.Owner = it->second;
					impl.ModuleOwners.erase(it);
				}
				impl.ForgottenModules.push_back(std::move(module));
			}
			return true;
		}

		// After the classes loaded again: a forgotten module whose new version does not load keeps its previous one (back in
		// require's cache, its top level's work still running, as a class that does not load keeps running its previous
		// version); otherwise what the old version's top level started ends. A module nothing required again also ends.
		static void SettleForgottenModules(ScriptEngine& engine)
		{
			ScriptEngine::Impl& impl = *engine.m_Impl;
			std::vector<ScriptEngine::Impl::ForgottenModule> forgotten = std::exchange(impl.ForgottenModules, {});
			std::optional<PackageView> package = GetPackage(impl.Lua);
			for (ScriptEngine::Impl::ForgottenModule& module : forgotten)
			{
				if (package && impl.FailedRequires.contains(module.Name) && !impl.ModuleOwners.contains(module.Name))
				{
					package->Loaded[module.Name] = module.Value;
					if (module.Owner != 0)
						impl.ModuleOwners.emplace(module.Name, module.Owner);
				}
				else if (module.Owner != 0)
				{
					Release(engine, module.Owner);
				}
			}
		}

		// Sets the instance's fields from the class's Properties (with the component's overrides); with onlyMissing,
		// only those it does not have yet. Raw writes: after creation an instance may carry a metatable of its own
		// whose __newindex would run (and could raise outside any protected call).
		static void AssignProperties(ScriptEngine& engine, sol::table& self, const sol::table& scriptClass, const ScriptComponent& component, bool onlyMissing)
		{
			const sol::object properties = scriptClass["Properties"];
			if (properties.get_type() != sol::type::table)
				return;
			for (const auto& [key, defaultValue] : properties.as<sol::table>())
			{
				if (!key.is<std::string>())
					continue;
				const std::string name = key.as<std::string>();
				if (onlyMissing && self.raw_get<sol::object>(name).get_type() != sol::type::lua_nil)
					continue;
				std::string error;
				self.raw_set(name, PropertyValue(engine.m_Impl->Lua, name, defaultValue, component, error));
				if (!error.empty())
					engine.ReportError(error);
			}
		}

		// The metatable is one MakeInstanceMetatable made (not one a script set itself).
		static bool IsInstanceMetatable(const sol::object& metatable)
		{
			return metatable.get_type() == sol::type::table && metatable.as<sol::table>().raw_get<sol::optional<bool>>(InstanceMetatableMarker).value_or(false);
		}
	};

	sol::object ScriptEngine::Impl::GetInstance(UUID id)
	{
		return ScriptEngineAccess::GetInstance(Engine, id);
	}

	void ScriptEngine::Impl::EnsureInstances(const std::vector<Entity>& entities)
	{
		Engine.EnsureInstances(entities);
	}

	bool ScriptEngine::Impl::IsOwnerActive(UUID owner) const
	{
		return owner == 0 || ClassOwners.contains(owner) || ScriptEngineAccess::IsActive(Engine, owner);
	}

	void ScriptEngine::Impl::FailOwner(UUID owner, const std::string& message, const std::string& where)
	{
		ScriptEngineAccess::FailOwner(Engine, owner, message, where);
	}

	SaveData& ScriptEngine::Impl::GetStorage()
	{
		const bool firstUse = !Storage.has_value();
		SaveData& data = ScriptEngineAccess::LocateStorage(Engine);
		std::string error;
		if (firstUse && !data.Load(error))
			Engine.ReportError("Storage: " + error);
		return data;
	}

	bool ScriptEngine::Impl::LoadStorage(std::string& outError)
	{
		return ScriptEngineAccess::LocateStorage(Engine).Load(outError);
	}

	ScriptEngine::ScriptEngine(Scene* scene)
		: m_Impl(CreateScope<Impl>(*this))
		, m_Scene(scene)
	{
		OpenSafeLibraries(m_Impl->Lua, m_Impl->Warnings);
		m_Impl->Context.SceneContext = scene;
		// Every play session starts from its seed (the project's), so its random numbers repeat run after run.
		m_Impl->Context.ResetRandom(scene->GetRandomSeed());
		m_Impl->Context.Host = m_Impl.get();
		RegisterScriptBindings(m_Impl->Lua, m_Impl->Context);

		// A module runs its top level once, for whichever script requires it first: what that code starts belongs to the
		// module (ScriptClassLoader::Require), not to that script's class, so reloading the class does not end it. The
		// original require stays in the registry (a captured Lua reference would outlive lua_close).
		sol::state& lua = m_Impl->Lua;
		lua.registry()["Basalt.Require"] = lua["require"];
		// Kept for hot reload (GetPackage), whatever scripts later do to the global package table.
		lua.registry()["Basalt.Package"] = lua["package"];
		lua.registry()["Basalt.SearchPath"] = lua["package"]["searchpath"];
		lua.set_function("require", [this](sol::this_state state, const sol::object& name) { return ScriptClassLoader::Require(*this, state, name); });

		entt::registry& registry = scene->GetRegistry();
		registry.on_construct<ScriptComponent>().connect<&Impl::OnScriptComponentChanged>(*m_Impl);
		registry.on_update<ScriptComponent>().connect<&Impl::OnScriptComponentChanged>(*m_Impl);
		registry.on_destroy<ScriptComponent>().connect<&Impl::OnScriptComponentChanged>(*m_Impl);
	}

	ScriptEngine::~ScriptEngine()
	{
		entt::registry& registry = m_Scene->GetRegistry();
		registry.on_construct<ScriptComponent>().disconnect<&Impl::OnScriptComponentChanged>(*m_Impl);
		registry.on_update<ScriptComponent>().disconnect<&Impl::OnScriptComponentChanged>(*m_Impl);
		registry.on_destroy<ScriptComponent>().disconnect<&Impl::OnScriptComponentChanged>(*m_Impl);

		// Lua references must be released before the state is closed, and bindings run by __gc metamethods
		// during lua_close must not reach the engine.
		m_Impl->Context.Host = nullptr;
		m_Impl->Tasks.Clear();
		m_Impl->Events.Clear();
		m_Impl->Instances.clear();
		m_Impl->Classes.clear();
		m_Impl->ForgottenModules.clear();
	}

	void ScriptEngine::ReportError(const std::string& message)
	{
		BS_CORE_ERROR("Script error: {}", message);
		m_Errors.push_back(message);
		if (SceneEventListener* listener = m_Scene ? m_Scene->GetEventListener() : nullptr)
			listener->OnScriptError(*m_Scene, message);
	}

	void ScriptEngine::EnsureInstance(Entity entity)
	{
		if (CreateInstance(entity))
			ScriptEngineAccess::StartInstance(*this, entity.GetUUID());
	}

	void ScriptEngine::EnsureInstances(const std::vector<Entity>& entities)
	{
		// Two phases so every new instance exists (and is reachable through GetScript) before any OnCreate.
		std::vector<UUID> created;
		for (Entity entity : entities)
		{
			if (entity && entity.HasComponent<ScriptComponent>() && CreateInstance(entity))
				created.push_back(entity.GetUUID());
		}
		for (UUID uuid : created)
			ScriptEngineAccess::StartInstance(*this, uuid);
	}

	bool ScriptEngine::CreateInstance(Entity entity)
	{
		Impl& impl = *m_Impl;
		if (!entity)
			return false;
		const UUID uuid = entity.GetUUID();
		impl.Pending.erase(uuid);

		// Copy the component: Lua run below (OnDestroy, the script chunk) may remove or move it in storage.
		const auto* liveComponent = entity.TryGetComponent<ScriptComponent>();
		if (!liveComponent)
		{
			ScriptEngineAccess::Teardown(*this, uuid);
			return false;
		}
		const ScriptComponent componentCopy = *liveComponent;
		const ScriptComponent* component = &componentCopy;

		auto existing = impl.Instances.find(uuid);
		if (existing != impl.Instances.end())
		{
			if (existing->second.ScriptPath == component->Script)
				return false;
			// Script changed: tear down the old instance.
			ScriptEngineAccess::Teardown(*this, uuid);
		}
		if (component->Script.empty() || !entity.IsValid())
			return false;

		// Load (or reuse) the class.
		auto classIt = impl.Classes.find(component->Script);
		if (classIt == impl.Classes.end())
		{
			if (impl.FailedClasses.contains(component->Script))
				return false;
			// Top-level script code belongs to the class, whichever instance happened to load it first.
			std::string error;
			std::optional<Impl::ScriptClass> scriptClass = ScriptClassLoader::Load(*this, component->Script, error);
			if (!scriptClass)
			{
				impl.FailedClasses.insert(component->Script);
				ReportError(error);
				return false;
			}
			classIt = impl.Classes.emplace(component->Script, std::move(*scriptClass)).first;
		}

		// Instance: fields from Properties (with per-entity overrides), methods via the class metatable.
		sol::table self = impl.Lua.create_table();
		self[sol::metatable_key] = classIt->second.InstanceMetatable;
		self["Entity"] = ScriptEntity{ uuid, m_Scene };
		ScriptClassLoader::AssignProperties(*this, self, classIt->second.Table, *component, false);

		Impl::Instance instance;
		instance.ScriptPath = component->Script;
		instance.Self = self;
		impl.Instances[uuid] = std::move(instance);
		impl.Order.push_back(uuid);

		return true;
	}

	bool ScriptEngine::HasInstance(Entity entity) const
	{
		return entity && m_Impl->Instances.contains(entity.GetUUID());
	}

	std::optional<nlohmann::json> ScriptEngine::GetInstanceField(Entity entity, const std::string& name) const
	{
		if (!entity)
			return std::nullopt;
		auto it = m_Impl->Instances.find(entity.GetUUID());
		if (it == m_Impl->Instances.end())
			return std::nullopt;
		try
		{
			return LuaToJson(it->second.Self[name].get<sol::object>());
		}
		catch (const std::exception&)
		{
			return std::nullopt;
		}
	}

	void ScriptEngine::Start()
	{
		Impl& impl = *m_Impl;
		impl.Started = true;
		impl.Pending.clear();
		// Every instance exists before the first OnCreate runs (scripts can reach each other in OnCreate).
		EnsureInstances(m_Scene->GetAllEntitiesOrdered());
	}

	void ScriptEngine::Update(Timestep ts, std::span<const UIWidgetEvent> uiEvents)
	{
		BS_PROFILE_SCOPE("Scripts");
		Impl& impl = *m_Impl;
		impl.Context.DeltaTime = ts.GetSeconds();
		impl.Tasks.BeginFrame(ts.GetSeconds());

		// Components added or changed since the last frame (outside of the Lua API).
		if (!impl.Pending.empty())
		{
			std::vector<UUID> pending(impl.Pending.begin(), impl.Pending.end());
			impl.Pending.clear();
			std::sort(pending.begin(), pending.end(), [](UUID a, UUID b) { return static_cast<uint64_t>(a) < static_cast<uint64_t>(b); });
			for (UUID uuid : pending)
				EnsureInstance(m_Scene->GetEntityByUUID(uuid));
		}

		DispatchGamepadConnections();
		for (const UIWidgetEvent& event : uiEvents)
			DispatchUIEvent(event);

		ScriptEngineAccess::CallOnEveryInstance(*this, "OnUpdate", ts.GetSeconds());

		// Timers and coroutines run after every OnUpdate, before physics.
		impl.Tasks.RunDue();
	}

	void ScriptEngine::DispatchGamepadConnections()
	{
		for (uint32_t index = 0; index < Input::MaxGamepads; index++)
		{
			const uint32_t connection = Input::GetGamepadConnection(index);
			uint32_t& announced = m_Impl->AnnouncedGamepads[index];
			if (connection == announced)
				continue;
			const bool wasConnected = announced != 0;
			announced = connection;
			if (wasConnected)
				ScriptEngineAccess::CallOnEveryInstance(*this, "OnGamepadDisconnected", static_cast<int>(index));
			if (connection != 0)
				ScriptEngineAccess::CallOnEveryInstance(*this, "OnGamepadConnected", static_cast<int>(index));
		}
	}

	void ScriptEngine::DispatchUIEvent(const UIWidgetEvent& event)
	{
		Entity widget = m_Scene->GetEntityByUUID(event.Widget);
		if (!widget || m_Scene->IsEntityPendingDestruction(widget))
			return;
		// Once per widget, method and kind of problem: fixing one problem must not hide the next.
		const auto report = [&](const char* kind, const std::string& problem) {
			if (m_ReportedUICallbacks.insert(fmt::format("{}/{}/{}", static_cast<uint64_t>(event.Widget), event.Method, kind)).second)
				ReportError("UI widget '" + widget.GetName() + "' cannot call " + event.Method + ": " + problem);
		};
		Entity target = m_Scene->GetEntityByUUID(event.Target);
		if (!target)
		{
			report("missing", "its Target entity " + std::to_string(static_cast<uint64_t>(event.Target)) + " does not exist");
			return;
		}
		// An earlier callback this frame destroyed the target (a menu closing itself): like a widget being destroyed,
		// nothing is left to call.
		if (m_Scene->IsEntityPendingDestruction(target))
			return;
		auto it = m_Impl->Instances.find(event.Target);
		if (it == m_Impl->Instances.end())
		{
			report("script", "'" + target.GetName() + "' has no running script");
			return;
		}
		// A failed instance already reported its error.
		if (it->second.Failed)
			return;
		if (it->second.Self[event.Method].get_type() != sol::type::function)
		{
			report("method", "the script of '" + target.GetName() + "' has no method " + event.Method);
			return;
		}
		const ScriptEntity widgetEntity{ event.Widget, m_Scene };
		const char* method = event.Method.c_str();
		if (const float* value = std::get_if<float>(&event.Value))
			ScriptEngineAccess::Call(*this, event.Target, method, widgetEntity, *value);
		else if (const bool* checked = std::get_if<bool>(&event.Value))
			ScriptEngineAccess::Call(*this, event.Target, method, widgetEntity, *checked);
		else if (const std::string* text = std::get_if<std::string>(&event.Value))
			ScriptEngineAccess::Call(*this, event.Target, method, widgetEntity, *text);
		else
			ScriptEngineAccess::Call(*this, event.Target, method, widgetEntity);
	}

	void ScriptEngine::LateUpdate(Timestep ts)
	{
		ScriptEngineAccess::CallOnEveryInstance(*this, "OnLateUpdate", ts.GetSeconds());
	}

	void ScriptEngine::Stop()
	{
		Impl& impl = *m_Impl;
		const std::vector<UUID> order = impl.Order;
		for (UUID uuid : order)
			ScriptEngineAccess::Teardown(*this, uuid);
		impl.Instances.clear();
		impl.Order.clear();
		// What is left belongs to the session (console or lua.exec timers and handlers): it ends with play.
		impl.Tasks.Clear();
		impl.Events.Clear();
		impl.Started = false;
	}

	void ScriptEngine::OnEntityDestroyed(Entity entity)
	{
		Impl& impl = *m_Impl;
		const UUID uuid = entity.GetUUID();
		impl.Pending.erase(uuid);
		ScriptEngineAccess::Teardown(*this, uuid);
	}

	void ScriptEngine::OnContactEvent(ContactEventType type, Entity a, Entity b, const std::optional<ContactInfo>& contact)
	{
		const char* callback = "OnCollisionBegin";
		switch (type)
		{
			case ContactEventType::CollisionBegin:
				callback = "OnCollisionBegin";
				break;
			case ContactEventType::CollisionEnd:
				callback = "OnCollisionEnd";
				break;
			case ContactEventType::TriggerEnter:
				callback = "OnTriggerEnter";
				break;
			case ContactEventType::TriggerExit:
				callback = "OnTriggerExit";
				break;
		}

		sol::state& lua = m_Impl->Lua;
		auto makeContact = [&lua](const ContactInfo& info) -> sol::object {
			sol::table table = lua.create_table();
			table["Point"] = info.Point;
			table["Normal"] = info.Normal;
			table["Impulse"] = info.Impulse;
			return table;
		};
		// Each side gets its own table: a callback may modify the one it receives.
		const sol::object contactA = contact ? makeContact(*contact) : sol::object(sol::lua_nil);
		ScriptEngineAccess::Call(*this, a.GetUUID(), callback, ScriptEntity{ b.GetUUID(), m_Scene }, contactA);
		if (a && b)
		{
			const sol::object contactB = contact ? makeContact(contact->Flipped()) : sol::object(sol::lua_nil);
			ScriptEngineAccess::Call(*this, b.GetUUID(), callback, ScriptEntity{ a.GetUUID(), m_Scene }, contactB);
		}
	}

	void ScriptEngine::OnAnimationEvent(Entity entity, const std::string& name, const std::string& clip)
	{
		ScriptEngineAccess::Call(*this, entity.GetUUID(), "OnAnimationEvent", name, clip);
	}

	void ScriptEngine::OnJointBroken(Entity holder, Entity body, Entity connected)
	{
		// Captured up front: a callback may destroy any of these entities.
		const UUID holderID = holder.GetUUID();
		const UUID bodyID = body ? body.GetUUID() : holderID;
		const UUID connectedID = connected ? connected.GetUUID() : UUID(0);
		auto notify = [this](UUID target, UUID other) {
			if (other != 0)
				ScriptEngineAccess::Call(*this, target, "OnJointBreak", ScriptEntity{ other, m_Scene });
			else
				ScriptEngineAccess::Call(*this, target, "OnJointBreak", sol::lua_nil);
		};
		notify(bodyID, connectedID);
		if (connectedID != 0)
			notify(connectedID, bodyID);
		// A separate joint entity hears about its own joint too, with the far side like its body, unless it is
		// one of the two bodies and was notified above.
		if (holderID != bodyID && holderID != connectedID)
			notify(holderID, connectedID);
	}

	ScriptReloadResult ScriptEngine::ReloadScripts(const std::vector<std::string>& changedFiles)
	{
		m_Impl->Context.ResetClipLeakWarnings();
		ScriptReloadResult result;
		// Lua runs here outside any callback: a sol2 error a script provokes (a replaced package table, an erroring
		// metamethod) must not escape to the host.
		try
		{
			ReloadClasses(changedFiles, result);
		}
		catch (const std::exception& e)
		{
			ReportError(std::string("hot reload stopped part way: ") + e.what());
			result.Error = e.what();
		}
		// Also after an error: the modules ForgetModules dropped must not stay in limbo.
		try
		{
			ScriptClassLoader::SettleForgottenModules(*this);
		}
		catch (const std::exception& e)
		{
			ReportError(std::string("hot reload could not settle the changed modules: ") + e.what());
			if (result.Error.empty())
				result.Error = e.what();
		}
		return result;
	}

	void ScriptEngine::ReloadClasses(const std::vector<std::string>& changedFiles, ScriptReloadResult& result)
	{
		Impl& impl = *m_Impl;
		const std::set<std::string> changed(changedFiles.begin(), changedFiles.end());
		std::vector<std::filesystem::path> files;
		files.reserve(changed.size());
		for (const std::string& path : changed)
			files.push_back(Project::ResolvePath(path).lexically_normal());
		// Any class may require a changed module, directly or not: all of them load again.
		std::string packageProblem;
		bool failedRequireChanged = false;
		const bool moduleChanged = ScriptClassLoader::ForgetModules(*this, files, failedRequireChanged, packageProblem);
		if (!packageProblem.empty() && !std::exchange(impl.ReportedPackageProblem, true))
			ReportError("hot reload: " + packageProblem);

		std::set<std::string> candidates;
		for (const auto& [path, scriptClass] : impl.Classes)
		{
			if (moduleChanged || changed.contains(path) || (failedRequireChanged && impl.StaleClasses.contains(path)))
				candidates.insert(path);
		}
		for (const std::string& path : impl.FailedClasses)
		{
			if (moduleChanged || failedRequireChanged || changed.contains(path))
				candidates.insert(path);
		}

		std::unordered_set<std::string> replaced;
		std::unordered_set<std::string> recovered;
		for (const std::string& path : candidates)
		{
			std::string error;
			std::optional<Impl::ScriptClass> loaded = ScriptClassLoader::Load(*this, path, error);
			const auto existing = impl.Classes.find(path);
			if (!loaded)
			{
				const bool running = existing != impl.Classes.end();
				std::string message = "hot reload of '" + path + "' failed";
				message += running ? ", the previous version keeps running: " : ": ";
				message += error;
				ReportError(message);
				if (running)
					impl.StaleClasses.insert(path);
				result.Failed.push_back({ path, error });
				continue;
			}
			impl.StaleClasses.erase(path);
			if (existing != impl.Classes.end())
			{
				ScriptClassLoader::Release(*this, existing->second.Owner);
				existing->second = std::move(*loaded);
				replaced.insert(path);
			}
			else
			{
				impl.Classes.emplace(path, std::move(*loaded));
				impl.FailedClasses.erase(path);
				recovered.insert(path);
			}
			result.Reloaded.push_back(path);
		}
		if (result.Reloaded.empty())
			return;

		// Existing instances: the new class's methods, and the properties it added. A copy of the order: the property
		// values can run Lua (metamethods), which may create instances.
		const size_t errorsBefore = m_Errors.size();
		const std::vector<UUID> order = impl.Order;
		std::vector<UUID> moved;
		for (UUID uuid : order)
		{
			auto it = impl.Instances.find(uuid);
			if (it == impl.Instances.end() || !replaced.contains(it->second.ScriptPath))
				continue;
			const Impl::ScriptClass& scriptClass = impl.Classes.at(it->second.ScriptPath);
			sol::table self = it->second.Self;
			Entity entity = m_Scene->GetEntityByUUID(uuid);
			// A metatable the script set on the instance itself stays: the new methods reach the instance only if that
			// metatable leads to them, so it is not counted as running the new code.
			if (!ScriptClassLoader::IsInstanceMetatable(self[sol::metatable_key]))
			{
				BS_CORE_WARN("Hot reload: the script of '{}' set its own metatable, which is kept; the new version's methods reach it only through that metatable", entity ? entity.GetName() : std::string("?"));
				continue;
			}
			self[sol::metatable_key] = scriptClass.InstanceMetatable;
			if (const auto* component = entity ? entity.TryGetComponent<ScriptComponent>() : nullptr)
			{
				// Copied: the property values below may run Lua that changes the component.
				const ScriptComponent componentCopy = *component;
				ScriptClassLoader::AssignProperties(*this, self, scriptClass.Table, componentCopy, true);
			}
			moved.push_back(uuid);
		}

		// Then the callbacks, once every instance runs the new code (OnReload may call other reloaded instances).
		std::vector<UUID> notified;
		for (UUID uuid : moved)
		{
			auto it = impl.Instances.find(uuid);
			Entity entity = m_Scene->GetEntityByUUID(uuid);
			if (it == impl.Instances.end() || !entity || m_Scene->IsEntityPendingDestruction(entity))
				continue;
			// Fixing the error that disabled an instance and saving the script is what brings it back. One whose OnCreate
			// failed (or that had not started) starts afresh.
			it->second.Failed = false;
			if (!it->second.Initialized)
			{
				it->second.Created = false;
				ScriptEngineAccess::StartInstance(*this, uuid);
			}
			else
			{
				ScriptEngineAccess::Call(*this, uuid, "OnReload");
			}
			notified.push_back(uuid);
		}

		// Entities whose script failed to load before get their instances now, as if they had just been spawned.
		if (!recovered.empty() && impl.Started)
		{
			std::vector<Entity> waiting;
			for (Entity entity : m_Scene->GetAllEntitiesOrdered())
			{
				const auto* component = entity.TryGetComponent<ScriptComponent>();
				if (component && recovered.contains(component->Script) && !HasInstance(entity) && !m_Scene->IsEntityPendingDestruction(entity))
					waiting.push_back(entity);
			}
			EnsureInstances(waiting);
			for (Entity entity : waiting)
				notified.push_back(entity.GetUUID());
		}
		result.Instances = static_cast<size_t>(std::count_if(notified.begin(), notified.end(), [&](UUID uuid) {
			const auto it = impl.Instances.find(uuid);
			return it != impl.Instances.end() && !it->second.Failed;
		}));
		result.InstanceErrors.assign(m_Errors.begin() + static_cast<std::ptrdiff_t>(std::min(errorsBefore, m_Errors.size())), m_Errors.end());
	}

	bool ScriptEngine::ExecuteString(const std::string& code, std::string& outResult)
	{
		ScriptOwnerScope scope(m_Impl->Context, UUID(0), "console");
		sol::state& lua = m_Impl->Lua;
		// Try as an expression first so "Scene.GetEntityCount()" returns its value.
		sol::load_result chunk = lua.load("return " + code, "=console");
		if (!chunk.valid())
			chunk = lua.load(code, "=console");
		if (!chunk.valid())
		{
			const sol::error error = chunk;
			outResult = error.what();
			return false;
		}

		sol::protected_function function = chunk;
		sol::protected_function_result result = function();
		if (!result.valid())
		{
			const sol::error error = result;
			outResult = error.what();
			return false;
		}

		outResult.clear();
		if (result.return_count() > 0)
		{
			const sol::protected_function toString = lua["tostring"];
			sol::protected_function_result text = toString(result.get<sol::object>(0));
			outResult = text.valid() ? text.get<std::string>() : "?";
		}
		return true;
	}

	bool ScriptEngine::CheckSyntax(const std::string& scriptPath, std::string& outError)
	{
		const auto source = FileSystem::ReadTextFile(Project::ResolvePath(scriptPath));
		if (!source)
		{
			outError = "cannot read script '" + scriptPath + "'";
			return false;
		}
		sol::state lua{ sol::default_at_panic, &MemoryTracker::LuaAllocator };
		const sol::load_result chunk = lua.load(*source, "@" + scriptPath);
		if (!chunk.valid())
		{
			const sol::error error = chunk;
			outError = error.what();
			return false;
		}
		return true;
	}

	std::optional<nlohmann::json> ScriptEngine::LoadScriptProperties(const std::string& scriptPath, std::string& outError)
	{
		ScriptContext context;
		LuaWarnings warnings;
		sol::state lua{ sol::default_at_panic, &MemoryTracker::LuaAllocator };
		OpenSafeLibraries(lua, warnings);
		RegisterScriptBindings(lua, context);

		auto scriptClass = LoadScriptClass(lua, scriptPath, outError);
		if (!scriptClass)
			return std::nullopt;

		const sol::object properties = (*scriptClass)["Properties"];
		if (properties.get_type() != sol::type::table)
			return nlohmann::json::object();
		try
		{
			nlohmann::json json = LuaToJson(properties);
			return json.is_object() ? json : nlohmann::json::object();
		}
		catch (const std::exception& e)
		{
			outError = scriptPath + ": invalid Properties table: " + e.what();
			return std::nullopt;
		}
	}

}
