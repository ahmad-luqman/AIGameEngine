-- Feature test driver: exercises every component and the entire Lua scripting API.
--
-- Run headless by ctest (see Tests/Data/FeatureTest/FeatureTest.batch.json). The batch presses Space and the
-- left mouse button for the first frame, releases them and moves the mouse, plays ~3 seconds (failing on any
-- script error), then calls FeatureTest.Report(), which raises an error listing every failed check.

FeatureTest = { Checks = 0, Failures = {}, Phase = "create", Quit = false }
FeatureTestEvents = FeatureTestEvents or {}

local function Check(name, condition)
	FeatureTest.Checks = FeatureTest.Checks + 1
	if not condition then
		table.insert(FeatureTest.Failures, name)
		Log.Error("FeatureTest FAILED: " .. name)
	end
end

local function Near(a, b, epsilon)
	return math.abs(a - b) <= (epsilon or 1e-4)
end

local function NearVec(a, b, epsilon)
	return Near(a.x, b.x, epsilon) and Near(a.y, b.y, epsilon) and Near(a.z, b.z, epsilon)
end

local function Expect(name, fn)
	local ok, err = pcall(fn)
	Check(name .. (ok and "" or (": " .. tostring(err))), ok)
end

local function ExpectError(name, fn, fragment)
	local ok, err = pcall(fn)
	Check(name, (not ok) and (fragment == nil or string.find(tostring(err), fragment, 1, true) ~= nil))
end

function FeatureTest.Report()
	if #FeatureTest.Failures > 0 then
		error(string.format("%d of %d checks failed: %s", #FeatureTest.Failures, FeatureTest.Checks, table.concat(FeatureTest.Failures, "; ")))
	end
	if FeatureTest.Phase ~= "done" then
		error("feature test did not finish (phase: " .. FeatureTest.Phase .. ")")
	end
	return string.format("PASS: %d checks", FeatureTest.Checks)
end

local Driver = {}
Driver.Properties = { Duration = 3.0, Note = "default", Offset = Vec3(0, 0, 0), Verbose = false }

-- ---------------------------------------------------------------------------------------------
-- Static API checks (OnCreate)
-- ---------------------------------------------------------------------------------------------

local function TestMath()
	local a, b = Vec3(1, 2, 3), Vec3(4, 5, 6)
	Check("Vec3 add", (a + b) == Vec3(5, 7, 9))
	Check("Vec3 sub", (b - a) == Vec3(3, 3, 3))
	Check("Vec3 scale", (a * 2) == Vec3(2, 4, 6) and (2 * a) == Vec3(2, 4, 6))
	Check("Vec3 mul/div", (a * b) == Vec3(4, 10, 18) and (b / 2).x == 2)
	Check("Vec3 unary minus", (-a) == Vec3(-1, -2, -3))
	Check("Vec3 dot/cross", a:Dot(b) == 32 and Vec3(1, 0, 0):Cross(Vec3(0, 1, 0)) == Vec3(0, 0, 1))
	Check("Vec3 length/normalized", Near(Vec3(3, 4, 0):Length(), 5) and Near(Vec3(0, 0, 9):Normalized().z, 1))
	Check("Vec3 distance/lerp", Near(a:Distance(b), math.sqrt(27)) and a:Lerp(b, 0.5) == Vec3(2.5, 3.5, 4.5))
	local v = Vec3(1)
	v.y = 7
	Check("Vec3 fields", v.x == 1 and v.y == 7 and string.find(tostring(v), "Vec3") ~= nil)
	Check("Vec2", (Vec2(1, 2) + Vec2(3, 4)) == Vec2(4, 6) and Near(Vec2(3, 4):Length(), 5) and Vec2(1, 0):Dot(Vec2(0, 1)) == 0)
	Check("Vec4", (Vec4(1, 2, 3, 4) * 2) == Vec4(2, 4, 6, 8) and Vec4(1).w == 1)

	local q = Quat.AngleAxis(Math.Radians(90), Vec3(0, 1, 0))
	Check("Quat rotate", NearVec(q * Vec3(1, 0, 0), Vec3(0, 0, -1)))
	Check("Quat identity/inverse", NearVec((q * q:Inverse()) * Vec3(1, 2, 3), Vec3(1, 2, 3)) and Quat.Identity() == Quat(1, 0, 0, 0))
	Check("Quat euler", NearVec(Quat.FromEuler(Vec3(0, Math.Radians(90), 0)):ToEuler(), Vec3(0, Math.Radians(90), 0), 1e-3))
	Check("Quat slerp/normalized", Near(Quat.Slerp(Quat.Identity(), q, 0.5):Normalized().w, math.cos(Math.Radians(22.5)), 1e-4))
	Check("Quat look rotation", NearVec(Quat.LookRotation(Vec3(1, 0, 0)) * Vec3(0, 0, -1), Vec3(1, 0, 0), 1e-4))

	Check("Math constants", Near(Math.Pi, math.pi, 1e-6) and Near(Math.Degrees(Math.Radians(45)), 45))
	Check("Math clamp/lerp/sign", Math.Clamp(5, 0, 1) == 1 and Math.Lerp(0, 10, 0.25) == 2.5 and Math.Sign(-3) == -1)
	Math.Seed(42)
	local first = Math.Random()
	Math.Seed(42)
	Check("Math.Random is seedable", Math.Random() == first)
	local r = Math.RandomInt(3, 5)
	Check("Math.RandomInt range", r >= 3 and r <= 5)
	local f = Math.Random(-2, -1)
	Check("Math.Random(min, max) range", f >= -2 and f <= -1)
	-- Lua's math.random shares the engine generator, so it is deterministic too (Lua seeds it from the clock).
	math.randomseed(7)
	local a, b, c = math.random(), math.random(10), math.random(-3, 3)
	math.randomseed(7)
	Check("math.random is deterministic", math.random() == a and math.random(10) == b and math.random(-3, 3) == c)
	Check("math.random ranges", a >= 0 and a < 1 and b >= 1 and b <= 10 and c >= -3 and c <= 3 and math.type(b) == "integer")
	Check("math.random empty interval errors", not pcall(math.random, -1))
end

local function TestEntities(self)
	local e = self.Entity
	Check("Entity identity", e:IsValid() and e.Name == "FeatureTestDriver" and e.ID > 0 and string.find(tostring(e), "FeatureTestDriver") ~= nil)
	Check("Entity equality", e == Scene.GetEntityByID(e.ID))

	-- Properties (defaults and per-entity overrides from the scene).
	Check("Script property override (number)", self.Duration == 4.5)
	Check("Script property override (string)", self.Note == "from scene")
	Check("Script property override (vector)", self.Offset == Vec3(1, 2, 3))
	Check("Script property default (bool)", self.Verbose == false)

	-- Transform access.
	local probe = Scene.CreateEntity("TransformProbe")
	probe.Translation = Vec3(1, 2, 3)
	probe.Scale = Vec3(2, 2, 2)
	probe.EulerAngles = Vec3(0, Math.Radians(90), 0)
	Check("Translation/Scale", probe.Translation == Vec3(1, 2, 3) and probe.Scale == Vec3(2, 2, 2))
	Check("EulerAngles/Rotation", NearVec(probe.EulerAngles, Vec3(0, Math.Radians(90), 0), 1e-3) and Near(probe.Rotation.w, math.cos(Math.Radians(45)), 1e-4))
	Check("Direction vectors", NearVec(probe:GetForward(), Vec3(-1, 0, 0), 1e-4) and NearVec(probe:GetRight(), Vec3(0, 0, -1), 1e-4) and NearVec(probe:GetUp(), Vec3(0, 1, 0), 1e-4))
	probe.Rotation = Quat.Identity()
	probe:LookAt(Vec3(1, 2, -10))
	Check("LookAt", NearVec(probe:GetForward(), Vec3(0, 0, -1), 1e-4))
	probe.WorldPosition = Vec3(5, 5, 5)
	Check("WorldPosition", probe.WorldPosition == Vec3(5, 5, 5))
	probe.Name = "Renamed"
	Check("Rename", probe.Name == "Renamed" and Scene.FindEntityByName("Renamed") == probe)

	-- Hierarchy.
	local child = Scene.CreateEntity("Child")
	child:SetParent(probe)
	child.Translation = Vec3(0, 1, 0)
	Check("SetParent/GetParent", child:GetParent() == probe and #probe:GetChildren() == 1 and probe:FindChild("Child") == child)
	Check("World position through hierarchy", NearVec(child.WorldPosition, Vec3(5, 7, 5), 1e-3))
	ExpectError("Parent cycle rejected", function() probe:SetParent(child) end, "descendant")
	child:SetParent(nil)
	Check("Unparent", child:GetParent() == nil)

	-- Scene queries.
	Check("FindEntityByName", Scene.FindEntityByName("Ground") ~= nil and Scene.FindEntityByName("Nobody") == nil)
	Check("FindEntitiesByName", #Scene.FindEntitiesByName("Pillar") == 2)
	Check("GetEntitiesWith", #Scene.GetEntitiesWith("SpotLight") == 1 and #Scene.GetEntitiesWith("Script") >= 3)
	Check("GetPrimaryCamera", Scene.GetPrimaryCamera() ~= nil and Scene.GetPrimaryCamera().Name == "MainCamera")
	Check("GetEntityCount", Scene.GetEntityCount() > 15)
	ExpectError("GetEntitiesWith unknown component", function() Scene.GetEntitiesWith("Nope") end, "unknown component")

	-- Components: generic access through the component registry.
	Expect("AddComponent with data", function() probe:AddComponent("PointLight", { Intensity = 7, Color = { 1, 0, 0 } }) end)
	local light = probe:GetComponent("PointLight")
	Check("GetComponent", light ~= nil and light.Intensity == 7 and light.Color[1] == 1)
	light.Radius = 3
	probe:SetComponent("PointLight", light)
	Check("SetComponent", probe:GetComponent("PointLight").Radius == 3)
	Check("HasComponent", probe:HasComponent("PointLight") and not probe:HasComponent("SpotLight"))
	probe:RemoveComponent("PointLight")
	Check("RemoveComponent", not probe:HasComponent("PointLight") and probe:GetComponent("PointLight") == nil)
	ExpectError("Unknown component", function() probe:AddComponent("Teleporter") end, "unknown component")
	ExpectError("Invalid field", function() probe:AddComponent("RigidBody", { Mas = 1 }) end, "Mas")
	ExpectError("Core component removal", function() probe:RemoveComponent("Transform") end, "cannot be removed")
	ExpectError("SetComponent on missing component", function() probe:SetComponent("Camera", {}) end, "no Camera")

	-- Every component type round-trips through Lua.
	for _, name in ipairs({ "Camera", "Mesh", "Material", "DirectionalLight", "PointLight", "SpotLight", "SkyLight", "RigidBody",
		"BoxCollider", "SphereCollider", "CapsuleCollider", "Joint", "AudioSource", "AudioListener", "Prefab" }) do
		local holder = Scene.CreateEntity("Holder" .. name)
		Expect("AddComponent " .. name, function() holder:AddComponent(name) end)
		local data = holder:GetComponent(name)
		Expect("SetComponent round trip " .. name, function() holder:SetComponent(name, data) end)
		holder:Destroy()
	end

	-- Other scripts.
	local spinner = Scene.FindEntityByName("Turntable"):GetScript()
	Check("GetScript", spinner ~= nil and spinner.Label == "turntable" and spinner.Speed == 2.0)
	Check("GetScript on entity without script", Scene.FindEntityByName("Ground"):GetScript() == nil)

	-- Destruction and stale handles.
	local doomed = Scene.CreateEntity("Doomed")
	Scene.Destroy(doomed)
	self.Doomed = doomed
	child:Destroy()
	probe:Destroy()
end

local function TestSystems(self)
	-- Logging (also print).
	Log.Trace("FeatureTest trace")
	Log.Info("FeatureTest info", 1, true, Vec3(1, 2, 3))
	Log.Warn("FeatureTest warn (expected)")
	print("FeatureTest print")

	-- Debug drawing.
	Expect("Debug drawing", function()
		Debug.DrawLine(Vec3(0, 0, 0), Vec3(1, 1, 1))
		Debug.DrawLine(Vec3(0, 0, 0), Vec3(1, 0, 0), Vec4(1, 0, 0, 1))
		Debug.DrawArrow(Vec3(0, 0, 0), Vec3(0, 2, 0))
		Debug.DrawBox(Vec3(0, 1, 0), Vec3(0.5, 0.5, 0.5), Vec4(0, 1, 0, 1))
		Debug.DrawSphere(Vec3(0, 1, 0), 0.5)
	end)

	-- Screen-space UI.
	local canvas = UI.GetSize()
	Check("UI.GetSize", canvas.y == 1080 and canvas.x > 0)
	Expect("UI drawing", function()
		UI.Rect(10, 10, 200, 60)
		UI.Rect(10, 10, 200, 60, Vec4(0, 0, 0, 0.5))
		UI.Text("Feature test", 20, 20)
		UI.Text("Centered", canvas.x * 0.5, 100, 48, Vec4(1, 1, 0, 1), "Center")
		UI.Text("Right", canvas.x - 20, 100, 24, Vec4(1, 1, 1, 1), "Right")
	end)
	ExpectError("UI.Text invalid alignment", function() UI.Text("x", 0, 0, 20, Vec4(1), "Middle") end, "align")

	-- Time.
	Check("Time at start", Time.GetFrame() == 0 and Near(Time.GetElapsed(), 0))

	-- Physics settings.
	local gravity = Physics.GetGravity()
	Check("Physics.GetGravity (scene setting)", NearVec(gravity, Vec3(0, -9.81, 0), 1e-3))
	Physics.SetGravity(Vec3(0, -20, 0))
	Check("Physics.SetGravity", NearVec(Physics.GetGravity(), Vec3(0, -20, 0)))
	Physics.SetGravity(gravity)

	-- Audio.
	local speaker = Scene.FindEntityByName("Speaker")
	Check("AudioSource PlayOnStart", speaker:IsAudioPlaying())
	speaker:StopAudio()
	Check("StopAudio", not speaker:IsAudioPlaying())
	Check("PlayAudio", speaker:PlayAudio() and speaker:IsAudioPlaying())
	Check("Audio.PlayOneShot", Audio.PlayOneShot("Assets/Audio/Beep.wav", Vec3(0, 1, 0), 0.5) and Audio.PlayOneShot("Assets/Audio/Beep.wav"))
	Check("Audio.PlayOneShot missing clip", not Audio.PlayOneShot("Assets/Audio/Missing.wav"))

	-- Input names are validated.
	ExpectError("Unknown key name", function() Input.IsKeyDown("Hyperspace") end, "unknown key")
	ExpectError("Unknown mouse button", function() Input.IsMouseButtonDown("Thumb") end, "unknown mouse button")

	-- Sandbox: no file access from scripts.
	Check("Sandbox: dofile removed", dofile == nil and loadfile == nil and io == nil and os == nil)
	local util = require("Assets.Scripts.Lib.FeatureUtil")
	Check("require project module", util.Triple(3) == 9)
end

-- ---------------------------------------------------------------------------------------------
-- Lifecycle
-- ---------------------------------------------------------------------------------------------

function Driver:OnCreate()
	self.Frame = 0
	self.Late = 0
	TestMath()
	TestEntities(self)
	TestSystems(self)
	FeatureTest.Phase = "running"
end

function Driver:OnUpdate(dt)
	self.Frame = self.Frame + 1
	local frame = self.Frame
	Check("Time.GetDelta", Near(Time.GetDelta(), dt))

	if frame == 1 then
		-- The batch holds Space and the left mouse button from the first frame.
		Check("Input.IsKeyPressed", Input.IsKeyPressed("Space"))
		Check("Input.IsKeyDown", Input.IsKeyDown("Space") and not Input.IsKeyDown("A"))
		Check("Input.IsMouseButtonPressed", Input.IsMouseButtonPressed("Left") and Input.IsMouseButtonDown("Left"))
		Check("Input.GetMousePosition", Input.GetMousePosition() == Vec2(320, 240))
		Check("Input.GetMouseScroll", Input.GetMouseScroll() == Vec2(0, 0))
		Check("No release on the press frame", not Input.IsKeyReleased("Space") and not Input.IsMouseButtonReleased("Left"))
		-- 200 m/s covers 3.3 m per step; only the continuous sweep stops it at the 5 cm wall.
		Scene.FindEntityByName("Bullet"):SetLinearVelocity(Vec3(200, 0, 0))
		Check("Destroyed entity is gone", not self.Doomed:IsValid())
		ExpectError("Stale entity access", function() return self.Doomed.Name end, "no longer exists")

		-- Spawning a prefab.
		local projectile = Scene.Instantiate("Assets/Prefabs/Projectile.bprefab", Vec3(-4, 3, 0))
		Check("Scene.Instantiate", projectile ~= nil and projectile.Name == "Projectile" and projectile:HasComponent("Prefab"))
		Check("Prefab children", projectile:FindChild("Trail") ~= nil)
		Check("Prefab script started immediately", FeatureTestEvents.ProjectilesCreated == 1)
		projectile:SetLinearVelocity(Vec3(0, -5, 0))
		Check("Prefab script properties", projectile:GetScript().Lifetime == 1.0)
		ExpectError("Instantiate missing prefab", function() Scene.Instantiate("Assets/Prefabs/Missing.bprefab") end, "Missing.bprefab")

		-- Physics API on the crate (gravity disabled in the scene).
		local crate = Scene.FindEntityByName("Crate")
		crate:SetLinearVelocity(Vec3(1, 0, 0))
		Check("Get/SetLinearVelocity", NearVec(crate:GetLinearVelocity(), Vec3(1, 0, 0), 1e-3))
		crate:SetAngularVelocity(Vec3(0, 1, 0))
		Check("Get/SetAngularVelocity", NearVec(crate:GetAngularVelocity(), Vec3(0, 1, 0), 1e-3))
		crate:AddImpulse(Vec3(0, 0, 2))
		Check("AddImpulse", Near(crate:GetLinearVelocity().z, 1.0, 1e-3)) -- mass 2
		crate:AddForce(Vec3(0, 0, 60))
		crate:AddTorque(Vec3(0, 6, 0))
		self.CrateStart = crate.Translation

		-- Runtime-created physics entity.
		local dynamic = Scene.CreateEntity("RuntimeBox")
		dynamic.Translation = Vec3(6, 4, 0)
		dynamic:AddComponent("RigidBody", { Type = "Dynamic" })
		dynamic:AddComponent("BoxCollider", { HalfExtents = { 0.25, 0.25, 0.25 } })
		dynamic:AddComponent("Mesh", { Mesh = "builtin://Cube" })
		self.RuntimeBox = dynamic

		-- Joints: the gate door is hinged to its post; a motor set through SetComponent swings it open.
		local door = Scene.FindEntityByName("GateDoor")
		local post = Scene.FindEntityByName("GatePost")
		Check("Joint ConnectedEntity is the entity ID", door:GetComponent("Joint").ConnectedEntity == post.ID)
		Check("GetJointPosition", Near(door:GetJointPosition(), 0, 0.5))
		Check("GetJointPosition without a joint", Scene.FindEntityByName("Crate"):GetJointPosition() == nil)
		Check("HasJoint", door:HasJoint() and not Scene.FindEntityByName("Crate"):HasJoint())
		door:SetComponent("Joint", { MotorMode = "Velocity", MotorTarget = 90 })
		local holder = Scene.CreateEntity("JointHolder")
		holder:AddComponent("Joint", { ConnectedEntity = post })
		Check("Joint ConnectedEntity accepts an entity", holder:GetComponent("Joint").ConnectedEntity == post.ID)
		holder:Destroy()

		-- Runtime-added script.
		local scripted = Scene.CreateEntity("RuntimeScripted")
		scripted:AddComponent("Script", { Script = "Assets/Scripts/Spinner.lua", Properties = { Label = "runtime" } })
		Check("AddComponent Script starts the script", scripted:GetScript() ~= nil and scripted:GetScript().Label == "runtime")
	end

	if frame == 2 then
		-- The batch released Space and the mouse button and moved the mouse by (10, 10) before this frame.
		Check("Input.IsKeyReleased", Input.IsKeyReleased("Space") and not Input.IsKeyDown("Space"))
		Check("Input.IsMouseButtonReleased", Input.IsMouseButtonReleased("Left") and not Input.IsMouseButtonDown("Left"))
		Check("Input.GetMouseDelta", Input.GetMouseDelta() == Vec2(10, 10) and Input.GetMousePosition() == Vec2(330, 250))

		-- One physics step applied the force (60 N * 1/60 s / 2 kg = +0.5 m/s) and torque.
		local crate = Scene.FindEntityByName("Crate")
		Check("AddForce", Near(crate:GetLinearVelocity().z, 1.5, 0.02))
		Check("AddTorque", crate:GetAngularVelocity().y > 1.1)
	end

	if frame == 30 then
		local hit = Physics.Raycast(Vec3(0, 10, 8), Vec3(0, -1, 0), 50)
		Check("Physics.Raycast hit", hit ~= nil and hit.Entity.Name == "Ground" and Near(hit.Point.y, 0, 0.05) and NearVec(hit.Normal, Vec3(0, 1, 0), 1e-3) and Near(hit.Distance, 10, 0.05))
		Check("Physics.Raycast miss", Physics.Raycast(Vec3(0, 10, 8), Vec3(0, 1, 0), 50) == nil)
		Check("Physics.Raycast ignore", Physics.Raycast(Vec3(0, 10, 8), Vec3(0, -1, 0), 50, Scene.FindEntityByName("Ground")) == nil)
		Check("Kinematic/dynamic motion", Scene.FindEntityByName("Crate").Translation.x > self.CrateStart.x)
		Check("Runtime physics body falls", self.RuntimeBox.Translation.y < 4)
		Check("Hinge motor opens the gate", Scene.FindEntityByName("GateDoor"):GetJointPosition() > 20)
		Check("Joint breaks and calls OnJointBreak", FeatureTestEvents.JointBreak == 1 and FeatureTestEvents.JointBrokeWith == "world")
		Check("Broken joint component is removed", not Scene.FindEntityByName("WeakLink"):HasComponent("Joint"))
		Check("HasJoint after a break", not Scene.FindEntityByName("WeakLink"):HasJoint())
		Check("Continuous body stops at a thin wall", Scene.FindEntityByName("Bullet").Translation.x < 15)
		Check("RigidBody Layer and Continuous fields", Scene.FindEntityByName("GhostBox"):GetComponent("RigidBody").Layer == "Ghost" and Scene.FindEntityByName("Bullet"):GetComponent("RigidBody").Continuous)
		Check("Physics.GetLayers", table.concat(Physics.GetLayers(), ",") == "Default,Ghost,Debris")
	end

	if frame == 150 then
		local events = FeatureTestEvents
		Check("Trigger enter", (events.TriggerEnter or 0) >= 1)
		Check("Trigger exit", (events.TriggerExit or 0) >= 1)
		Check("Collision begin", (events.CollisionBegin or 0) >= 1)
		Check("Ball rests on ground", Near(Scene.FindEntityByName("Ball").Translation.y, 0.5, 0.05))

		-- Layers: Ghost ignores Default (the ground), Debris collides with it.
		local debris = Scene.FindEntityByName("DebrisBox")
		Check("Ignored layer pair passes through", Scene.FindEntityByName("GhostBox").Translation.y < -5)
		Check("Colliding layer pair rests", Near(debris.Translation.y, 0.5, 0.05))
		local top, down = Vec3(16, 10, 12), Vec3(0, -1, 0)
		Check("Raycast Layers option", Physics.Raycast(top, down, 50, { Layers = { "Default" } }).Entity.Name == "Ground")
		local all = Physics.Raycast(top, down, 50, { All = true })
		Check("Raycast All", #all == 2 and all[1].Entity == debris and all[2].Entity.Name == "Ground")
		Check("Raycast Ignore option", Physics.Raycast(top, down, 50, { Ignore = debris }).Entity.Name == "Ground")
		local sphere = Physics.SphereCast(top, 0.25, down, 50)
		Check("Physics.SphereCast", sphere ~= nil and sphere.Entity == debris and Near(sphere.Distance, 8.75, 0.06) and NearVec(sphere.Normal, Vec3(0, 1, 0), 0.05))
		Check("SphereCast All", #Physics.SphereCast(top, 0.25, down, 50, { All = true }) == 2)
		local box = Physics.BoxCast(top, Vec3(1, 0.1, 0.1), Quat.AngleAxis(math.rad(90), Vec3(0, 0, 1)), down, 50)
		Check("Physics.BoxCast", box ~= nil and box.Entity == debris and Near(box.Distance, 8.0, 0.06))
		local overlap = Physics.OverlapSphere(debris.Translation, 0.1)
		Check("Physics.OverlapSphere", #overlap == 1 and overlap[1] == debris)
		Check("Physics.OverlapBox Layers", #Physics.OverlapBox(Vec3(16, 0, 12), Vec3(1, 1, 1), Quat.Identity(), { Layers = { "Debris" } }) == 1)
		Check("Overlap IncludeTriggers", #Physics.OverlapSphere(Vec3(3, 3, 0), 0.1) == 0 and Physics.OverlapSphere(Vec3(3, 3, 0), 0.1, { IncludeTriggers = true })[1].Name == "Zone")
		ExpectError("Query with an unknown layer", function() return Physics.Raycast(top, down, 50, { Layers = { "Nope" } }) end, "unknown physics layer 'Nope'")
		Check("Projectile destroyed itself", events.ProjectilesDestroyed == 1)
		Check("Projectile hit something", (events.ProjectileHits or 0) >= 1)
		Check("Spinner turned", Scene.FindEntityByName("Turntable"):GetScript().Turns > 0.5)
		Check("Late update runs", Scene.FindEntityByName("Turntable"):GetScript().LateUpdates == frame - 1)
		Check("Driver late update runs", self.Late == frame - 1)
		Check("Time advances", Time.GetFrame() == frame - 1 and Time.GetElapsed() > 2.0)
		Check("Audio clip finished", not Scene.FindEntityByName("Speaker"):IsAudioPlaying())
		-- Lift the resting ball off the ground to end the contact.
		local ball = Scene.FindEntityByName("Ball")
		ball.Translation = ball.Translation + Vec3(0, 5, 0)
	end

	if frame == 160 then
		Check("Collision end", (FeatureTestEvents.CollisionEnd or 0) >= 1)
		Expect("Game.Quit", function() Game.Quit() end)
		FeatureTest.Quit = true
		FeatureTest.Phase = "done"
	end
end

function Driver:OnLateUpdate(dt)
	self.Late = self.Late + 1
end

function Driver:OnDestroy()
	FeatureTestEvents.DriverDestroyed = true
end

return Driver
