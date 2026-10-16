---
name: basalt-add-component
description: Add a new component type to the Basalt engine end to end (data, serialization, systems, editor, Lua, tests, docs). Use when a feature needs new per-entity data.
---

# Adding a component

1. **Data** — `Basalt/Source/Basalt/Scene/Components.h`: a plain struct (PascalCase public fields with
   defaults, no runtime state). Add it to `AllComponents` (copying scenes/duplicating entities uses it).
2. **Serialization** — `Scene/ComponentRegistry.cpp`: a `Fields(FieldReader&, T&)` overload listing every
   field once (use `EnumField` for enums, `RotationField`/`AngleField` for angles), a `Write(const T&)`
   overload, and `infos.push_back(MakeInfo<T>("Name"))` in `BuildRegistry`. The name is what files, Lua and
   the automation API use. If invalid values could break a system, add a `std::string Validate(const T&)`
   overload beside the ParticleEmitter one, before `MakeInfo` (a later one is silently never called;
   components with a custom Deserialize, like Camera, validate there). It returns the problem, naming the
   field and value; see the
   ParticleEmitter one, whose rules live in `Particles/ParticleEmitterRules.h` so the system can also
   repair values C++ code wrote directly). Every load, Lua `SetComponent`, `component.set` and inspector
   edit runs it. Adding a missing component constructs it once with its final values.
3. **Runtime behaviour** — if a system consumes it (physics, audio, renderer, scripting), react to
   `on_construct/on_update/on_destroy` signals or read it each frame. Never store runtime handles in the
   component. Physics components get one `Wire<...>` line in `PhysicsWorld::Impl::WireSignals`
   (`Physics/PhysicsWorld.cpp`); new collider types also go in `BuildColliderShape` (`Physics/ColliderShapes.cpp`),
   and new Joint fields in the `JointFields.cpp` table (which fields apply, and whether a live joint updates them).
4. **Editor** — nothing to do: the inspector is generated from the registry. Add a creation preset to
   `SceneHierarchyPanel.cpp` if users create it often, and an overlay in `EditorLayer::DrawEditorOverlays`
   if it has a spatial extent.
5. **Lua** — generic access works automatically (`AddComponent/GetComponent/SetComponent`). Add typed
   helpers to `ScriptGlue.cpp` only for hot paths, document them in Docs/ScriptingAPI.md and declare them in
   Docs/basalt.d.lua (the `LuaTypeDefinitions` test fails otherwise).
6. **Tests** — round-trip is covered by `SceneTests.cpp` ("Every registered component round-trips"); add
   behaviour tests for the consuming system; add the component to `Tests/Data/FeatureTest` (scene +
   `FeatureTest.lua` list of component names) and run `scripts/test.sh`.
7. **Docs** — `python3 scripts/generate_docs.py` regenerates Docs/Components.md, Docs/AutomationCommands.md and the
   component section of Docs/basalt.d.lua (the `GeneratedDocs` ctest fails while they are stale).
