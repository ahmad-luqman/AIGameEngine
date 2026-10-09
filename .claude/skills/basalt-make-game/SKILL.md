---
name: basalt-make-game
description: Build a complete game with the Basalt engine (e.g. "make me Tetris/Breakout/a platformer") using only the basalt CLI or the editor's automation server — no GUI interaction needed. Use whenever asked to create or modify a game, scene, prefab or gameplay script.
---

# Building a game with Basalt (as an agent)

Everything is done through JSON commands (Docs/AutomationCommands.md) plus Lua scripts
(Docs/ScriptingAPI.md). Components and their fields: Docs/Components.md. Build the tools first
(`scripts/build.sh`); binaries are in `build/bin/`.

## 1. Create the project

```bash
B=build/bin/basalt
$B project.create '{"path": "Games/Tetris", "name": "Tetris"}'
```

## 2. Write gameplay as Lua scripts

Write scripts with `asset.write` (or directly as files under `<project>/Assets/Scripts/`). Typical patterns:
- One "Game" controller script on an empty entity owns the rules and state (grid arrays, score, timers),
  reads `Input.IsKeyPressed`, and spawns/moves/destroys entities (`Scene.CreateEntity` + `AddComponent`,
  or `Scene.Instantiate("Assets/Prefabs/Block.bprefab", pos)`).
- Grid games (Tetris, Snake, Minesweeper): keep the logical state in Lua tables and mirror it with
  entities (cubes with `Mesh builtin://Cube` + `Material AlbedoColor`). Do not use physics for them.
- Physics games: `RigidBody` (`Dynamic`/`Static`/`Kinematic`, `IsTrigger`) + a collider; react in
  `OnCollisionBegin`/`OnTriggerEnter`. Give walking characters (platformers, first-person) a
  `CapsuleCollider` + `CharacterController` (no RigidBody) and call `self.Entity:Move(velocity)` every
  frame (an upward part jumps when `IsGrounded()`). Put bodies on named
  layers (`RigidBody.Layer`) and set which layers collide with `project.set` `physicsLayers`
  (`{"Names": ["Default", "Player", "Pickup"], "IgnoredPairs": [["Player", "Pickup"]]}`). Set
  `RigidBody.Continuous` on fast projectiles. Probe the world with `Physics.Raycast/SphereCast/BoxCast/OverlapSphere/OverlapBox`
  (options: `Ignore`, `Layers`, `IncludeTriggers`, `All`; see Docs/ScriptingAPI.md).
- Imported levels and props: add `RigidBody {}` (Static by default) and `MeshCollider {}` to each entity with
  a `Mesh` component (an empty `Mesh` field uses the entity's mesh) so static geometry collides by its real
  triangles; without a RigidBody nothing collides. Dynamic bodies and triggers use the mesh's convex hull: set
  `Convex = true` on them. Prefer box/sphere/capsule colliders for simple moving objects: they are cheaper.
- Joints (`Joint` component): `Fixed`, `Point`, `Hinge`, `Slider`, `Distance`, `Cone`, `SixDOF` (ragdolls).
  An entity holds one joint; for several on one body (a rung on two ropes) give each its own child entity
  with `"BodyEntity": "<body name>"`. `LimitSpringFrequency` makes limits springy (a distance joint without
  limits becomes a bungee).
- Use `Math.Seed(n)` for reproducible randomness, `Debug.Draw*` to visualise logic, `Game.Quit()` to exit.
- Check every script compiles: `$B --project P script.check '{"path": "Assets/Scripts/Game.lua"}'`.

## 3. Build the scene with a batch file

Write `build_scene.json` (an array of requests) and run `$B --project P batch build_scene.json`:

```json
[
  {"command": "scene.new", "params": {"name": "Main"}},
  {"command": "entity.create", "params": {"name": "Game", "components": {"Script": {"Script": "Assets/Scripts/Game.lua"}}}},
  {"command": "entity.create", "params": {"name": "Camera", "components": {
     "Transform": {"Translation": [5, 10, 25]}, "Camera": {"Projection": "Orthographic", "OrthoSize": 24}}}},
  {"command": "scene.settings", "params": {"Renderer": {"Exposure": 1.0}}},
  {"command": "scene.save", "params": {"path": "Assets/Scenes/Main.bscene"}},
  {"command": "project.set", "params": {"startScene": "Assets/Scenes/Main.bscene"}}
]
```
`scene.new` adds a camera, a sun and a sky unless `"empty": true`. Every response says `ok`; on error the
message names the bad field and the valid alternatives — fix and rerun. Batches stop nothing on error:
check every response.

## 4. Test headlessly (fast loop, no GPU needed)

```json
[
  {"command": "scene.open", "params": {"path": "Assets/Scenes/Main.bscene"}},
  {"command": "play.start"},
  {"command": "input.key", "params": {"key": "Left", "down": true}},
  {"command": "play.step", "params": {"frames": 10}},
  {"command": "input.key", "params": {"key": "Left", "down": false}},
  {"command": "play.step", "params": {"frames": 120}},
  {"command": "lua.exec", "params": {"code": "Scene.FindEntityByName('Game'):GetScript().Score"}},
  {"command": "entity.list", "params": {"nameContains": "Block"}},
  {"command": "log.get", "params": {"errorsOnly": true}}
]
```
`play.step` reports `scriptErrors` (with file:line). Expose game state as script fields so `lua.exec`
can assert on it. Play mode runs on a copy; the saved scene is never modified by playing.

## 5. Look at it

- `build/bin/BasaltRuntime --project P --frames 60 --screenshot shot.png` renders the start scene after
  60 frames (macOS/Homebrew: prefix `DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib`). Read the PNG to check.
- In a running editor (`build/bin/BasaltEditor --project P`), send the same commands over TCP
  (`echo '{"command":"..."}' | nc 127.0.0.1 7420`) plus `editor.screenshot` (editor camera),
  `editor.window_screenshot` (whole UI), `editor.camera`, `editor.select`, `render.screenshot`.

## 6. Ship it

`$B --project P project.export '{"output": "Dist/Tetris"}'` produces a standalone folder (runtime renamed
after the game + assets + MoltenVK on macOS). For release builds export with a `Dist` build of the runtime
(`scripts/build.sh Dist`, then run that build's `basalt`).
