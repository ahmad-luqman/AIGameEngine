# Basalt Lua Scripting API

Gameplay code is written in Lua 5.4. A script is attached to an entity with a `Script` component
(`{"Script": "Assets/Scripts/Player.lua"}`). The feature test (`Tests/Data/FeatureTest/Assets/Scripts/FeatureTest.lua`)
calls every function on this page (checking its effect wherever it can be observed headlessly) and is the
best executable reference.

**Editor support.** [`basalt.d.lua`](basalt.d.lua) declares the whole API (and every component's fields) for
[lua-language-server](https://luals.github.io/): completion, hover docs and type checks in VS Code, Neovim and other
editors. The repository's `.luarc.json` loads it; in a game project add the file to `workspace.library` and annotate a
script class with `---@class Player: BasaltScript` (new scripts from the editor's content browser start that way). The
`LuaTypeDefinitions` unit test fails when a binding is added without a declaration, and `scripts/generate_docs.py`
regenerates the component section.

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
function Player:OnAnimationEvent(name, clip) end  -- the entity's Animator crossed an Events marker (see Skeletal animation)
function Player:OnGamepadConnected(index) end  -- a gamepad is connected (see "Input"); likewise OnGamepadDisconnected
function Player:OnReload() end                 -- the script was hot reloaded (see "Hot reload")

return Player
```

- `self.Entity` is the entity the instance belongs to; property values are fields (`self.Speed`).
  Instances also have the engine methods `self:StartCoroutine`, `self:StopCoroutine` and
  `self:StopAllCoroutines` (see "Timers and coroutines"); a class can still define its own method of the
  same name, and class inheritance through metatables keeps working.
- **Creation order:** when play starts, when a prefab is instantiated, and when a scene is loaded
  (`Scene.Load`), every new instance is created
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

### Hot reload

A script saved while the game runs (Play; not while simulating) is loaded again without stopping play: in the editor
as soon as the file settles on disk (View > Reload Changed Files Automatically, on by default), through automation
with `asset.write` or `asset.refresh` (which also reloads models and textures changed on disk).

- Every instance of the script moves onto the new class and **keeps its fields** (`self.Health`, counters,
  references to other entities). `Properties` the new version adds are set from the defaults or the component's
  overrides; a default that changed does not reach instances that already have the field. Then each started instance
  gets `OnReload()` (not `OnCreate`); one that had not started, or whose `OnCreate` raised an error, gets `OnCreate`.
  An instance whose metatable the script replaced itself (`setmetatable(self, ...)`) is left as it is.
- A changed module (`require`) is loaded again together with the modules that required it, and every script loads
  again so they pick up the new version; other modules keep their state. A module whose new version does not load
  keeps its previous one (cached, and what its top level started keeps running). Saving a module whose `require`
  failed retries the scripts that failed because of it. Replacing the global `package` table does not hide modules
  from hot reload.
- A version that does not load (a syntax error, an error in its top-level code, no class table) is reported and a
  script that was running **keeps its previous version**; fix it (or the module that broke it) and save again. An
  instance that a runtime error disabled runs again after a successful reload of its script, and entities whose
  script failed to load get their instances (with `OnCreate`) then.
- The script's top-level code runs again. Timers and event handlers it started (`Events.On` at the top level) end
  with the old version, so they never pile up; those a module's top level started belong to the module and end only
  when the module itself is loaded again. A failing top-level timer or handler ends alone.
- Not carried over: module-level `local` variables start afresh, and functions instances already handed to timers,
  coroutines or event handlers keep running the old code until they are created again (`OnReload` can restart them).
  A disabled instance's timers and handlers were cancelled when it failed.
- Deleting a script leaves its running version in place until play stops.

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
`Math.Sign(v)`, `Math.Seed(n)`, `Math.Random()` (in [0, 1)), `Math.Random(min, max)`, `Math.RandomInt(min, max)`.
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
Entities created during Play or Simulate (including prefab instances and duplicates) use a dedicated
UUID stream seeded from the session's `RandomSeed`. The same starting scene, seed and creation sequence
produce the same IDs, making UUID-ordered joint removal, ignored-pair activation, contact callbacks and
pending script creation reproducible. Lua and particle random draws do not consume this stream.
`Scene.Load` continues it; a new session resets it. Zero, live IDs and non-generated IDs reserved during
the session (authored, explicitly assigned, loaded, or present at session start) are skipped. Reservations
constrain the generator only and are released on stop. Generated IDs are not retained after destruction,
so an extremely unlikely repeated random value could reuse a retired generated ID without growing memory
for every spawn. Entities loaded from a scene keep their saved IDs; prefab instances receive new ones
from the stream. Explicitly loaded IDs can reuse retired IDs. Edit-mode creation still uses random IDs.

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
| `e:IsSleeping()` | Whether the body is asleep (see `RigidBody.AllowSleeping` below); false for static bodies, nil for entities without a body |
| `e:Move(velocity)` | Walk a CharacterController with this velocity (m/s) until the next call; an upward part jumps from the ground (see Character controllers). Errors without a CharacterController |
| `e:IsGrounded()` | The character stands on ground no steeper than its `SlopeLimit` (false without a character) |
| `e:GetGroundNormal()` | Normal of the ground the character touches (walkable or too steep); nil in the air or without a character |
| `e:GetJointPosition()` | Hinge angle (degrees) or slider offset (meters) of the Joint this entity holds, from its rest pose; nil otherwise |
| `e:GetJointRotation()` | Rotation (Euler degrees, like `Rotation`) of a cone or six-DOF Joint this entity holds, in the joint frame from its rest pose; nil otherwise |
| `e:HasJoint()` | Whether the Joint this entity holds is a live constraint (false before both bodies exist, when it is invalid, and after it broke) |
| `e:PlayAudio()`, `e:StopAudio()`, `e:IsAudioPlaying()` | The entity's AudioSource |
| `e:GetAudioDopplerPitch()` | The Doppler pitch factor of the AudioSource from the last update (the shift its next mix applies): 1 = unshifted, also while it is not playing or for a non-spatial source |
| `e:PlayParticles()`, `e:StopParticles([clear])`, `e:EmitParticles(count)` | The entity's ParticleEmitter (see Particles; `clear` is a boolean). Errors without a ParticleEmitter |
| `e:IsEmittingParticles()`, `e:GetParticleCount()` | Whether the ParticleEmitter is playing, and its live particles (false/0 without one) |
| `e:PlayAnimation([clip])`, `e:CrossFadeAnimation(clip, seconds)`, `e:BlendAnimations(first, second, weight)` | Animator playback and transitions; see Skeletal animation |
| `e:PauseAnimation([paused])`, `e:StopAnimation()`, `e:SetAnimationTime(seconds)` | Animator pause, stop and seek |
| `e:GetAnimationClips()`, `e:GetAnimationTime()`, `e:GetAnimationDuration()`, `e:IsAnimationPlaying()` | Animator queries |
| `e:PlayBlendSpace(space, x, [y], [fadeSeconds])`, `e:SetBlendSpaceParameter(x, [y])`, `e:GetBlendSpaceWeights()` | Animator blend spaces; see Blend spaces |
| `e:GetBoneTransform(joint)` | World position (Vec3), rotation (Quat) and scale (Vec3) of a joint of the Animator's current pose, or nil; see Bones |
| `e:GetScript()` | The entity's script instance (call its functions, read its fields) |
| `e:Destroy()` | Destroy at the end of the frame (with children) |

Component names and fields are listed by `basalt component.types` (and in Docs/Components.md).
Vectors in component tables are arrays (`{1, 2, 3}`); rotations are Euler degrees.

## Scene

`Scene.CreateEntity([name])`, `Scene.FindEntityByName(name)`, `Scene.FindEntitiesByName(name)`,
`Scene.GetEntityByID(id)`, `Scene.GetEntitiesWith(componentName)`, `Scene.Destroy(entity)`,
`Scene.Instantiate(prefabPath [, position [, parent]])` (scripts in the prefab start immediately),
`Scene.GetPrimaryCamera()`, `Scene.GetEntityCount()`, `Scene.GetName()`, `Scene.GetPath()` (the scene file
it was loaded from or last saved to: project-relative inside the project, otherwise absolute; nil for a
scene never loaded from or saved to a file).

### Switching scenes

`Scene.Load(path)` replaces the playing scene with the scene file `path` (a project-relative `.bscene`
inside the project; absolute paths and `..` escapes are rejected, stricter than `Scene.Instantiate`,
because a scene switch replaces the whole game) at the **end of the current frame**:

1. Every entity that is not persistent is destroyed (its `OnDestroy` runs). Entities that `OnDestroy`
   spawns are destroyed too; if they keep spawning more, the survivors are kept after 8 passes and a
   script error names them.
2. The file's settings (renderer, physics) replace the old ones; settings the file leaves out take their
   defaults (the debug view, a viewing aid, stays).
3. The file's entities start as on play start: physics bodies exist first, `PlayOnStart` audio and
   particles start, and every new script instance exists before any `OnCreate` runs. A script that fails
   to load is reported; the others still start.

The file is read and checked right away, so a missing or invalid file raises an error from `Scene.Load`
and nothing changes. Calling it again in the same frame replaces the earlier request; calling it while a
load is applied (from `OnDestroy` or the new scene's `OnCreate`, e.g. a splash screen moving on) loads at
the end of the next frame. Elapsed time (`Time.GetElapsed`) and the frame count keep running, and the Lua
state is the same: globals, `Storage`, everything owned by persistent entities, and the play session's own
timers and handlers (from console or top-level script code) carry over. Loading the current scene's file again restarts it.

`Scene.DontDestroyOnLoad(entity)` keeps a **root** entity and its whole tree (anything else is an error)
through every load for the rest of the play session; its instance keeps its fields, timers, coroutines and
event handlers. If it is later parented under an entity that is not persistent, a load first moves it back
to the root (keeping its place in the world). An entity of the loaded file whose ID is already present
(typically the same manager placed in every level, so each level also plays on its own) is skipped
together with its children in the file, so there is only ever one. Stopping play (or the editor's Stop
button) returns to the scene play started from.

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

- `Timer.After(seconds, fn)` (`seconds` >= 0) calls `fn(handle)` once; `Timer.Every(seconds, fn [, count])`
  calls it every `seconds` (> 0), `count` times or until cancelled. Both return a `TaskHandle`.
- `self:StartCoroutine(fn, ...)` runs `fn(...)` as a coroutine **right away, up to its first wait**, and
  returns a `TaskHandle`. An error before that first wait is raised from `StartCoroutine`. Inside it,
  `Wait(seconds)` (>= 0) and `WaitFrames(n)` (an integer >= 1) suspend it, and a plain `coroutine.yield()`
  waits one frame. They only work in a coroutine started this way (not in one made with `coroutine.create`
  or `coroutine.wrap`, and not in a function the engine calls from C, such as an event handler). A
  coroutine may start others.
- `handle:Cancel()` stops a timer or coroutine (returns whether it was still active); `handle:IsActive()`.
  `self:StopCoroutine(handle)` stops one of this instance's coroutines and returns whether it was still
  active (an error for an active timer or another instance's active coroutine, which `handle:Cancel()`
  stops); `self:StopAllCoroutines()` stops the instance's
  coroutines but not its timers. A coroutine that stops itself ends at its next wait. To-be-closed
  variables of a stopped coroutine are not closed (those of a coroutine that fails are).
- Time is game time: nothing runs while the scene is paused. Due timers and coroutines run once per frame
  after every `OnUpdate` (before physics), in the order they were created, and always at the earliest on
  the next frame (a 0 s delay waits one frame). A repeating timer fires at most once per frame; periods
  missed in one long frame are skipped.
- **Ownership:** a timer or event handler belongs to the instance whose callback is running when it is
  created (any of its callbacks, timers, coroutines or handlers). Calling another instance's method does
  not change that: a timer created in `other:Arm()` called from your `OnUpdate` is yours. A coroutine
  belongs to the instance it was started on. Whatever an instance owns ends when that instance is
  destroyed (or its Script removed) and when it fails: an error in a timer, coroutine or handler is
  reported like any script error (naming the owner) and disables its instance. Creating one from
  `OnDestroy` (it could never run) raises an error. Timers and handlers created by console code
  (`lua.exec`, the editor console) or by a script file's top-level code belong to the play session; a
  failing one is cancelled. Play stopping ends them all.

## Events

```lua
self.OnHit = Events.On("PlayerHit", function(damage, source) self.Health = self.Health - damage end)
Events.Once("LevelLoaded", function(name) Log.Info("entered " .. name) end)
Events.Emit("PlayerHit", 10, self.Entity)   -- returns how many handlers were called
self.OnHit:Cancel()
```

- `Events.On(name, fn)` / `Events.Once(name, fn)` subscribe (a `Once` handler ends before its first call)
  and return an `EventHandle` with `Cancel()` and `IsActive()`. Names are non-empty strings; any values
  can be passed (they are not copied).
- `Events.Emit(name, ...)` is synchronous: the handlers subscribed when it starts run in subscription
  order before it returns, and it returns how many were called. Handlers subscribed during an emit wait
  for the next one; handlers cancelled before their turn are skipped, as are handlers of an instance that
  is being destroyed or has failed. An error in a handler is reported and disables the handler's instance
  without reaching the emitter, unless a handler of the emitting instance itself failed: then `Emit`
  raises an error too, so the disabled instance's callback stops there (a handler that only destroys the
  emitter is fine). Emits nested more than 32 levels deep (a handler that
  keeps re-emitting) raise an error.
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
  `Vec2/3/4` come back as arrays (`{x, y, z}`), a `Quat` as Euler degrees, an `Entity` as its numeric ID.
  Tables must be sequences (1..n) or have only string keys. `Get` returns a copy: changing a returned
  table changes nothing until it is `Set` again. `Set` raises an error for what could not be saved
  exactly: functions, NaN or infinite numbers, and strings (or keys) that are not valid UTF-8 text.
- Changes are kept in memory until `Storage.Save()`, which writes them atomically; `Storage.Load()`
  re-reads the file, dropping unsaved changes. Both return `true`, or `false` and the reason (also
  logged), including when there is nowhere to save (no user data directory); the other functions raise an
  error then. Unsaved changes are lost when play stops.
- The file is `SaveData.json` in a per-game directory named after the project (characters that are not
  safe in a file name become `_`, and then a short hash of the name is appended): `%APPDATA%\<Project>`
  (Windows), `~/Library/Application Support/<Project>` (macOS), `$XDG_DATA_HOME/<Project>` or
  `~/.local/share/<Project>` (Linux). The environment variable `BASALT_USER_DATA_DIR` replaces the
  platform directory (tests and tools use it; set to a relative path, `Storage` raises an error rather than
  falling back). Scripts never choose a path, so the file-system sandbox stays intact. The editor's play
  mode and the exported game share the file.
- The player's data is never destroyed:
  - a file that is damaged (hand-edited, truncated, larger than 16 MB) is moved to
    `SaveData.corrupt.json` (then `SaveData.corrupt.2.json`, ...) and the game continues with empty data
    (if it cannot be moved, it is left alone and `Storage.Save()` refuses to overwrite it);
  - a file written by a newer version of the game, or one that cannot be read (locked, no permission), is
    left alone, and `Storage.Save()` refuses to overwrite it until a `Storage.Load()` succeeds.

  The first `Storage` call loads the file; a problem with that load is reported as a script error, unless
  that call is `Storage.Load()`, which returns it instead.

## Input

Key names: `A`-`Z`, `D0`-`D9`, `Space`, `Enter`, `Escape`, `Tab`, `Backspace`, `Left`, `Right`, `Up`,
`Down`, `LeftShift`, `LeftControl`, `LeftAlt`, `F1`-`F12`, `KP0`-`KP9`, ... (case-insensitive).
Mouse buttons: `Left`, `Right`, `Middle`, `Button3`-`Button7`.

`Input.IsKeyDown(key)`, `Input.IsKeyPressed(key)` (this frame only), `Input.IsKeyReleased(key)`,
`Input.IsMouseButtonDown/Pressed/Released(button)`, `Input.GetMousePosition()`, `Input.GetMouseDelta()`,
`Input.GetMouseScroll()` (all Vec2).

**Coordinates.** The mouse position is in the game view: 0,0 is its top-left corner and `Input.GetViewSize()`
(Vec2) its bottom-right. In the runtime that is the window (window units, which differ from pixels on high-DPI
displays); in the editor's Game view it is the game resolution, whatever size the view is drawn at, so a script
reads the same coordinates in both. The delta is in the same units. Headless runs use a 1920x1080 view.

**What the game receives.** Scripts (and the game UI's widgets) see only the input the host gives the game. The runtime and headless runs give it everything. The editor gives it the keyboard and gamepads while
the Game view has focus, and the mouse buttons, movement and scroll while the Game view has focus and the mouse is
over its image (or the game has locked the cursor). The mouse position, gamepad connections and names (and
`OnGamepadConnected`/`OnGamepadDisconnected`) are always reported. Losing that input reads like letting go of
everything held (`IsKeyReleased` is true on that frame); a key or button held when it comes back stays up for the
game until it is let go, and the click that focuses the Game view is not the game's. This applies to automation
too: in the editor, injected keys and gamepads reach scripts only while the Game view has focus
(`editor.game_view {"focus": true}`), and injected mouse buttons, movement and scroll also need the real mouse
cursor over the Game view's image, as a real keyboard and mouse would.

**Cursor.** `Input.SetCursorMode(mode)` asks for `"Normal"` (visible), `"Hidden"` (hidden over the game, free to
leave) or `"Locked"` (hidden and held in the window: only the delta means anything, as first-person cameras want;
the platform's raw mouse motion is used where available). `Input.GetCursorMode()` returns the mode asked for. Play
starts and stops with `"Normal"` (`Scene.Load` keeps it). In the editor the mode applies while the game runs and the
Game view has focus (`"Hidden"` hides the cursor only over the Game view's image); Escape gives the cursor back to
the editor until the Game view is clicked again, and neither that Escape nor that click reaches the game (a hidden
cursor keeps Escape for the game). While the game asks for a locked cursor, the mouse position is meaningless and UI
widgets get no pointer, in every host. Headless runs change no cursor.

### Input actions

Actions name what the player does (`"Jump"`, `"MoveX"`) rather than which control does it, so controls can be
rebound without changing scripts. The project declares them in `Input.binput`, next to `Project.bproject` (edit it
in Project Settings > Input Actions, or with `project.set inputActions`; it is exported with the game):

```json
{
	"Format": "BasaltInput",
	"Version": 1,
	"Actions": [
		{ "Name": "Jump", "Type": "Button", "Bindings": [{ "Key": "Space" }, { "GamepadButton": "A" }] },
		{ "Name": "Fire", "Bindings": [{ "MouseButton": "Left" }, { "GamepadAxis": "RightTrigger" }] },
		{ "Name": "MoveX", "Type": "Axis", "Bindings": [{ "Key": "D" }, { "Key": "A", "Scale": -1 },
			{ "GamepadAxis": "LeftX", "DeadZone": 0.25 }, { "GamepadButton": "DPadRight", "Gamepad": 1 }] }
	]
}
```

- A binding names one control: `Key`, `MouseButton`, `GamepadButton` or `GamepadAxis`. Keys and buttons read 1
  while held. A gamepad axis reads its value past its `DeadZone` (0 to 0.95, default 0.2), rescaled so a stick
  pushed all the way still reads 1. `Scale` (-1 to 1, default 1) multiplies the value: `-1` makes a key the negative
  half of an axis, or a stick pushed up (-Y) press a button. `Gamepad` (0-15) reads one gamepad (local
  multiplayer); without it, every gamepad counts: for a `Button`, the one pushing it hardest; for an `Axis`, the
  stick pushed furthest either way. Control names are
  case-insensitive, like key names in scripts; `Type` is exactly `"Button"` or `"Axis"`, like component enums.
- A `Button` action (the default type) is as strong as its strongest binding, from 0 to 1 (a trigger gives
  values in between). An `Axis` action adds its bindings and clamps the sum to -1..1, so A and D held together
  cancel and a key plus the stick never pass full.
- An action is down from a strength (an axis from a magnitude) of 0.5. Pressed and released compare this frame
  with the previous one, both read through the current bindings (rebinding is not a press or a release).
- Actions read only what the game receives (see above).
- `project.validate` warns about actions without bindings and bindings that can never act (`Scale` 0, or a key or
  button with a negative `Scale` on a `Button` action).

`Input.GetAction(name)` — the value (a number); `Input.IsActionDown/Pressed/Released(name)` — booleans. An unknown
name is an error that lists the defined actions (names are case-sensitive).

Rebinding lasts for the play session (it survives `Scene.Load`) and never changes the project's file; save the
player's choices with `Storage` and apply them in `OnCreate`:

- `Input.GetActions()` — every action: `{ Name, Type, Bindings }` tables
- `Input.GetBindings(name)` — the action's bindings, as tables like the file's (`{ Key = "Space" }`)
- `Input.SetBindings(name, bindings)` — replaces them (at most 16; `{}` removes every binding); a bad binding is an
  error and changes nothing
- `Input.ResetBindings([name])` — puts back the bindings play started with, for one action or all

```lua
function Player:OnUpdate(dt)
    local move = Input.GetAction("MoveX")
    self.Entity.Translation = self.Entity.Translation + Vec3(move * self.Speed * dt, 0, 0)
    if Input.IsActionPressed("Jump") then self:Jump() end
end
```

### Gamepads

Gamepads use the standard (Xbox-style) layout; GLFW's mapping database maps PlayStation, Switch and most other
controllers onto it. Up to 16 are tracked, by index 0..15 (an integer; anything else is an error).
Buttons: `A` (bottom face button), `B`, `X`, `Y`, `LeftBumper`, `RightBumper`, `Back`, `Start`, `Guide`,
`LeftThumb`, `RightThumb`, `DPadUp`, `DPadRight`, `DPadDown`, `DPadLeft`. Axes: `LeftX`, `LeftY`, `RightX`,
`RightY` (-1..1, +Y is down, like the canvas), `LeftTrigger`, `RightTrigger` (0..1). Names are
case-insensitive; an unknown one is an error. Sticks are reported raw: apply your own dead zone (input actions apply
one). Like the keyboard, gamepads reach the game only while it has focus (see above). While the window
does not have focus, gamepads read as released; a button or stick held through a focus loss (or pressed while
unfocused) stays released until it is let go. Controllers missing from GLFW's built-in mapping database are
ignored, with a warning naming them; mappings in SDL's format in the `SDL_GAMECONTROLLERCONFIG` environment
variable are added at startup.

- `Input.GetGamepads()` — the indices of the connected gamepads (array)
- `Input.IsGamepadConnected(index)`, `Input.GetGamepadName(index)` (nil when not connected)
- `Input.IsGamepadButtonDown/Pressed/Released(index, button)`, `Input.GetGamepadAxis(index, axis)` — a
  gamepad that is not connected reads as buttons up and centered axes (`IsGamepadButtonReleased` still reports
  the buttons held when it disconnected, on that frame)

Every script instance gets `OnGamepadConnected(index)` and `OnGamepadDisconnected(index)` before `OnUpdate`, on
the first frame the scene updates after the change: gamepads already connected when play starts are announced on
the first frame, changes while the game is paused when it resumes, and a gamepad replaced at its index (unplugged
and another plugged in) as a disconnect and a connect. An instance created later (a spawned entity, a new scene)
gets `OnGamepadConnected` for every connected gamepad right after its `OnCreate`. Headless runs have no devices: the
automation command `input.gamepad` injects one (tests and replays use it; a replay sees only the gamepads it
injects).

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

A body that has barely moved for about half a second falls asleep (`e:IsSleeping()`): it stops being simulated
until something touches, pushes or moves it (a force, an impulse, a non-zero velocity, setting its transform or a
changed joint target wakes it). That saves work in piles of resting objects, but it can also freeze a ball that
slowly turns around on a slope. Set `RigidBody.AllowSleeping = false` on bodies that must never stall (marbles,
rolling balls); dynamic bodies resting on or jointed to such a dynamic body stay awake with it. It has no effect
on static or trigger bodies (triggers never sleep), and setting it there logs a warning.

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
colors are linear HDR, so values above 1 stay bright after exposure and tonemapping (and contribute to bloom when the active PostProcess component enables it).
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

## Local light shadows

`PointLight` and `SpotLight` accept `CastShadows` (default `false`) and `ShadowSoftness`
(default `1`) through `GetComponent`/`SetComponent`, scene files, `component.set`, and the
inspector. Softness must be finite and within 0–4; other values are rejected. It is the PCF
radius in shadow texels (unlike directional light softness, which is an angular size).
Zero retains hardware comparison filtering. `ShadowsEnabled` in the scene's `Renderer`
settings (scene files / `scene.settings`) disables all shadows. `Mesh.CastShadows` controls
local and directional casters. Opaque and alpha-masked meshes cast; alpha-blended meshes do not.

Local shadows use a 4096² depth atlas with 64 fixed 512² tiles, allocated on first use; until
then a 1×1 fallback is bound. Each frame, point range spheres and spheres bounding spot cones
are tested against the camera frustum. Visible influences are ranked by conservative projected
size, capped at full-screen width and height, then UUID and light type. In particular, lights
containing the camera tie at full-screen importance even if their ranges differ. A spot consumes
one tile; a point consumes six overlapping cube faces, allocated as a whole. Zero-intensity
lights get no tiles. Lights that do not fit remain lit without shadows. The existing 256-entry
light buffer limit applies before atlas allocation. `render.stats` reports `localShadowMaps`,
`localShadowsSkipped`, `localShadowAtlasSize`, and `lightsSkipped`; the editor also displays the
counts. Atlas and light-buffer overflow each warn once per overflow episode.

Radius/range must be finite and nonnegative. Shadows use a near plane of
`min(0.05, range * 0.01)`; ranges below `0.001` do not allocate shadows. In the scene's `Renderer`
settings, `ShadowBias` (0–0.05) is a depth offset for directional shadows and a range-relative
world offset for local shadows, so tuning it affects both. `ShadowNormalBias` (0–1 world units)
offsets local receivers along their vertex normal, scaled by `(1 − N·L)` to emphasize grazing
angles. Invalid bias settings are rejected. A zero-scale spot has no valid direction and is
skipped; non-finite spot directions also warn once until the transforms recover.

## Screen UI

Game UI is drawn over the scene on a canvas 1080 units tall, with the origin at the top-left, +Y down and
the width following the window's aspect ratio (`UI.GetSize()`). It comes in two forms, drawn in this order:

1. **UI components** on entities, laid out from the scene hierarchy every frame. Use them for HUDs and menus
   that can be authored without code (the editor draws them in every mode and, outside play mode, outlines
   their rects: View > UI Bounds).
2. **Immediate-mode `UI.*` calls**, drawn on top. Submit them every frame (usually from `OnUpdate`); they
   last until the next update, so a paused game keeps showing them.

Colors are display colors with opacity, 0..1 (not HDR). Debug views (`--debug-view`) show no game UI.

### UI components

| Component | Fields |
|-----------|--------|
| `UIRect` | `Visible`, `AnchorMin`, `AnchorMax`, `Pivot`, `Position`, `Size` (Vec2s) |
| `UIImage` | `Texture`, `Color` (tint), `Slice` (Vec4 left, top, right, bottom, in texels), `SliceScale` |
| `UIText` | `Text`, `Font` (.ttf path, empty = built-in), `FontSize`, `Color`, `HorizontalAlign` (`Left`/`Center`/`Right`), `VerticalAlign` (`Top`/`Middle`/`Bottom`), `Wrap`, `Shadow` |
| `UIButton` | `Interactable`, tints (below), `Target`, `OnClick` |
| `UISlider` | `Interactable`, `Value`, `Min`, `Max`, `Step`, `TrackColor`, `FillColor`, `HandleColor`, `HandleWidth`, tints, `Target`, `OnValueChanged` |
| `UICheckbox` | `Interactable`, `Checked`, `BoxColor`, `CheckColor`, tints, `Target`, `OnValueChanged` |

`UIRect` places a rectangle relative to its parent's rect: that of the nearest ancestor with UI components,
or the whole canvas (nothing is clipped to it). Anchors are fractions of the parent rect (`0,0` top-left,
`1,1` bottom-right).

- **Equal anchors** pin the rect to that spot: `Size` is its size, and its `Pivot` (a fraction of the rect)
  sits on the anchor, moved by `Position`.
- **Different anchors** stretch the rect with its parent: `Size` is added to the anchors' span, so
  `Size = Vec2(-40, -40)` with anchors `0,0`/`1,1` (and the default centered `Pivot` and zero `Position`)
  insets it by 20 units on every side.
- `Visible = false` hides the entity and everything below it.

`UIImage` fills the rect with `Color`, or with `Texture` tinted by `Color`. An image that cannot be read is
logged once and draws the plain `Color` (the inspector and `ui.layout` show the error). A non-zero `Slice`
makes it a nine-slice panel: the corners keep their size (`Slice` texels times `SliceScale` canvas units,
shrinking proportionally when opposite borders would overlap, in the rect or in the texture), the edges
stretch along one axis and the center along both. `UIText` draws inside the rect, each line aligned on its
own, breaking lines at the rect's width when `Wrap` is on (between words; a word wider than the rect is
split across lines). Without a `UIRect`, an image or text fills its parent's rect.

Draw order follows the hierarchy: a parent before its children, later siblings over earlier ones. Change
components with `SetComponent` like any other. Values are checked, and a bad one is an error naming the
field: anchors and pivots in 0..1 with `AnchorMax >= AnchorMin`, `Position` and `Size` within ±1000000,
colors in 0..1, `Slice` 0..10000, `SliceScale` 0.001..100, `FontSize` 4..512; for sliders `Min < Max`
(within ±1000000), `Min <= Value <= Max`, `Step` and `HandleWidth` 0..1000000.

```lua
local label = Scene.FindEntityByName("ScoreLabel")
label:SetComponent("UIText", { Text = "SCORE " .. self.Score })
Scene.FindEntityByName("PauseMenu"):SetComponent("UIRect", { Visible = paused })
```

- `UI.GetRect(entity)` — the entity's laid-out rect as `Vec4(x, y, width, height)` in canvas units, or nil
  when it has no UI components or it or an ancestor is hidden. Laid out for the last rendered viewport
  (1920×1080 when headless), like `UI.GetSize()`.

### Widgets

`UIButton`, `UISlider` and `UICheckbox` make an entity's rect interactive while the scene plays (in the editor
and while simulating they only draw). Give the entity a `UIRect` as usual; the rect is the area that takes
input.

- **`UIButton`** draws nothing itself: it tints the entity's `UIImage` (its background) by state, and the
  entity's `UIText` is its label. Without a `UIImage` it is an invisible click area.
- **`UISlider`** (horizontal) draws its track (`TrackColor`, the whole rect), a fill from the left edge to the
  value (`FillColor`) and a handle (`HandleColor`, `HandleWidth` units wide, as tall as the rect, tinted by
  state) that stays inside the rect. `Value` lies in `Min..Max`; a positive `Step` snaps it to
  `Min + k * Step` (and `Max`), 0 is continuous.
- **`UICheckbox`** draws a box (`BoxColor`, tinted by state) as tall as the rect at its left edge and, when
  `Checked`, a mark inside it (`CheckColor`); the entity's `UIText` is laid out in the rest of the rect, as the
  label. Clicking anywhere in the rect toggles it.

Tints multiply one part of the widget by state (the button's `UIImage`, the slider's handle, the checkbox's
box): `NormalTint`, `HighlightTint` (hovered by the mouse, or focused by keyboard/gamepad navigation),
`PressedTint` (held with the mouse over it — a dragged slider stays pressed — or by the accept key) and
`DisabledTint` (`Interactable = false`: no input, no focus). An entity with several widget components acts as the first of button, slider, checkbox
(`ui.layout` reports it).

**Callbacks.** A user interaction calls a method on the script of the `Target` entity (0, the default, means
the widget's own entity), named by `OnClick` (buttons) or `OnValueChanged` (sliders: the new number;
checkboxes: the new boolean); an empty name calls nothing. The widget entity is the first argument, so one
menu script can handle every button:

```lua
-- On the menu entity; each button's Target is the menu, its OnClick names the method.
function Menu:StartGame(button) Scene.Load("Assets/Scenes/Level1.bscene") end
function Menu:OnValueChanged(widget, value)
    if widget.Name == "Volume" then self.Volume = value end
end
```

Callbacks run at the start of the frame, before `OnUpdate`, in the order the interactions happened; the
widget's `Value`/`Checked` is already updated. Setting values with `SetComponent` (or in the scene file) calls
nothing. A callback that cannot run — the target entity does not exist, has no running script, or its script
has no such method — is a script error, reported once per widget, method and kind of problem (`ui.layout` lists
the targets that are missing or have no Script). A widget or target that an earlier callback destroyed this
frame (a menu closing itself) is skipped. Names must be Lua identifiers (or
empty); `Target` is remapped when the widget is duplicated or instantiated with its target, like joints.

**Input.** Only widgets take pointer input (a plain `UIImage` over a button does not block it; hide or disable
the widgets under a modal panel). The topmost widget under the mouse is hovered (immediate-mode widgets are
over the components, later ones over earlier ones; a disabled widget still covers the ones below). Pressing
the left mouse button on a widget holds it; releasing over it clicks a button or toggles a checkbox (losing the
window's focus while holding does not), and a held slider follows the mouse as it moves, even off its rect. Any
left click in the game view drops the keyboard/gamepad focus, so after a click Space and Enter are the game's
again. Keyboard and
gamepads move the focus:

| Input | Effect |
|-------|--------|
| Arrows, D-pad, left stick (pushed past half way, along its dominant axis) | Focus the nearest widget in that direction (from widget centers; sideways distance counts double). The first press with nothing focused focuses the first widget. Held, it repeats after 0.4 s, every 0.1 s. |
| Left/Right on a focused slider | Move it to the next stop (`Min + k * Step`, and `Max`; a twentieth of the range when continuous); Up/Down still navigate |
| Tab / Shift+Tab | Next / previous widget in draw order, wrapping |
| Enter, keypad Enter, Space, gamepad `A` | Click the focused button / toggle the focused checkbox, on the press |

Hidden, disabled and destroyed widgets lose the focus, and so does everything when the scene changes
(`Scene.Load`). The keys that act on a focused widget still reach `Input`: a game that reads the arrows, Space or
the D-pad itself while widgets are visible turns navigation off (`UI.SetNavigationEnabled(false)`), or checks
`UI.GetFocus() == nil`. In the editor, the game view takes the mouse while it is hovered and the keyboard and
gamepads while it has focus.

- `UI.GetFocus()` — the focused widget: its entity, an immediate-mode widget's id (string), or nil
- `UI.SetFocus(widget)` — focus a widget entity (an error if it has no widget component) or an immediate-mode
  widget id; nil clears the focus. An immediate-mode id may be set before its widget is first drawn (e.g. in
  `OnCreate`, for a menu that opens with a button focused); a focus that is not a visible, interactable widget a
  frame later is dropped with a warning. An error outside play.
- `UI.SetNavigationEnabled(enabled)`, `UI.IsNavigationEnabled()` — whether the keyboard and gamepads move the
  focus and activate widgets (on by default; turning it off drops the focus, though `UI.SetFocus` can still set
  one). The game's setting: it survives `Scene.Load`, and the editor also pauses navigation while its game view
  does not have focus. `UI.SetNavigationEnabled` is an error outside play; `UI.IsNavigationEnabled` is then false.
- `UI.IsPointerOverWidget()` — whether the mouse is over an interactable widget (so a click there is the UI's,
  not the game's)

### Immediate mode

- `UI.GetSize()` — canvas size (Vec2)
- `UI.Text(text, x, y [, size = 32 [, color = Vec4(1) [, align = "Left"|"Center"|"Right"]]])` or
  `UI.Text(text, x, y, options)` — options: `Size`, `Color`, `Align`, `Anchor`, `Pivot`, `Font`,
  `Shadow` (default true), `Width` (wraps lines at this width)
- `UI.Rect(x, y, width, height [, color = Vec4(0, 0, 0, 0.5) | options])` — options: `Color`, `Anchor`, `Pivot`
- `UI.Image(texture, x, y, width, height [, tint = Vec4(1) | options])` — options: `Tint`, `Anchor`,
  `Pivot`, `Slice`, `SliceScale` (as in `UIImage`)

`Anchor` is a preset name (`TopLeft`, `Top`, `TopRight`, `Left`, `Center`, `Right`, `BottomLeft`,
`Bottom`, `BottomRight`) or a Vec2 of canvas fractions; `x, y` are then offsets from that point, so a HUD
keeps its place at any aspect ratio. `Pivot` (a fraction of the element) defaults to the anchor, so an
element anchored to a corner grows away from it. For text, `Align` sets the pivot's x. Unknown options,
unknown preset names, values of the wrong type and values outside the ranges the UI components accept (see
above; `x, y` within ±1000000, `width, height` up to 1000000, `Width` above 0 and up to 1000000) are
errors; a negative width or height draws nothing (a health bar at negative health). A frame takes at most 65536
`UI.*` commands; one more is an error (usually a loop that never ends).

```lua
UI.Text("Lives: " .. self.Lives, -24, 24, { Anchor = "TopRight", Size = 40, Font = "Assets/Fonts/Title.ttf" })
UI.Image("Assets/Textures/Panel.png", 0, -40, 480, 120, { Anchor = "Bottom", Slice = Vec4(12, 12, 12, 12) })
```

Immediate-mode widgets are drawn and take input like the widget components (with built-in default colors and the
components' default tints; a slider's label is drawn centered on it), and return what the user did:

- `UI.Button(label, x, y, width, height [, options])` — true on the frame the button is clicked
- `UI.Checkbox(label, checked, x, y, width, height [, options])` — the new checked state (options add
  `CheckColor`)
- `UI.Slider(label, value, min, max, x, y, width, height [, options])` — the new value (options add `Step`,
  `FillColor`, `HandleColor`, `HandleWidth`); `min < max`, and `value` must lie between them

Options for all three: `Id`, `Anchor`, `Pivot`, `Interactable` (default true), `Color` (background, box or
track), `TextColor`, `Size` (label font size; default 60% of the height, at most 32), `Font`. A widget is
identified across frames by its `Id`, which defaults to the label: two widgets with the same id in one frame are
an error (give one an `Id`; a slider without a visible label needs one). A label that changes (`"Volume 40%"`)
needs a stable `Id`, or the widget loses its hover, hold and focus whenever the text changes. A widget drawn
disabled ignores input that arrived while it was enabled. The width and height must be
positive. A widget takes input from the frame after it is first drawn, so draw it every frame; it lasts while it
is drawn. A frame takes at most 4096 immediate-mode widgets.

```lua
function Menu:OnUpdate(dt)
    if UI.Button("Play", 0, -60, 320, 64, { Anchor = "Center" }) then Scene.Load("Assets/Scenes/Level1.bscene") end
    self.Music = UI.Checkbox("Music", self.Music, 0, 20, 320, 40, { Anchor = "Center" })
    self.Volume = UI.Slider("Volume", self.Volume, 0, 1, 0, 80, 320, 32, { Anchor = "Center", Step = 0.1 })
end
```

### Fonts

`Font` takes a TrueType file in the project (`.ttf`, or `.otf` with TrueType outlines). Each file is loaded
once and drawn at any size. A font that cannot be read, or is not a TrueType font (CFF outlines,
collections, fonts without a Unicode character map), logs an error once and falls back to the built-in
font until the file changes (a font that loaded is kept). Every font is checked before it is used; glyphs
whose data could crash the font rasterizer draw blank, with a warning (also shown by the inspector and
`ui.layout`). Fonts are files up to 32 MB.

## Audio

An `AudioSource` component plays a project-relative clip (WAV, FLAC or MP3) while the scene plays: from
the start with `PlayOnStart`, or when a script calls `e:PlayAudio()` (which restarts it from the beginning).
Every field except the effects (`Doppler`, `LowPass`, `ReverbSend`) is read when the clip is loaded; changes
apply on the next `e:PlayAudio()`, which stops the current sound first even when the new one cannot play. The
effects apply while the sound plays: set them with `SetComponent` at any time. `PlayOnStart` sources (on play start, and those a
`Scene.Load` adds) are loaded before the scripts' `OnCreate`, where they already report
`e:IsAudioPlaying()`, and start sounding right after it: bus volumes set there apply from the first sample,
and `e:StopAudio()` there keeps a source silent. `e:PlayAudio()` and `e:StopAudio()` raise an error on an
entity without an `AudioSource`; `e:PlayAudio()` returns false (with the reason in the log) when the source
cannot play.

| Field | Meaning |
|-------|---------|
| `Volume` | Linear gain: a finite number >= 0 (0 = silent, 1 = unchanged, above 1 amplifies) |
| `Pitch` | Playback speed, a finite number > 0 (2 = twice as fast and an octave higher) |
| `Spatial` | Positioned at the entity, panned by direction and faded with distance from the listener (the first active `AudioListener`, or the primary camera) |
| `MinDistance`, `MaxDistance` | Full volume up to `MinDistance` (> 0), then `MinDistance / distance`, no quieter beyond `MaxDistance` (>= `MinDistance`; equal distances turn fading off) |
| `Bus` | `"Master"`, `"Music"`, `"SFX"` (default) or `"UI"` (see Mixer buses) |
| `Stream` | Decode the clip a second at a time while it plays instead of loading it whole: for music and long ambience |
| `Doppler` | Strength of the Doppler shift, 0 to 2: 0 (default) = off, 1 = physical. Spatial sources only (see Effects) |
| `LowPass` | Low-pass cutoff in Hz, 10 to 20000, muffling the sound (behind a wall, under water); 0 (default) = off. The cutoff used is at most 0.45 times the output's sample rate |
| `ReverbSend` | Share of the sound fed to its bus's reverb, 0 (default) to 1; the dry sound is unchanged |

Clips that are not streamed are decoded when they load and shared while any sound uses them. Positions
that are not finite, or beyond a billion units on an axis, never reach a sound: `e:PlayAudio()` and
`Audio.PlayOneShot` refuse them (logged), and a playing source or the listener keeps its last position. Invalid values
are rejected with an error by `SetComponent`, scene files and the automation API.

**Mixer buses.** Every sound plays through a bus: `Music`, `SFX` or `UI`, which all feed `Master` (a source
can also play on `Master` directly). A sound's level is its own volume times its bus's volume times
`Master`'s. Bus volumes belong to the play session: each starts at 1 when play starts, keeps its value
through `Scene.Load` and is forgotten when play stops, so apply saved settings in `OnCreate`:

```lua
function Menu:OnCreate()
	Audio.SetBusVolume("Music", Storage.Get("MusicVolume", 0.8))
end

function Menu:OnMusicVolumeChanged(slider, value) -- a UISlider's OnValueChanged
	Audio.SetBusVolume("Music", value)
	Storage.Set("MusicVolume", value)
end
```

- `Audio.PlayOneShot(clip [, position [, volume [, bus]]])` — fire and forget, on `bus` (default `"SFX"`);
  3D when a position is given (pass `nil` to skip it). Returns false when the clip cannot be loaded or
  audio is unavailable. One-shots are never streamed.
- `Audio.SetBusVolume(bus, volume)`, `Audio.GetBusVolume(bus)` — `bus` is `"Master"`, `"Music"`, `"SFX"` or
  `"UI"`; `volume` is a linear gain like `Volume`. `SetBusVolume` returns true, or false (logged) if the bus
  could not be created.
- An unknown bus name or a volume that is negative or not finite raises an error, as does any `Audio.*`
  call outside play ("audio is not running").

**Effects.** A source's effects follow its component while it plays, so scripts can drive them every frame:

- *Low-pass* (`LowPass`): a 12 dB-per-octave filter that glides to a new cutoff (most of the way within about
  10 ms, settled within about 100 ms), so it can follow a value that changes every frame without clicks.
  Raycasting from the listener to a source and muffling it when something is in the way gives cheap
  occlusion (see `Samples/FirstPerson`).
- *Reverb* (`ReverbSend`): each bus gets one reverb the first time one of its sources sends to it. Its
  output returns into that bus, so the bus volume scales the reverb too; turning a send down lets the tail
  ring out. One room setting applies to every bus's reverb; it belongs to the play session like the bus
  volumes (defaults when play starts, kept through `Scene.Load`):
  - `Audio.SetReverb({ RoomSize = 0.85, Damping = 0.1, Width = 1 })` patches the given settings (each 0
    to 1: larger rooms ring longer, damping dulls the tail, width spreads it in stereo). Unknown or
    non-string keys, non-numbers and values outside 0..1 raise an error, and nothing changes.
  - `Audio.GetReverb()` returns the current settings as a table.
  The reverb handles mono and stereo output at 22.05 to 176.4 kHz: on other devices (surround) the sends
  are dropped (logged once per bus) and the sounds play dry.
- *Doppler* (`Doppler`): pitch rises while a source and the listener approach each other and falls as
  they part. Velocities come from physics where possible: the entity's own dynamic or kinematic body or
  character, else its closest ancestor's (a camera on a character moves with it; only the linear velocity
  counts), else its change in world position over the frame (also for a static body a script moves).
  Speeds are capped at 100 units per second so a teleport does not read as a sonic boom, and placing an
  entity in `OnCreate` is not a movement.
  `e:GetAudioDopplerPitch()` reports the shift the last update set up. One-shots are never shifted.

```lua
-- Occlusion: muffle the generator when a wall is between it and the player's camera. OnLateUpdate runs after
-- physics, so the ray sees where the player ended up this frame.
function Game:OnLateUpdate(dt)
	local from = self.Camera.WorldPosition
	local to = self.Generator.WorldPosition
	local hit = Physics.Raycast(from, to - from, from:Distance(to), self.Player)
	local blocked = hit ~= nil and hit.Entity ~= self.Generator
	self.Generator:SetComponent("AudioSource", { LowPass = blocked and 600 or 0 })
end
```

## Debug drawing, time, logging, game

- `Debug.DrawLine(from, to [, color])`, `Debug.DrawArrow(from, to [, color])`,
  `Debug.DrawBox(center, halfExtents [, color])`, `Debug.DrawSphere(center, radius [, color])` — drawn
  this frame only
- `Time.GetDelta()`, `Time.GetElapsed()`, `Time.GetFrame()`
- `Log.Trace/Info/Warn/Error(...)`, `print(...)` (to the engine log / editor console)
- `Game.Quit()` — ends the game (the runtime exits, the editor leaves play mode)

## Skeletal animation

An `Animator` component draws a whole glTF model under one entity. `Mesh` is the asset path;
`Clip` selects the initial clip (empty selects the first; it may also name a blend space, see Blend spaces); `PlayOnStart` and `Looping` default to true;
`Speed` defaults to 1 and accepts 0..100; `CastShadows` defaults to true. Importing a model with skins
or animation creates this component on the imported root. Static imports retain their entity hierarchy.
The inspector, component commands and generic Lua component access share these fields.

```lua
local actor = Scene.CreateEntity("Fox")
actor:AddComponent("Animator", { Mesh = "Assets/Models/Fox.glb", Clip = "Walk" })
actor:CrossFadeAnimation("Run", 0.25)
```

Alternatively, use a persistent two-clip blend. Starting it replaces any crossfade; later updates to
the same pair preserve both clocks:

```lua
actor:BlendAnimations("Walk", "Run", 0.7)
```

| Function | Behavior |
|---|---|
| `e:GetAnimationClips()` | Array of selectable clip names. Unnamed clips use `AnimationN`; duplicates receive numeric suffixes. |
| `e:PlayAnimation([clip])` | Restarts the selected clip at 0, clears blending, returns success. Omitted/nil/empty selects the first clip; other argument types raise an error. |
| `e:CrossFadeAnimation(clip, seconds)` | Starts the target at 0 and fades from the current pose. When leaving a single clip, outgoing and target clocks advance. Interrupted fades, blends, and stopped poses fade from a fixed snapshot of the visible pose. Zero duration acts as Play. Returns success. |
| `e:BlendAnimations(first, second, weight)` | Persistent local-TRS blend, weight 0..1 toward the second clip. New pairs start at 0; changing only weight preserves both clocks. Returns success. |
| `e:PauseAnimation([paused])` | Freezes/resumes active clocks and transitions. False does not restart a stopped animator; use PlayAnimation. Defaults to true; only boolean or nil accepted. |
| `e:StopAnimation()` | Stops, clears blending, resets time, and displays the authored rest pose. |
| `e:SetAnimationTime(seconds)` | Seeks to nonnegative finite seconds, clamped to each active clip's duration, without changing pause state or fade progress. Returns success. |
| `e:GetAnimationTime()` | Target/current clip time in seconds (the second clip for a persistent blend). |
| `e:GetAnimationDuration()` | Target/current clip duration, or 0 if none. |
| `e:IsAnimationPlaying()` | Whether playback is advancing; speed 0 still counts as playing; false while a blend space cannot play. |

All functions require a running scene and an Animator on the entity. A missing asset yields empty
clips, zero times, false queries/operations. Unknown clips or invalid numeric parameters return false
without changing existing playback. Failures are logged with the operation and clip name or invalid value. Non-looping clips hold their final pose; looping clocks wrap by
clip duration, including large timesteps. Enabling Looping after a clip finishes does not restart it;
call PlayAnimation. Play, CrossFade, Blend and PlayBlendSpace unpause playback. Empty clip names select the first
clip in Play, CrossFade and Blend. Crossfade duration is scene time, independent of Speed.
`Speed` and `Looping` updates apply immediately; changing `Mesh` or the configured `Clip`, asset reload,
or removing/re-adding Animator resets playback according to `PlayOnStart`. With PlayOnStart false,
the selected clip is held at time 0 until played. Edit mode shows the authored rest pose.

Playback also runs in Simulate, respects scene pause/step, and advances after scripts and physics.
State belongs to the play scene and is not serialized or included in `scene.hash`; copies start fresh.
Persistent entities retain playback across `Scene.Load`. There is no retargeting or root-motion
extraction. Animated node motion is relative to the entity; physics uses the entity's colliders, not
deformed triangles.

### Animation events

`Animator.Events` lists named markers on clip timelines: `{ Clip = "Walk", Time = 0.4, Name = "Footstep" }`
(`Clip` empty marks every clip; `Name` must not be empty; `Time` is seconds, finite and nonnegative). When
playback crosses a marker the entity's scripts receive `OnAnimationEvent(name, clip)`, and automation
clients subscribed to `animation` events get `{name, clip, entity, frame}`.

- A marker fires when the clock moves over it: each update covers `[previous time, new time)`, so a marker at
  0 fires on the first update after `PlayAnimation`, and one at the seek target fires after `SetAnimationTime`
  (except at the end of a non-looping clip, which has nothing left to play). Seeks, stops and pauses skip what
  lies between; speed 0 fires nothing.
- A looping clip fires its tail (its end included) and then restarts at 0 on every wrap, so one long update
  fires every marker of every loop it covers, in order. A non-looping clip fires its end once.
- During a crossfade from a single playing clip both clips fire until the fade ends (the `clip` argument tells
  them apart); fades that start from a fixed pose (out of a blend, a blend space or an interrupted fade) fire only
  the target. With `BlendAnimations` each clip fires while its weight is above 0. In a blend space only the heaviest sample's clip
  fires, because its clips run in step and would repeat each marker; when the parameter makes another sample the
  heaviest, a marker near that moment can be skipped or repeated once.
- At most 64 events per Animator and frame are delivered; the rest are dropped with a warning (once per
  animation started). A marker past the end of the playing clip does not fire for it (an empty-`Clip` marker still
  fires for longer clips); markers naming a missing or zero-length clip never fire. The inspector and `scene.info`
  warn about markers that can never fire.
- Events are delivered after `OnLateUpdate` and after this frame's pose and attachments are computed, before
  particles; a callback may change animation or remove the Animator safely. Within one update an Animator's
  events come clip by clip in the order playback crossed them (the current clip's before the outgoing clip's; for
  `BlendAnimations(first, second)`, `second`'s first). Simulate reports
  them to automation only (no scripts run there).

### Blend spaces

`Animator.BlendSpaces` places clips on parameter axes, e.g. a 1D locomotion space over speed:

```lua
actor:SetComponent("Animator", { MatchVelocity = true, BlendSpaces = { {
	Name = "Locomotion", Type = "1D", Samples = {
		{ Clip = "Survey", X = 0 },             -- Speed 0: stands still
		{ Clip = "Walk", X = 1.5, Speed = 1.5 }, -- m/s the clip moves at playback speed 1
		{ Clip = "Run", X = 5, Speed = 5 } } } } })
actor:PlayBlendSpace("Locomotion", 0, nil, 0.2)
function Player:OnUpdate(dt) self.Entity:SetBlendSpaceParameter(self.Speed) end
```

- 1D spaces interpolate between the two samples around `X` and clamp outside them; 2D spaces (`Type = "2D"`,
  e.g. strafing over X/Y velocity) use gradient band interpolation: a sample's weight is 1 at its position and
  falls to 0 at every other sample. Weights always sum to 1; samples at the same position share its weight. An
  empty sample `Clip` selects the first clip, and samples whose clip is missing get no weight (their share goes to
  the other weighted samples). Blend space names must be unique (the inspector's Add and Duplicate pick a fresh one).
- All clips run in step: one cycle lasts the weighted sum of their durations and each clip is sampled at the
  same fraction of its own length, so feet stay in phase. `SetBlendSpaceParameter` keeps that phase.
- `PlayBlendSpace` fades from the visible pose over `fadeSeconds` (default 0). While the same space is playing it
  only moves the parameter (the fade is ignored); a paused or finished one restarts. `PlayAnimation`,
  `CrossFadeAnimation`, `BlendAnimations` and `StopAnimation` leave the space; `PauseAnimation`,
  `GetAnimationTime`, `GetAnimationDuration` and `SetAnimationTime` act on its blended cycle.
  `GetBlendSpaceWeights()` returns `{ [clip] = weight }` (clips without weight at the parameter listed with 0; empty
  when no space runs or it cannot play). `SetBlendSpaceParameter(x)` without `y` keeps a 2D space's current Y.
- A space cannot play when it is missing or has no samples, or when no sample weighted at the parameter has a clip
  (e.g. after edits during play, or a parameter that sits on a sample whose clip is missing). Playback then holds
  its pose, `IsAnimationPlaying()`, `SetBlendSpaceParameter` and `SetAnimationTime` return false and the problem is
  logged when it appears or changes; it resumes as soon as the space can play.
- `Animator.Clip` may name a blend space: `PlayOnStart` then starts it at parameter (0, 0). A clip of the same
  name wins (the inspector warns). A blend space of that name added during play still starts, unless the Animator
  played something else first.
- **MatchVelocity:** with it on, the whole space plays at one rate that blends the plain `Speed` (for the weight
  of samples with `Speed` 0: standing, or unknown) with `Speed` x the entity's measured horizontal speed / the
  moving samples' weighted `Speed` (for the moving samples' weight), clamped to 0..100. A character moving faster
  than its run clip strides faster instead of sliding, and the rate eases in from idle. The speed is measured from
  the entity's world position (any mover: physics, characters, Lua), ignoring vertical motion and any horizontal
  move faster than 100 units/s between updates (teleports), and smoothed over about 0.1 s. A space of only moving
  samples freezes while the entity stands still.

### Bones

`e:GetBoneTransform(joint)` returns the world position, rotation and scale of the first node named `joint` in
the Animator's current pose (any glTF node, not only skin joints), or nil when there is none or the Animator's model
cannot animate (logged when the problem appears or changes, per joint). Animation advances after `OnLateUpdate`, so `OnUpdate` and `OnLateUpdate` see the previous
frame's pose and `OnAnimationEvent` this frame's. Like the other Animator functions it raises an error on an
entity without an Animator.

A `BoneAttachment` component keeps an entity on a joint of another entity's Animator while the scene plays or
simulates (weapon in hand, hat on head): `{ Target = <entity ID, 0 = the parent>, Joint = "b_Head_05",
Translation, Rotation, Scale }` (the offset in the joint's space; `Rotation` in Euler degrees, as in component
data). Its world transform is set right after the pose is sampled, also for children of other entities, and
attachments to attached entities update in dependency order within the frame. Edit mode keeps the authored
transform. Physics reads transforms before animation, so a Kinematic body on an attachment follows one physics
step later (its body is moved with a velocity, so it pushes what it hits); a Dynamic body is also moved by
physics (the inspector warns). Missing targets, joints or Animators, a Target inside the entity's own hierarchy
below it, attachment cycles and a zero scale or non-finite value above the entity (or in its resulting transform) are
logged once and leave the entity where it is.

## Graphics settings

`Graphics.GetSettings()` returns a copy of the current Play-session preferences. `Graphics.SetSettings(patch)`
atomically updates any subset, rejecting unknown fields, invalid types, nonfinite numbers and bad ranges:

| Key | Values/default |
|---|---|
| `VSync` | Boolean; project default true |
| `RenderScale` | 0.25–2; default 1 |
| `ShadowQuality` | `"Off"`, `"Low"`, `"High"`; default `"High"` |
| `SSAOQuality` | `"Off"`, `"Low"`, `"High"`; default `"High"` |
| `DynamicResolution` | Boolean; default false |
| `TargetFrameRate` | 10–1,000,000; default 60 (project defaults are limited to 1000; larger values in all Play sessions, including editor Play, support diagnostic runs) |
| `MinRenderScale` | 0.25–2; default 0.5 |

Settings start from the project (plus runtime command-line overrides), apply on the next rendered frame,
survive `Scene.Load`, and reset on restarting Play. They do not modify saved project or scene data. Use
`Storage` explicitly if player preferences should persist. Headless scripts can use the same API.

```lua
local preferences = Graphics.GetSettings()
Graphics.SetSettings({ VSync = false, RenderScale = 0.75, SSAOQuality = "Low" })
-- PostProcess is authored scene data, available through the shared component API:
local post = Scene.FindEntityByName("PostProcessing")
post:SetComponent("PostProcess", { BloomEnabled = true, FXAAEnabled = true, AutoExposure = true })
```

The highest-priority enabled PostProcess component wins (lowest UUID breaks ties). Its settings include
`Exposure`, `Tonemapper`, SSAO radius/intensity/bias, auto-exposure limits/compensation/adaptation, and bloom
threshold/intensity/scatter. See [Components](Components.md) for the field list and [Rendering](Rendering.md)
for ordering, quality levels, legacy fallback and exposure lifecycle.

Explicit runtime CLI graphics flags remain host overrides above Lua preferences; `Graphics.GetSettings()`
reports the stored session preferences. Unspecified settings remain script-controlled.
