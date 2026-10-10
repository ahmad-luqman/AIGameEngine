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
function Player:OnCollisionBegin(other, contact) end  -- other: Entity; contact: see below
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
- `contact` in `OnCollisionBegin` is a table describing where and how hard the bodies first touched:
  `Point` (Vec3, world space, midway between the surfaces), `Normal` (Vec3, unit, pointing away from
  `other`: a body landing on the ground gets `(0, 1, 0)`) and `Impulse` (number, N·s: the estimated impulse
  that stops the bodies closing in, bounce included; 0 when they touched without closing in). When several parts of the two
  bodies touch in the same step, the strongest is reported. Use it for impact sounds, damage or "landed" checks.
- Friction and restitution of a contact are combined from both bodies by `RigidBody.FrictionCombine` and
  `RestitutionCombine`: `Default`, `GeometricMean`, `Average`, `Min`, `Multiply` or `Max`. When the bodies
  differ, the mode later in that list wins (so `Default` defers to the other body); when both are
  `Default`, friction uses the geometric mean and restitution the larger value. `Friction` below 0 is
  treated as 0 and `Restitution` is clamped to 0..1 (with a warning).
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

Physics is deterministic across platforms too: the same scene and inputs reach the same state (and
`scene.hash`) on Windows, Linux and macOS, in Debug and Release (`Tests/Data/Determinism` checks it on
every CI run). That holds as long as nothing feeding the state goes through the platform's math library,
whose trigonometric and exponential functions can differ in the last bit between platforms:
- Lua's `math.sin`/`cos`/`tan`/`asin`/`acos`/`atan`/`exp`/`log` and `^`;
- Euler angles in either direction: `e.EulerAngles`, `Quat.FromEuler`, `Quat.ToEuler`,
  `SetComponent("Transform", { Rotation = ... })`, and scene files whose rotations are not all zero;
- `Quat.AngleAxis`, `Quat.Slerp`, `GetJointRotation()`, and six-DOF `Position` motors on rotation axes
  (their target is an Euler orientation).

`Math.Radians` and `Math.Degrees` are plain multiplications, and physics-produced motion is portable.
Entities spawned during play get random UUIDs, and some engine orders (joint removal, contact callbacks,
the creation order of new script instances) still follow UUID values, so a game that spawns jointed or
contact-driven entities at runtime is not yet reproducible run to run.

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
| `e:SetLinearVelocity(v)`, `e:GetLinearVelocity()`, `e:SetAngularVelocity(v)`, `e:GetAngularVelocity()` | Physics velocities (`GetLinearVelocity` also reports how fast a character moved in the last step) |
| `e:Move(velocity)` | Walk a CharacterController with this velocity (m/s) until the next call; an upward part jumps from the ground (see Character controllers). Errors without a CharacterController |
| `e:IsGrounded()` | The character stands on ground no steeper than its `SlopeLimit` (false without a character) |
| `e:GetGroundNormal()` | Normal of the ground the character touches (walkable or too steep); nil in the air or without a character |
| `e:GetJointPosition()` | Hinge angle (degrees) or slider offset (meters) of the Joint this entity holds, from its rest pose; nil otherwise |
| `e:GetJointRotation()` | Rotation (Euler degrees, like `Rotation`) of a cone or six-DOF Joint this entity holds, in the joint frame from its rest pose; nil otherwise |
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

Each rigid body is on one named layer (`RigidBody.Layer`, default `"Default"`; a character uses
`CharacterController.Layer`). The project defines up to
32 layers and which pairs of layers collide (`project.set` with `physicsLayers`, stored in
`Project.bproject`). The matrix is read when play starts. A layer name the project does not define falls
back to `Default` with a warning; `project.set` and `scene.info` list such bodies and characters as
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
min == max locks it; X rotation (twist) lies within ±180 and the Y and Z rotations form a symmetric swing
cone whose half angles are `AngularLimitMax.y`/`.z`. A swing minimum of 0 means -max, so a one-sided
swing range is not possible (other minimums are mirrored with a warning). Jolt treats an angle limit within
0.5° of 0 as locked and one within 0.5° of 180 as free. `FreeLinearAxes` (`{ x, y, z }` booleans) frees
translation axes outright, with or without `UseLimits` (their linear limits are then ignored). Without
`UseLimits`, translation is locked and rotation free. A limit spring softens only limited translation axes;
locked and free axes and rotation limits stay rigid (Jolt has no soft rotation limits).

**Powered ragdolls.** Six-DOF joints have a motor per axis of the joint frame: `LinearMotorMode` and
`AngularMotorMode` are `{ x, y, z }` lists of `"Off"`, `"Velocity"` or `"Position"`, and each axis reads
its component of `LinearMotorTarget` (m/s, or meters from the rest pose) or `AngularMotorTarget` (degrees/s
around the axis; a velocity axis turns with the joint's own body, so it drifts from the connected body's
frame as the joint bends). The `Position` rotation axes together drive toward one target orientation, given as Euler
angles in degrees relative to the rest pose (the same form `GetJointRotation()` returns, so a recorded pose
plays back as is); position targets beyond the limits are clamped to them. A motor on a locked axis does
nothing and warns. `MotorMaxForce` caps both the force and the torque, and a `Position` motor pulls with a
spring of `MotorSpringFrequency` Hz (default 2, must be above 0; higher is stiffer, up to `MotorMaxForce`) and
`MotorSpringDamping` (1 = critically damped), which also apply to hinge and slider motors. The cone joint
has no motor; use a six-DOF joint with swing limits instead.

```lua
-- A door that swings open on a motor (hinge on the door's left edge, around Y).
door:AddComponent("Joint", { Type = "Hinge", ConnectedEntity = frame, Anchor = { -0.5, 0, 0 },
    UseLimits = true, LimitMin = 0, LimitMax = 100 })
door:SetComponent("Joint", { MotorMode = "Position", MotorTarget = 90 })  -- cheap: updates the live joint

-- A shoulder: twist limited to ±30 degrees, a 70/40 degree swing cone.
arm:AddComponent("Joint", { Type = "SixDOF", ConnectedEntity = torso, Anchor = { 0, 0.5, 0 },
    Axis = { 0, -1, 0 }, SecondaryAxis = { 1, 0, 0 }, UseLimits = true,
    AngularLimitMin = { -30, -70, -40 }, AngularLimitMax = { 30, 70, 40 } })

-- Powered: every frame, pull the shoulder toward the animated pose (Euler degrees from the rest pose).
arm:SetComponent("Joint", { AngularMotorMode = { "Position", "Position", "Position" },
    AngularMotorTarget = pose, MotorSpringFrequency = 8, MotorMaxForce = 200 })  -- cheap: in place
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
- Fields a joint does not use (for its `Type`, or without `UseLimits`) are ignored, with one warning
  naming them when they are set away from their defaults; the editor shows only the fields a joint uses
  (plus any such ignored ones, marked). Free cone and six-DOF joints (without `UseLimits` or, for six-DOF,
  a rotation motor) and point and distance joints hold no torque, so `BreakTorque` does not apply to them.
- Setting limits, limit springs, motor fields, break thresholds or `EnableCollision` updates the joint in
  place (per-axis motors and `FreeLinearAxes` included), so it is fine to do every frame. Changing `Type`, an entity, an anchor, an axis or `UseLimits`
  rebuilds it from the current poses.
- Motors (`MotorMode` `Velocity` or `Position`, `MotorTarget` in degrees(/s) or meters(/s),
  `MotorMaxForce`) work on hinges and sliders; six-DOF joints use the per-axis motors above. Hinge position
  targets are clamped to [-180, 180].
- `HasJoint()`, `GetJointPosition()` and `GetJointRotation()` are called on the entity that holds the
  `Joint`. `GetJointPosition()` is the body's position relative to the connected one: the hinge angle
  around `Axis` (right-handed, wrapping at ±180) or the slider offset along `Axis`; nil for other types.
  `GetJointRotation()` is the body's rotation relative to the connected one for cone and six-DOF joints,
  in the joint frame (X = `Axis`; Y = `SecondaryAxis` for six-DOF, an arbitrary perpendicular for a cone)
  as Euler degrees; nil for other types.
- A joint whose force or torque exceeds `BreakForce`/`BreakTorque` (0 = unbreakable) is removed with its
  component. Each body gets `OnJointBreak(other)` with the body on the other side (nil for the world), and
  a separate joint entity that is neither body gets it once too (with the connected entity). The force and
  torque include limit and motor effort, so a motor stalled against an obstacle can break its joint. Each
  break is logged with the measured force and torque, which helps tune thresholds.
- Destroying a body removes the constraint without calling `OnJointBreak`. When the `Joint` is held by
  another entity that survives (not the body or one of its children), `HasJoint()` turns false while the
  component stays, and the joint is rebuilt if a body with that ID appears again.
- A joint whose body (or connected body) does not exist yet is built as soon as it does. Warnings about
  a joint are logged once per distinct problem, not on every update.
- Jointed bodies do not collide with each other unless `EnableCollision` is set. Changing that (or which
  bodies a joint connects) applies at once, even to bodies resting against each other.
- Joints inside a prefab or a duplicated hierarchy connect the new copies, and joint entities move the
  copied body.

### Character controllers

A `CharacterController` with a collider (a `CapsuleCollider` fits most characters; the editor's
Create > Character adds both; a `MeshCollider` uses its convex hull) makes the entity a game character instead of a rigid body: it slides along
walls, walks up slopes up to `SlopeLimit` degrees and steps up to `StepHeight` meters, rides moving
platforms, and pushes dynamic bodies with at most `MaxStrength` newtons. A `RigidBody` on the same entity
is ignored. Other bodies, queries and triggers see the character as a kinematic body on its `Layer`,
shaped like its colliders at 90% size (so that walking up to something does not shove it): a ray that
grazes the outer edge of the collider, or a thin trigger at floor level, can miss it.

```lua
function Player:OnUpdate(dt)
    local input = Vec3(0, 0, 0)
    if Input.IsKeyDown("D") then input.x = input.x + 1 end
    if Input.IsKeyDown("A") then input.x = input.x - 1 end
    local velocity = input * self.Speed
    -- Held, not pressed: Move is used at the next physics step, and on a fast frame the next frame's
    -- Move can replace it before any step runs.
    if Input.IsKeyDown("Space") then
        velocity.y = self.JumpSpeed
    end
    self.Entity:Move(velocity)
end
```

- `Move(velocity)` holds until the next call, so call it every frame. With gravity (scene gravity times
  `GravityFactor`), its upward part only counts on walkable ground, where it jumps, again on every landing
  while it is held. In the air gravity drives the vertical speed and `Move` steers sideways. A ceiling
  stops a jump at once. With `GravityFactor = 0` the velocity applies in full (flying, ladders). A
  velocity that is not finite is a script error.
- `AddForce`, `AddImpulse`, `AddTorque`, `SetLinearVelocity` and `SetAngularVelocity` do nothing on a
  character (one warning); knock it back through `Move`. `GetLinearVelocity()` returns how fast it actually
  moved over the last step.
- Standing still on a walkable slope does not slide; ground steeper than `SlopeLimit` does not count as
  standing (`IsGrounded()` is false), and the character slides down it. `StepHeight` also keeps the
  character on the ground when walking down stairs and slopes; 0 turns both off. A capsule's round bottom
  rolls over low edges even without `StepHeight`.
- Moving the entity's transform teleports the character; its rotation follows the entity's rotation.
- The character enters triggers like any body (`OnTriggerEnter`/`OnTriggerExit`). Dynamic bodies that run
  into it raise collision callbacks (kinematic ones, such as other characters, do not); the character walking into something does not (it stops just short of it),
  so use a trigger or a query to detect what it touches.
- Changing `SlopeLimit`, `StepHeight`, `MaxStrength`, `Mass` or `GravityFactor` applies in place; a new
  `Layer` or collider rebuilds the character, which keeps its velocity and ground. Out-of-range values are
  clamped with one warning; `SlopeLimit` is at least 1 degree.

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
