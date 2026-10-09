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
function Player:OnJointBreak(other) end        -- joint exceeded BreakForce/BreakTorque; other is the far body, nil for the world

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
Random numbers are deterministic: each play session starts from the same seed and produces the same
sequence on every platform. Lua's `math.random`/`math.randomseed` use the same generator (standard Lua
seeds them from the clock), so replays and automated tests reproduce exactly.
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
| `e:GetJointPosition()` | Hinge angle (degrees) or slider offset (meters) of the Joint this entity holds, from its rest pose; nil otherwise |
| `e:HasJoint()` | Whether the Joint this entity holds is a live constraint (false before both bodies exist, when it is invalid, and after it broke) |
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

`Physics.GetGravity()`, `Physics.SetGravity(v)`, and `Physics.GetLayers()` (the layer names the running
world uses, in order: the project's, read when play started).

### Layers and continuous collision

Each rigid body is on one named layer (`RigidBody.Layer`, default `"Default"`). The project defines up to
32 layers and which pairs of layers collide (`project.set` with `physicsLayers`, stored in
`Project.bproject`). The matrix is read when play starts. A layer name the project does not define falls
back to `Default` with a warning; `project.set` and `scene.info` list such bodies as
`unknownPhysicsLayers`. Scenes saved before named layers (numeric `Layer`, `CollisionMask`) load when no
body's mask excluded a layer: those bodies become `Default`. Other masks are a load error, since they need
named layers. From Lua, `SetComponent("RigidBody", { Layer = 1 })` is likewise read as `Default`.

Set `RigidBody.Continuous = true` on fast dynamic bodies (projectiles) so they cannot pass through thin
geometry between steps. It costs more per step and has no effect on static, kinematic or trigger bodies.

### Queries

Casts sweep a ray, sphere or box from `origin` along `direction` (any length) for up to `maxDistance`:

| Function | Returns |
|----------|---------|
| `Physics.Raycast(origin, direction, maxDistance [, options])` | closest hit or nil |
| `Physics.SphereCast(origin, radius, direction, maxDistance [, options])` | closest hit or nil |
| `Physics.BoxCast(origin, halfExtents, rotation, direction, maxDistance [, options])` | closest hit or nil |
| `Physics.OverlapSphere(center, radius [, options])` | array of entities |
| `Physics.OverlapBox(center, halfExtents, rotation [, options])` | array of entities |

A hit is `{ Entity, Point, Normal, Distance }`. A cast that starts inside a body hits it at distance 0,
whichever way it moves. Invalid arguments (a zero or non-finite direction, a non-finite position, a
non-positive distance, radius or half extent, a zero rotation) are script errors. `options` is an entity
to ignore, or a table:

- `Ignore`: an entity to skip (for example the caster).
- `Layers`: a non-empty list of layer names to test against (default: all). An unknown name is an
  error.
- `IncludeTriggers`: also hit triggers (default false).
- `All` (casts only): return an array of every hit entity, closest first, one hit per entity (default
  false).

Overlaps always list every entity they touch, each once. Unknown or misspelled options are errors.

```lua
-- Ground check that skips the player and only sees level geometry.
local ground = Physics.SphereCast(self.Entity.WorldPosition, 0.4, Vec3(0, -1, 0), 0.2,
	{ Ignore = self.Entity, Layers = { "Default", "Level" } })
-- Push every debris body within 5 m of an explosion.
for _, entity in ipairs(Physics.OverlapSphere(center, 5, { Layers = { "Debris" } })) do
	entity:AddImpulse((entity.WorldPosition - center):Normalized() * 10)
end
```

### Joints

A `Joint` component connects a rigid body to `ConnectedEntity`'s body, or to the world when it is 0. The
body it moves is `BodyEntity`'s, or the entity's own when `BodyEntity` is 0. `Type` is `Fixed`, `Point`
(ball and socket), `Hinge`, `Slider`, `Distance`, `Cone` or `SixDOF`. `Anchor` and `Axis` (hinge axis,
slider direction, cone axis or six-DOF twist axis) are in the body's local space; `Axis` turns with the
body but ignores its scale. `ConnectedAnchor` (distance joints) is in the connected entity's local space,
or a world position when the joint connects to the world. The pose the bodies have when the joint is built
is its rest pose: hinge angles, slider offsets and six-DOF limits are measured from it.

Limits (with `UseLimits`) depend on the type:

| Type | Limits |
|------|--------|
| `Hinge` | Angles in degrees, `-180 <= LimitMin <= 0 <= LimitMax <= 180` |
| `Slider` | Offsets in meters, `LimitMin <= 0 <= LimitMax` |
| `Distance` | Lengths in meters, `0 <= LimitMin <= LimitMax`; without limits it keeps its starting length |
| `Cone` | `LimitMax` is the cone's half angle in degrees (0..180); `LimitMin` stays 0. Without limits it swings freely |
| `SixDOF` | `LinearLimitMin/Max` (meters) and `AngularLimitMin/Max` (degrees) per axis of the joint frame (see below) |
| `Fixed`, `Point` | None |

Other values are clamped with a warning. `LimitSpringFrequency` (Hz, 0 = rigid) softens hinge, slider,
distance and six-DOF translation limits into a spring, damped by `LimitSpringDamping` (1 = critical, lower
values bounce). A spring on a distance joint without limits pulls it back to its starting length: a
bungee cord.

**Six-DOF joints** (ragdolls) build a frame at the anchor: X is `Axis` (twist), Y is `SecondaryAxis`
(made perpendicular to `Axis`) and Z is X × Y. With `UseLimits`, each axis's range must contain 0, and
min == max locks it; X rotation (twist) lies within ±180 and the Y and Z rotations form a swing cone whose
half angles are `AngularLimitMax.y`/`.z` (leave their minimums at 0, or mirror them). For a free
translation, use a range wider than the body can travel. Without `UseLimits`, translation is locked and
rotation free. A limit spring softens only limited translation axes; locked axes and rotation limits stay
rigid.

```lua
-- A door that swings open on a motor (hinge on the door's left edge, around Y).
door:AddComponent("Joint", { Type = "Hinge", ConnectedEntity = frame, Anchor = { -0.5, 0, 0 },
    UseLimits = true, LimitMin = 0, LimitMax = 100 })
door:SetComponent("Joint", { MotorMode = "Position", MotorTarget = 90 })  -- cheap: updates the live joint

-- A shoulder: free twist of ±30 degrees, 70/40 degree swing cone.
arm:AddComponent("Joint", { Type = "SixDOF", ConnectedEntity = torso, Anchor = { 0, 0.5, 0 },
    Axis = { 0, -1, 0 }, SecondaryAxis = { 1, 0, 0 }, UseLimits = true,
    AngularLimitMin = { -30, -70, -40 }, AngularLimitMax = { 30, 70, 40 } })
```

**Several joints on one body.** An entity holds one `Joint`. To give a body more (a ladder rung held by
two ropes), put each joint on its own entity, usually a child of the body, and set `BodyEntity` to the
body. The joint entity needs no body or transform of its own:

```lua
-- The rung's ends at x = -1 and 1 hang 2 m below world points (the rung is at (0, 2, 0)).
for _, x in ipairs({ -1, 1 }) do
    local rope = Scene.CreateEntity("Rope")
    rope:SetParent(rung)
    rope:AddComponent("Joint", { Type = "Distance", BodyEntity = rung, Anchor = { x, 0, 0 },
        ConnectedAnchor = { x, 4, 0 } })
end
```

- `BodyEntity` and `ConnectedEntity` accept an entity or its ID and read back as the ID.
- Setting limits, limit springs, motor fields, break thresholds or `EnableCollision` updates the joint in
  place, so it is fine to do every frame. Changing `Type`, an entity, an anchor, an axis or `UseLimits`
  rebuilds it from the current poses.
- Motors (`MotorMode` `Velocity` or `Position`, `MotorTarget` in degrees(/s) or meters(/s),
  `MotorMaxForce`) work on hinges and sliders. Hinge position targets are clamped to [-180, 180].
- `HasJoint()` and `GetJointPosition()` are called on the entity that holds the `Joint`.
  `GetJointPosition()` is the body's position relative to the connected one: the hinge angle around
  `Axis` (right-handed, wrapping at ±180) or the slider offset along `Axis`; nil for other types.
- A joint whose force or torque exceeds `BreakForce`/`BreakTorque` (0 = unbreakable) is removed with its
  component. Both bodies get `OnJointBreak(other)` with the body on the other side, and so does a
  separate joint entity (with the connected entity). The force and torque include limit and motor effort,
  so a motor stalled against an obstacle can break its joint. Point and distance joints hold no torque:
  `BreakTorque` does not apply to them. Each break is logged with the measured force and torque, which
  helps tune thresholds.
- Destroying either body removes the constraint without calling `OnJointBreak`; `HasJoint()` turns false
  while the component stays.
- A joint whose body (or connected body) does not exist yet is built as soon as it does. Warnings about
  a joint are logged once per distinct problem, not on every update.
- Jointed bodies do not collide with each other unless `EnableCollision` is set.
- Joints inside a prefab or a duplicated hierarchy connect the new copies, and joint entities move the
  copied body.

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
