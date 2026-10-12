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
function Player:OnCollisionEnd(other, contact) end
function Player:OnTriggerEnter(other, contact) end  -- either body is a trigger (RigidBody.IsTrigger)
function Player:OnTriggerExit(other, contact) end
function Player:OnJointBreak(other) end        -- joint exceeded BreakForce/BreakTorque; other is the far body, nil for the world

return Player
```

- `self.Entity` is the entity the instance belongs to; property values are fields (`self.Speed`).
  Instances also have the engine methods `self:StartCoroutine`, `self:StopCoroutine` and
  `self:StopAllCoroutines` (see "Timers and coroutines"); a class can still define its own method of the
  same name, and class inheritance through metatables keeps working.
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
- `OnTriggerEnter` gets the same table for where the bodies started to overlap (`Impulse` is 0: triggers
  push nothing). `OnCollisionEnd` and `OnTriggerExit` get where the bodies parted: `Point` midway between
  their closest points right after they separated and `Normal` pointing away from `other` (the way this
  entity left), `Impulse` 0. Their `contact` is nil when either body is gone (destroyed, or rebuilt after a
  `RigidBody` or collider change) or they are already far apart (e.g. one was teleported away).
- Friction and restitution of a contact are combined from both bodies by `RigidBody.FrictionCombine` and
  `RestitutionCombine`: `Default`, `GeometricMean`, `Average`, `Min`, `Multiply` or `Max`. When the bodies
  differ, the mode later in that list wins (so `Default` defers to the other body); when both are
  `Default`, friction uses the geometric mean and restitution the larger value. `Friction` below 0 is
  treated as 0 and `Restitution` is clamped to 0..1 (with a warning). A collider with `OverrideMaterial`
  set (every collider type has it) touches with its own `Friction` and `Restitution` instead of the
  body's, combined by the body's modes: one body can have an icy runner and a grippy seat. Characters
  ignore collider materials.
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
Random numbers are deterministic: each play session starts from the project's `RandomSeed`
(`project.set randomSeed`, applied from the next play; `project.info` shows it, by default 9067031) and
produces the same sequence on every platform. Lua's `math.random`/`math.randomseed` use the same generator
(standard Lua seeds them from the clock), so replays and automated tests reproduce exactly.
`math.randomseed(n)` and `Math.Seed(n)` take an integer or an integral float (seeds are 32-bit: negative
integers and integers of 2^32 and up use their low 32 bits); `math.randomseed()` (or `nil`) returns to the
seed the session started with. Replays store that seed, and `replay.run` refuses to run with a different one.

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
| `e:PlayParticles()`, `e:StopParticles([clear])`, `e:EmitParticles(count)` | The entity's ParticleEmitter (see Particles; `clear` is a boolean). Errors without a ParticleEmitter |
| `e:IsEmittingParticles()`, `e:GetParticleCount()` | Whether the ParticleEmitter is playing, and its live particles (false/0 without one) |
| `e:GetScript()` | The entity's script instance (call its functions, read its fields) |
| `e:Destroy()` | Destroy at the end of the frame (with children) |

Component names and fields are listed by `basalt component.types` (and in Docs/Components.md).
Vectors in component tables are arrays (`{1, 2, 3}`); rotations are Euler degrees.

## Scene

`Scene.CreateEntity([name])`, `Scene.FindEntityByName(name)`, `Scene.FindEntitiesByName(name)`,
`Scene.GetEntityByID(id)`, `Scene.GetEntitiesWith(componentName)`, `Scene.Destroy(entity)`,
`Scene.Instantiate(prefabPath [, position [, parent]])` (scripts in the prefab start immediately),
`Scene.GetPrimaryCamera()`, `Scene.GetEntityCount()`, `Scene.GetName()`, `Scene.GetPath()` (the
project-relative scene file, or nil for a scene never saved).

### Switching scenes

`Scene.Load(path)` replaces the playing scene with the scene file `path` (a project-relative `.bscene`
inside the project; absolute paths and `..` escapes are rejected, stricter than `Scene.Instantiate`) at
the **end of the current frame**:

1. Every entity that is not persistent is destroyed (its `OnDestroy` runs).
2. The file's settings (renderer, physics) replace the old ones; settings the file leaves out take their
   defaults.
3. The file's entities start as on play start: physics bodies exist first, `PlayOnStart` audio and
   particles start, and every new script instance exists before any `OnCreate` runs.

The file is read and checked right away, so a missing or invalid file raises an error from `Scene.Load`
and nothing changes. Calling it again in the same frame replaces the earlier request. Elapsed time
(`Time.GetElapsed`) and the frame count keep running, and the Lua state is the same: globals, the session's
timers and event handlers, and `Storage` carry over.

`Scene.DontDestroyOnLoad(entity)` keeps a **root** entity and its whole tree (anything else is an error)
through every load for the rest of the play session; its instance keeps its fields, timers, coroutines and
event handlers. An entity of the loaded file whose ID is already present (typically the same manager placed
in every level, so each level also plays on its own) is skipped together with its children, so there is
only ever one. Stopping play (or the editor's Stop button) returns to the scene play started from.

```lua
function GameManager:OnCreate()
	Scene.DontDestroyOnLoad(self.Entity)
	Events.On("LevelComplete", function(next) Scene.Load(next) end)
end
```

## Timers and coroutines

```lua
function Bomb:OnCreate()
	self.Fuse = Timer.After(3, function() self:Explode() end)         -- once, after 3 s
	Timer.Every(0.5, function(timer) self:Blink() end, 6)             -- every 0.5 s, 6 times
	self:StartCoroutine(function(height)
		self.Entity.Translation = Vec3(0, height, 0)
		Wait(1.5)                                                     -- seconds
		WaitFrames(2)                                                 -- frames
		Log.Info("landed")
	end, 10)
end
function Bomb:Defuse() self.Fuse:Cancel() end
```

- `Timer.After(seconds, fn)` calls `fn(handle)` once; `Timer.Every(seconds, fn [, count])` calls it every
  `seconds` (> 0), `count` times or until cancelled. Both return a `TaskHandle`.
- `self:StartCoroutine(fn, ...)` runs `fn(...)` as a coroutine **right away, up to its first wait**, and
  returns a `TaskHandle`. An error before that first wait is raised from `StartCoroutine`. Inside it,
  `Wait(seconds)` (>= 0) and `WaitFrames(n)` (an integer >= 1) suspend it, and a plain `coroutine.yield()`
  waits one frame. They only work in a coroutine started this way (not in one made with `coroutine.create`
  or `coroutine.wrap`, and not in a function the engine calls from C, such as an event handler).
- `handle:Cancel()` stops a timer or coroutine (returns whether it was still active);
  `handle:IsActive()`. `self:StopCoroutine(handle)` is the same as `handle:Cancel()`;
  `self:StopAllCoroutines()` stops the instance's coroutines but not its timers. A coroutine that stops
  itself ends at its next wait.
- Time is game time: nothing runs while the scene is paused. Due timers and coroutines run once per frame
  after every `OnUpdate` (before physics), in the order they were created, and always at the earliest on
  the next frame (a 0 s delay waits one frame). A repeating timer fires at most once per frame; periods
  missed in one long frame are skipped.
- **Ownership:** a timer, coroutine or event handler belongs to the instance whose code created it (any
  of its callbacks, timers, coroutines or handlers). It ends when that instance is destroyed (or its
  Script removed) and when the instance fails: an error in a timer, coroutine or handler is reported like
  any script error and disables its instance. Creating one from `OnDestroy` (it could never run) raises
  an error. Ones created by console code (`lua.exec`, the editor console) belong to the play session.
  Play stopping ends them all.

## Events

```lua
self.OnHit = Events.On("PlayerHit", function(damage, source) self.Health = self.Health - damage end)
Events.Once("LevelLoaded", function(name) Log.Info("entered " .. name) end)
Events.Emit("PlayerHit", 10, self.Entity)   -- returns how many handlers ran
self.OnHit:Cancel()
```

- `Events.On(name, fn)` / `Events.Once(name, fn)` subscribe (a `Once` handler ends before its first call)
  and return an `EventHandle` with `Cancel()` and `IsActive()`. Names are non-empty strings; any values
  can be passed (they are not copied).
- `Events.Emit(name, ...)` is synchronous: the handlers subscribed when it starts run in subscription
  order before it returns, and it returns how many ran. Handlers subscribed during an emit wait for the
  next one; handlers cancelled before their turn are skipped, as are handlers of an instance that is being
  destroyed. An error in a handler does not reach the emitter (it is reported and disables the handler's
  instance). Emits nested more than 32 levels deep (a handler that keeps re-emitting) raise an error.
- Handlers follow the ownership rules of timers above and are all cleared when play stops.

## Save data

`Storage` is a key-value store that persists between play sessions and game launches:

```lua
local best = Storage.Get("BestScore", 0)
if score > best then
	Storage.Set("BestScore", score)
	local ok, err = Storage.Save()
	if not ok then Log.Warn("could not save: " .. err) end
end
```

- `Storage.Get(key [, default])`, `Storage.Set(key, value)` (`nil` deletes), `Storage.Has(key)`,
  `Storage.Delete(key)` (returns whether it existed), `Storage.Clear()`, `Storage.GetKeys()` (sorted).
  Keys are non-empty strings. Values are numbers, strings, booleans and tables of them, stored as JSON:
  `Vec2/3/4` come back as arrays (`{x, y, z}`), an `Entity` as its numeric ID. Functions are an error.
- Changes are kept in memory until `Storage.Save()`, which writes them atomically; `Storage.Load()`
  re-reads the file, dropping unsaved changes. Both return `true`, or `false` and the reason. Unsaved
  changes are lost when play stops.
- The file is `SaveData.json` in a per-game directory named after the project: `%APPDATA%\<Project>`
  (Windows), `~/Library/Application Support/<Project>` (macOS), `$XDG_DATA_HOME/<Project>` or
  `~/.local/share/<Project>` (Linux). The environment variable `BASALT_USER_DATA_DIR` replaces the
  platform directory (tests and tools use it). Scripts never choose a path, so the file-system sandbox
  stays intact. The editor's play mode and the exported game share the file.
- A save file that cannot be read (edited by hand, damaged) is reported as a script error, moved to
  `SaveData.corrupt.json` so it can be recovered, and the game continues with empty data.

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

Colliders are sized by the entity's world scale. Changing the scale during play (`e.Scale`, or a parent's)
resizes the body's shape at the next physics step. Its velocity and contacts carry on, so animating the
scale of a trigger or a body does not repeat its enter/begin callbacks; its joints are rebuilt from the
current poses, with their anchors at the new scale. Characters are resized the same way. An entity whose
scale was zero gets its body once the scale is valid again.

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
nothing and warns. `MotorMaxForce` caps the translation motors' force (N) and `MotorMaxTorque` the rotation
motors' torque (N·m); a `MotorMaxTorque` of 0 (the default) or below uses `MotorMaxForce` for both, as joints
saved before it existed did (a negative one also warns). A `Position` motor pulls with a
spring of `MotorSpringFrequency` Hz (default 2, must be above 0; higher is stiffer, up to that cap) and
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
    AngularMotorTarget = pose, MotorSpringFrequency = 8, MotorMaxTorque = 200 })  -- cheap: in place
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
  `MotorMaxForce` for sliders, `MotorMaxTorque` for hinges, which falls back to `MotorMaxForce` while it is 0 or below)
  work on hinges and sliders; six-DOF joints use the per-axis motors above. Hinge position
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
- Characters collide with each other's full shapes (on layers that collide) and a moving character pushes
  another with its velocity, whatever its `MaxStrength` (that only limits how hard it pushes dynamic
  bodies).
- The character enters triggers like any body (`OnTriggerEnter`/`OnTriggerExit`). Dynamic bodies that run
  into it raise collision callbacks (kinematic ones, such as other characters, do not); the character walking into something does not (it stops just short of it),
  so use a trigger or a query to detect what it touches.
- Changing `SlopeLimit`, `StepHeight`, `MaxStrength`, `Mass` or `GravityFactor` applies in place; a new
  `Layer` or collider rebuilds the character, which keeps its velocity and ground. Out-of-range values are
  clamped with one warning; `SlopeLimit` is at least 1 degree.

## Particles

A `ParticleEmitter` component emits camera-facing billboards while the scene plays (and while the editor
simulates). Particles are simulated on the CPU and drawn unlit, sorted with transparent meshes; their
colors are linear HDR, so values above 1 stay bright after exposure and tonemapping (there is no bloom).
Use `Blend = "Additive"` for sparks and fire, `"Alpha"` for smoke. `Texture` is a project-relative image
(its alpha is coverage); without one particles are soft discs.

- Each emission cycle lasts `Duration` seconds: it starts with `BurstCount` particles and emits `Rate` per
  second. `Looping` emitters start a new cycle (and burst) at its end; the others stop emitting.
  `PlayOnStart` emitters play from the start, the others wait for `e:PlayParticles()`.
- `e:PlayParticles()` starts a new cycle unless the emitter is playing. `e:StopParticles()` stops emitting
  and lets live particles finish; `e:StopParticles(true)` also removes them.
- `e:EmitParticles(count)` emits up to `count` (0..10000) particles at once, playing or not, and returns
  how many fit under `MaxParticles`. They use the emitter's shape, speeds, lifetimes, colors and sizes, and
  are first drawn where they were born, like burst and rate particles.
- Shapes are in the emitter's local space: `Point` and `Sphere` (radius `ShapeRadius`) emit in every
  direction, `Box` (half-extents `BoxExtents`) along local +Y, `Cone` (base radius `ShapeRadius`, half-angle
  `ConeAngle` degrees) within that angle of +Y. Speed (m/s) is picked between `SpeedMin` and `SpeedMax`
  along that direction (negative speeds emit the other way), plus `Velocity` (m/s along the emitter's
  rotated axes).
  The shape scales with the emitter; speeds do not. `GravityScale` scales the scene gravity; `Drag` is a
  rate (1/s): speed decays by about e^-Drag per second.
- `SimulationSpace = "World"` leaves particles where they were emitted when the emitter moves; `"Local"`
  carries (and turns) them along.
- Color and size go from `StartColor`/`StartSize` to `EndColor`/`EndSize` over each particle's life (picked
  between `LifetimeMin` and `LifetimeMax` seconds). Min/max pairs may be in either order.
- Limits: `Duration` and lifetimes 0.001..1e5 s, `MaxParticles` 1..10000, `BurstCount` 0..10000, `Rate`
  0..1e6 per second, `ConeAngle` 0..180, opacity 0..1; speeds, `Velocity` and `GravityScale` between -1e4
  and 1e4; `Drag`, sizes, `ShapeRadius`, `BoxExtents` and color channels 0..1e4. Scene and prefab files,
  `SetComponent` and `component.set` reject values outside them, naming the field and value.

Emitters are deterministic: each one draws from its own random stream, seeded from the session's seed
(the project's `RandomSeed`) and the order emitters appear in (scene emitters by entity ID, then the ones
added during play), so the same session plays the same particles on the same platform (shape sampling uses
`sin`/`cos`/`cbrt`, which differ slightly between C libraries), and particles never change what `Math.Random`
returns. Particles are runtime state only: they are not saved and not part of `scene.hash`.

```lua
function Explosion:OnCreate()
    self.Entity:EmitParticles(200)          -- one-off burst; does not start the emitter (Rate/BurstCount do not apply)
end

function Torch:OnUpdate(dt)
    if Input.IsKeyPressed("F") then
        if self.Entity:IsEmittingParticles() then self.Entity:StopParticles() else self.Entity:PlayParticles() end
    end
end
```

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
