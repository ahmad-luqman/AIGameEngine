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

Write scripts with `asset.write` (or directly as files under `<project>/Assets/Scripts/`). Iterate without
restarting play: a script written with `asset.write` during Play (not Simulate) is hot reloaded (instances keep
their fields; define `OnReload` to redo setup), and in a long-lived session (`basalt serve`/`mcp`, the editor) files
written with other tools are picked up by `asset.refresh` (check `scripts.failed` and `scripts.instanceErrors`; a
broken version leaves the old one running). Typical patterns:
- One "Game" controller script on an empty entity owns the rules and state (grid arrays, score, timers),
  reads input, and spawns/moves/destroys entities (`Scene.CreateEntity` + `AddComponent`,
  or `Scene.Instantiate("Assets/Prefabs/Block.bprefab", pos)`).
- Prefer input actions over raw keys: declare them with `project.set {"inputActions": {"Actions": [{"Name":
  "MoveX", "Type": "Axis", "Bindings": [{"Key": "D"}, {"Key": "A", "Scale": -1}, {"GamepadAxis": "LeftX"}]}]}}`
  and read `Input.GetAction("MoveX")` / `Input.IsActionPressed("Jump")`, so keys, gamepads and the mouse work
  alike and players can rebind (`Input.SetBindings`). `input.key`/`input.gamepad` still drive them in tests;
  `input.actions` shows their values. In the editor, injected input reaches scripts only while the Game view has
  focus (`editor.game_view {"focus": true}` first), and injected mouse buttons also need the real cursor over the
  Game view, as with a real keyboard and mouse: test mouse-driven games headless. First-person cameras: `Input.SetCursorMode("Locked")` and
  `Input.GetMouseDelta()`, yaw on the character's body and pitch on a child camera (Samples/Breakout shows actions,
  the mouse and the cursor; Samples/FirstPerson a full first-person setup).
- Sound design: put the `AudioListener` on the camera. `AudioSource.LowPass` (Hz, 0 = off), `ReverbSend` and
  `Doppler` apply live, so scripts drive them: raycast from the listener to a source to muffle it behind walls,
  switch `Audio.SetReverb` and sends in trigger zones, set `Doppler = 1` on fast movers (Samples/FirstPerson).
- Grid games (Tetris, Snake, Minesweeper): keep the logical state in Lua tables and mirror it with
  entities (cubes with `Mesh builtin://Cube` + `Material AlbedoColor`). Do not use physics for them.
- Shared looks: write a material file once (`material.create`, or `asset.write` of a `.bmaterial`; see
  Docs/AssetPipeline.md) and point every entity's `Material.Material` at it instead of repeating inline values;
  `material.extract` turns a model's materials into files, and `Model.glb#material/N` reuses one directly.
- Physics games: `RigidBody` (`Dynamic`/`Static`/`Kinematic`, `IsTrigger`) + a collider; react in
  `OnCollisionBegin`/`OnTriggerEnter`. Give walking characters (platformers, first-person) a
  `CapsuleCollider` + `CharacterController` (no RigidBody) and call `self.Entity:Move(velocity)` every
  frame (an upward part jumps when `IsGrounded()`). Put bodies on named
  layers (`RigidBody.Layer`) and set which layers collide with `project.set` `physicsLayers`
  (`{"Names": ["Default", "Player", "Pickup"], "IgnoredPairs": [["Player", "Pickup"]]}`). Set
  `RigidBody.Continuous` on fast projectiles and `RigidBody.AllowSleeping = false` on balls that roll slowly
  (a sleeping ball freezes where it stopped, even on a slope). Probe the world with `Physics.Raycast/SphereCast/BoxCast/OverlapSphere/OverlapBox`
  (options: `Ignore`, `Layers`, `IncludeTriggers`, `All`; see Docs/ScriptingAPI.md).
- Animated characters (Samples/Platformer): write the state machine with `asset.write` (`.banimgraph`), set its
  parameters and triggers from the movement script every frame, and let a root-motion locomotion blend space move the
  character (`Animator.RootMotion` + `RootJoint`; `Move` still supplies jumps and air movement, and fills in through
  fades into and out of root-motion states). Set a trigger and the jump's upward `Move` only on
  `Input.IsActionPressed`, keep pushing up until the controller rises, and debounce `IsGrounded()` so stairs do not
  flicker into a fall state; `CharacterController.JumpGrace` lets a jump pressed just after walking off a ledge work
  (coyote time). Add `InverseKinematics` Ground
  chains for the feet and `SpringBone` chains for hair and capes; Kinematic platforms moved through their transform
  carry characters standing on them.
- Imported levels and props: add `RigidBody {}` (Static by default) and `MeshCollider {}` to each entity with
  a `Mesh` component (an empty `Mesh` field uses the entity's mesh) so static geometry collides by its real
  triangles; without a RigidBody nothing collides. Dynamic bodies and triggers use the mesh's convex hull: set
  `Convex = true` on them. Prefer box/sphere/capsule colliders for simple moving objects: they are cheaper.
- Joints (`Joint` component): `Fixed`, `Point`, `Hinge`, `Slider`, `Distance`, `Cone`, `SixDOF` (ragdolls).
  An entity holds one joint; for several on one body (a rung on two ropes) give each its own child entity
  with `"BodyEntity": "<body name>"`. `LimitSpringFrequency` makes limits springy (a distance joint without
  limits becomes a bungee).
- Random numbers repeat every play session (the project's `RandomSeed`); `Math.Seed(n)` picks another sequence, `Debug.Draw*` to visualise logic, `Game.Quit()` to exit.
- HUDs and menus: put `UIRect` + `UIImage`/`UIText` entities in the scene (anchored, nine-slice panels,
  `.ttf` fonts; see "Screen UI" in Docs/ScriptingAPI.md) and update their text with `SetComponent`; check
  the layout headlessly with `ui.layout`. Quick per-frame text goes through `UI.Text(..., { Anchor = ... })`.
  Screenshots (`render.screenshot`, `editor.screenshot`, the runtime's `--screenshot`) include the game UI.
- Menus and settings: add `UIButton` / `UISlider` / `UICheckbox` to UI entities, with `Target` set to the
  menu entity (commands accept its name) and `OnClick` / `OnValueChanged` naming the menu script's methods
  (`function Menu:StartGame(button)`). They work with the mouse, arrows/Tab/Enter and gamepads. Test them
  headlessly: mouse positions are canvas units there, so click a rect from `ui.layout` (default size) with
  `input.mouse` (press, `play.step`, release, `play.step`), navigate with `input.key`, and drive a pad with
  `input.gamepad {connected: true, button: "A"}`. Per-frame menus can use `UI.Button/Checkbox/Slider` instead.
- Check every script compiles: `$B --project P script.check '{"path": "Assets/Scripts/Game.lua"}'`.

## 3. Build the scene with a batch file

Write `build_scene.json` (an array of requests) and run `$B --project P batch build_scene.json`:

```json
[
  {"command": "scene.new", "params": {"name": "Main"}},
  {"command": "entity.create", "params": {"name": "Game", "components": {"Script": {"Script": "Assets/Scripts/Game.lua"}}}},
  {"command": "entity.create", "params": {"name": "Camera", "components": {
     "Transform": {"Translation": [5, 10, 25]}, "Camera": {"Projection": "Orthographic", "OrthoSize": 24}}}},
  {"command": "entity.create", "params": {"name": "PostProcessing", "components": {"PostProcess": {"Exposure": 1.0}}}},
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

Write gameplay checks as assertions, so a batch reads as the game's specification and fails with the reason:
```json
[
  {"command": "events.subscribe", "params": {"types": ["collision", "scriptError", "playState"]}},
  {"command": "scene.snapshot", "params": {"name": "start"}},
  {"command": "play.start"},
  {"command": "play.step", "params": {"frames": 120, "assertNoErrors": true}},
  {"command": "assert.entity_exists", "params": {"entity": "Enemy", "count": 3}},
  {"command": "assert.component", "params": {"entity": "Ball", "component": "Transform", "fields": {"Translation": [0, 0.5, 0]}, "tolerance": 0.01}},
  {"command": "events.poll", "params": {"subscription": 1, "expect": [{"event": "collision", "type": "triggerEnter", "a": {"name": "Goal"}, "b": {"name": "Ball"}}]}},
  {"command": "scene.diff", "params": {"from": "start", "tolerance": 0.001}}
]
```
`events.poll`'s `expect` patterns name the fields an event must have (when a pattern names a contact's `a` or
`b`, the bodies match in either order, its `normal` then reversed; floats match within `tolerance`, default
0.0001); each pattern needs an event of its own, so list one twice to require two. `assertTypes` only checks the
kinds. `scene.diff` lists exactly what playing changed (entities added/removed, fields with
before and after); add `"expectError": "assertion failed"` to a request that must fail. Subscription IDs start at
1 in each CLI run. `Samples/MarbleRun/test_game.json` is a complete example (it also clicks HUD widgets with
`input.mouse` at canvas positions).
Game scripts get completion and type checks in code editors from `Docs/basalt.d.lua` (lua-language-server): add it
to `workspace.library` in the project's `.luarc.json`.

## 5. Look at it

- `build/bin/BasaltRuntime --project P --frames 60 --screenshot shot.png` renders the start scene after
  60 frames. Read the PNG to check.
- In a running editor (`build/bin/BasaltEditor --project P`), send the same commands over TCP
  (`echo '{"command":"..."}' | nc 127.0.0.1 7420`) plus `editor.screenshot` (editor camera),
  `editor.window_screenshot` (whole UI), `editor.camera`, `editor.select`, `render.screenshot`, and
  `render.compare {"reference": "Golden/Level1.png"}` (render and compare, with a `similarity` score). An
  `events.subscribe` on that connection streams log lines, script errors, play-state changes and collisions as
  extra lines (with an `event` key, no `ok`) instead of polling `log.get`.

## 6. Ship it

`$B --project P project.validate` lists what would break the exported game (missing or wrongly cased asset
paths, scenes or prefabs that do not load, scripts that do not compile); fix every error.
`$B --project P project.export '{"output": "Dist/Tetris", "smokeTestFrames": 120}'` validates again, produces a
standalone folder (runtime renamed after the game + assets + MoltenVK on macOS) and runs the exported game
headless for 120 frames: the command fails, with the game's log, if it does not exit cleanly or a script
reported an error (`project.smoke_test` re-runs that check on any export). For release builds export with a `Dist` build of the runtime
(`scripts/build.sh Dist`, then run that build's `basalt`).
