# Basalt Lua Scripting API

Gameplay code is written in Lua 5.4. A script is attached to an entity with a `Script` component
(`{"Script": "Assets/Scripts/Player.lua"}`). The feature test (`Tests/Data/FeatureTest/Assets/Scripts/FeatureTest.lua`)
calls every function on this page (checking its effect wherever it can be observed headlessly) and is the
best executable reference.

## Script structure

A script file returns a *class* table. Every entity using the script gets its own instance (`self`).

```lua
local Player = {}

-- Defaults for per-entity properties. Overrides live in the Script component's "Properties" and are
-- editable in the inspector. Supported: numbers, booleans, strings, Vec2/Vec3/Vec4.
Player.Properties = { Speed = 5.0, JumpImpulse = 6.0, Name = "Hero" }

function Player:OnCreate() end                 -- see "Creation order" below
function Player:OnUpdate(dt) end               -- every frame, before physics
function Player:OnLateUpdate(dt) end           -- every frame, after physics
function Player:OnDestroy() end                -- entity destroyed, component removed, or play stopped
function Player:OnCollisionBegin(other) end    -- other: Entity
function Player:OnCollisionEnd(other) end
function Player:OnTriggerEnter(other) end      -- either body is a trigger (RigidBody.IsTrigger)
function Player:OnTriggerExit(other) end

return Player
```

- `self.Entity` is the entity the instance belongs to; property values are fields (`self.Speed`).
- **Creation order:** when play starts, and when a prefab is instantiated, every new instance is created
  (with its properties) before any `OnCreate` runs, so `OnCreate` can use `GetScript()` on the others.
  A script added later on its own (`AddComponent("Script", ...)`) starts immediately. An entity destroyed
  before its `OnCreate` ran gets neither `OnCreate` nor `OnDestroy`.
- A runtime error disables only the failing instance and is reported (file:line) in the log and in
  `play.step`'s `scriptErrors`.
- Destroying entities from any callback is safe: destruction is deferred until the current update ends.
- `require("Assets.Scripts.Lib.Util")` loads project modules. `io`, `os`, `dofile` and `loadfile` are not
  available (scripts cannot touch the file system).

## Math types

| Type | Construction | Members / operators |
|------|--------------|---------------------|
| `Vec2` | `Vec2()`, `Vec2(s)`, `Vec2(x, y)` | `x y`, `+ - * /` (vector or scalar), unary `-`, `==`, `Length()`, `Normalized()`, `Dot(v)` |
| `Vec3` | `Vec3()`, `Vec3(s)`, `Vec3(x, y, z)` | `x y z`, operators as Vec2, `Length() Normalized() Dot(v) Cross(v) Distance(v) Lerp(v, t)` |
| `Vec4` | `Vec4()`, `Vec4(s)`, `Vec4(x, y, z, w)` | `x y z w`, `+ - *`, `==` (colors are Vec4 RGBA) |
| `Quat` | `Quat()` (identity), `Quat(w, x, y, z)` | `w x y z`, `q * q`, `q * Vec3` (rotate), `ToEuler() Normalized() Inverse()` |

`Quat.Identity()`, `Quat.FromEuler(Vec3 radians)`, `Quat.AngleAxis(radians, axis)`,
`Quat.LookRotation(forward [, up])`, `Quat.Slerp(a, b, t)`.

`Math.Pi`, `Math.Radians(deg)`, `Math.Degrees(rad)`, `Math.Clamp(v, min, max)`, `Math.Lerp(a, b, t)`,
`Math.Sign(v)`, `Math.Seed(n)`, `Math.Random()` (0..1), `Math.Random(min, max)`, `Math.RandomInt(min, max)`.
The random generator is deterministic; seed it for reproducible games and tests.

## Entity

Entity handles store an ID and stay safe to hold: using a destroyed entity raises an error
(`entity N no longer exists`); check with `e:IsValid()`.

| Member | Description |
|--------|-------------|
| `e.ID` (read-only), `e.Name` | Unique 64-bit ID, display name |
| `e:IsValid()` | Entity still exists |
| `e.Translation`, `e.Rotation` (Quat), `e.EulerAngles` (Vec3 radians), `e.Scale` | Local transform (relative to parent) |
| `e.WorldPosition` | World-space position (get/set) |
| `e:GetForward()`, `e:GetRight()`, `e:GetUp()` | World-space axes (forward is -Z) |
| `e:LookAt(target [, up])` | Rotate to face a world position |
| `e:HasComponent(name)` | e.g. `"RigidBody"` |
| `e:AddComponent(name [, table])` | Add with optional field values |
| `e:GetComponent(name)` | Snapshot table of all fields (nil if missing) |
| `e:SetComponent(name, table)` | Set the given fields (others unchanged); invalid fields raise errors |
| `e:RemoveComponent(name)` | Remove (Tag/Transform cannot be removed) |
| `e:GetParent()`, `e:SetParent(parent or nil)`, `e:GetChildren()`, `e:FindChild(name)` | Hierarchy (SetParent keeps the world transform) |
| `e:AddForce(v)`, `e:AddImpulse(v)`, `e:AddTorque(v)` | Physics (dynamic bodies) |
| `e:SetLinearVelocity(v)`, `e:GetLinearVelocity()`, `e:SetAngularVelocity(v)`, `e:GetAngularVelocity()` | Physics velocities |
| `e:PlayAudio()`, `e:StopAudio()`, `e:IsAudioPlaying()` | The entity's AudioSource |
| `e:GetScript()` | The entity's script instance (call its functions, read its fields) |
| `e:Destroy()` | Destroy at the end of the frame (with children) |

Component names and fields are listed by `basalt component.types` (and in Docs/Components.md).
Vectors in component tables are arrays (`{1, 2, 3}`); rotations are Euler degrees.

## Scene

`Scene.CreateEntity([name])`, `Scene.FindEntityByName(name)`, `Scene.FindEntitiesByName(name)`,
`Scene.GetEntityByID(id)`, `Scene.GetEntitiesWith(componentName)`, `Scene.Destroy(entity)`,
`Scene.Instantiate(prefabPath [, position [, parent]])` (scripts in the prefab start immediately),
`Scene.GetPrimaryCamera()`, `Scene.GetEntityCount()`.

## Input

Key names: `A`-`Z`, `D0`-`D9`, `Space`, `Enter`, `Escape`, `Tab`, `Backspace`, `Left`, `Right`, `Up`,
`Down`, `LeftShift`, `LeftControl`, `LeftAlt`, `F1`-`F12`, `KP0`-`KP9`, ... (case-insensitive).
Mouse buttons: `Left`, `Right`, `Middle`, `Button3`-`Button7`.

`Input.IsKeyDown(key)`, `Input.IsKeyPressed(key)` (this frame only), `Input.IsKeyReleased(key)`,
`Input.IsMouseButtonDown/Pressed/Released(button)`, `Input.GetMousePosition()`, `Input.GetMouseDelta()`,
`Input.GetMouseScroll()` (all Vec2).

## Physics

`Physics.Raycast(origin, direction, maxDistance [, ignoreEntity])` returns
`{ Entity, Point, Normal, Distance }` or nil (triggers are ignored). `Physics.GetGravity()`,
`Physics.SetGravity(v)`.

## Screen UI

Immediate-mode HUD drawn over the game. Submit everything every frame (usually from `OnUpdate`); the
canvas is 1080 units tall with the origin at the top-left and its width follows the window's aspect ratio.

- `UI.GetSize()` — canvas size (Vec2)
- `UI.Text(text, x, y [, size = 32 [, color = Vec4(1) [, align = "Left"|"Center"|"Right"]]])`
- `UI.Rect(x, y, width, height [, color = Vec4(0, 0, 0, 0.5)])`

## Audio, debug drawing, time, logging, game

- `Audio.PlayOneShot(clip [, position [, volume]])` (3D when a position is given)
- `Debug.DrawLine(from, to [, color])`, `Debug.DrawArrow(from, to [, color])`,
  `Debug.DrawBox(center, halfExtents [, color])`, `Debug.DrawSphere(center, radius [, color])` — drawn
  this frame only
- `Time.GetDelta()`, `Time.GetElapsed()`, `Time.GetFrame()`
- `Log.Trace/Info/Warn/Error(...)`, `print(...)` (to the engine log / editor console)
- `Game.Quit()` — ends the game (the runtime exits, the editor leaves play mode)
