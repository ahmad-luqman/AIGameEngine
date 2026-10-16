-- Feature test driver: exercises every component and the entire Lua scripting API.
--
-- Run headless by ctest (see Tests/Data/FeatureTest/FeatureTest.batch.json). The batch presses Space and the
-- left mouse button for the first frame, releases them and moves the mouse, plays ~3 seconds (failing on any
-- script error), then calls FeatureTest.Report(), which raises an error listing every failed check.

FeatureTest = { Checks = 0, Failures = {}, Phase = "create", Quit = false }
FeatureTestEvents = FeatureTestEvents or {}

local Checks = require("Assets.Scripts.Lib.Checks").New(FeatureTest, "FeatureTest")
local Check, Expect, ExpectError = Checks.Check, Checks.Expect, Checks.ExpectError

local function Near(a, b, epsilon)
	return math.abs(a - b) <= (epsilon or 1e-4)
end

local function NearVec(a, b, epsilon)
	return Near(a.x, b.x, epsilon) and Near(a.y, b.y, epsilon) and Near(a.z, b.z, epsilon)
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
	for _, kind in ipairs({ "PointLight", "SpotLight" }) do
		local lamp = Scene.GetEntitiesWith(kind)[1]
		local original = lamp:GetComponent(kind)
		lamp:SetComponent(kind, { CastShadows = true, ShadowSoftness = 2.5 })
		local shadow = lamp:GetComponent(kind)
		Check(kind .. " shadow controls", shadow.CastShadows and shadow.ShadowSoftness == 2.5)
		lamp:SetComponent(kind, { CastShadows = false, ShadowSoftness = 0 })
		shadow = lamp:GetComponent(kind)
		Check(kind .. " shadow disable", not shadow.CastShadows and shadow.ShadowSoftness == 0)
		Check(kind .. " negative softness rejected", not pcall(function() lamp:SetComponent(kind, { ShadowSoftness = -1 }) end))
		Check(kind .. " numeric shadow flag rejected", not pcall(function() lamp:SetComponent(kind, { CastShadows = 1 }) end))
		lamp:SetComponent(kind, original)
	end
	Check("HasComponent", probe:HasComponent("PointLight") and not probe:HasComponent("SpotLight"))
	probe:RemoveComponent("PointLight")
	Check("RemoveComponent", not probe:HasComponent("PointLight") and probe:GetComponent("PointLight") == nil)
	ExpectError("Unknown component", function() probe:AddComponent("Teleporter") end, "unknown component")
	ExpectError("Invalid field", function() probe:AddComponent("RigidBody", { Mas = 1 }) end, "Mas")
	ExpectError("Core component removal", function() probe:RemoveComponent("Transform") end, "cannot be removed")
	ExpectError("SetComponent on missing component", function() probe:SetComponent("Camera", {}) end, "no Camera")

	-- Every component type round-trips through Lua.
	for _, name in ipairs({ "Camera", "Mesh", "Animator", "Material", "DirectionalLight", "PointLight", "SpotLight", "SkyLight", "PostProcess", "RigidBody",
		"BoxCollider", "SphereCollider", "CapsuleCollider", "MeshCollider", "Joint", "CharacterController", "AudioSource", "AudioListener", "ParticleEmitter", "UIRect", "UIImage", "UIText", "UIButton", "UISlider", "UICheckbox", "Prefab" }) do
		local holder = Scene.CreateEntity("Holder" .. name)
		Expect("AddComponent " .. name, function() holder:AddComponent(name) end)
		local data = holder:GetComponent(name)
		Expect("SetComponent round trip " .. name, function() holder:SetComponent(name, data) end)
		holder:Destroy()
	end

	local rig = Scene.FindEntityByName("AnimationRig")
	Check("GetAnimationClips", #rig:GetAnimationClips() == 3)
	Check("GetAnimationDuration", rig:GetAnimationDuration() == 2)
	Check("IsAnimationPlaying", rig:IsAnimationPlaying())
	rig:PauseAnimation()
	Check("PauseAnimation", not rig:IsAnimationPlaying())
	Check("SetAnimationTime", rig:SetAnimationTime(1))
	Check("GetAnimationTime", rig:GetAnimationTime() == 1)
	rig:StopAnimation()
	Check("StopAnimation", not rig:IsAnimationPlaying() and rig:GetAnimationTime() == 0)
	Check("PlayAnimation", rig:PlayAnimation("Bend"))
	ExpectError("Animation clip numeric type", function() rig:PlayAnimation(5) end, "clip must be a string or nil")
	ExpectError("Animation clip boolean type", function() rig:PlayAnimation(true) end, "clip must be a string or nil")
	Check("CrossFadeAnimation", rig:CrossFadeAnimation("Step", .5))
	Check("BlendAnimations", rig:BlendAnimations("Bend", "Cubic", .5))
	Check("Animation missing clip", not rig:PlayAnimation("Missing"))
	Check("Animation bad time", not rig:SetAnimationTime(-1))
	ExpectError("Animation pause type", function() rig:PauseAnimation(1) end, "paused must be")
	ExpectError("Animation speed validation", function() rig:SetComponent("Animator", { Speed = -1 }) end, "Speed must be")

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
	Expect("UI options and images", function()
		UI.Text("Anchored", -20, -20, { Size = 24, Anchor = "BottomRight", Font = "Assets/Fonts/Karla-Regular.ttf", Shadow = false })
		UI.Text("Wrapped paragraph of text", 0, 200, { Anchor = "Top", Align = "Center", Width = 300 })
		UI.Rect(0, 0, 100, 20, { Color = Vec4(1, 1, 1, 0.5), Anchor = Vec2(0.5, 0.5), Pivot = Vec2(0.5, 0.5) })
		UI.Image("Assets/Textures/UIPanel.png", 20, 20, 64, 64)
		UI.Image("Assets/Textures/UIPanel.png", 20, 20, 200, 80, { Slice = Vec4(10, 10, 10, 10), SliceScale = 2, Tint = Vec4(1, 1, 1, 0.8), Anchor = "TopLeft" })
	end)
	ExpectError("UI unknown option", function() UI.Rect(0, 0, 1, 1, { Colour = Vec4(1) }) end, "unknown option 'Colour'")
	ExpectError("UI unknown anchor", function() UI.Text("x", 0, 0, { Anchor = "Middle" }) end, "unknown Anchor 'Middle'")
	ExpectError("UI.Image without a texture", function() UI.Image("", 0, 0, 1, 1) end, "texture path is empty")
	ExpectError("UI non-finite position", function() UI.Rect(0 / 0, 0, 1, 1) end, "x must be between")
	ExpectError("UI out-of-range font size", function() UI.Text("x", 0, 0, 1000) end, "size must be between 4 and 512")

	-- UI components: laid out from the parent's rect (the HUD is inset by 24 units from the canvas edges).
	local function RectIs(rect, x, y, w, h)
		return rect ~= nil and Near(rect.x, x) and Near(rect.y, y) and Near(rect.z, w) and Near(rect.w, h)
	end
	local hud = Scene.FindEntityByName("HUD")
	local panel = Scene.FindEntityByName("ScorePanel")
	local label = Scene.FindEntityByName("ScoreLabel")
	local banner = Scene.FindEntityByName("Banner")
	Check("UI.GetRect stretched", RectIs(UI.GetRect(hud), 24, 24, canvas.x - 48, canvas.y - 48))
	Check("UI.GetRect anchored to a corner", RectIs(UI.GetRect(panel), canvas.x - 24 - 360, 24, 360, 120))
	Check("UI.GetRect nested and inset", RectIs(UI.GetRect(label), canvas.x - 24 - 340, 36, 320, 96))
	Check("UI.GetRect bottom bar", RectIs(UI.GetRect(banner), 24, canvas.y - 24 - 64, canvas.x - 48, 64))
	Check("UI.GetRect without a UIRect fills the parent", RectIs(UI.GetRect(Scene.FindEntityByName("BannerText")), 24, canvas.y - 88, canvas.x - 48, 64))
	Check("UI.GetRect hidden", UI.GetRect(Scene.FindEntityByName("HiddenMenu")) == nil)
	Check("UI.GetRect without UI components", UI.GetRect(Scene.FindEntityByName("Sun")) == nil)
	local text = label:GetComponent("UIText")
	Check("UIText fields", text.Text == "SCORE 1250" and text.Font == "Assets/Fonts/Karla-Regular.ttf" and text.HorizontalAlign == "Right" and text.VerticalAlign == "Middle")
	local image = panel:GetComponent("UIImage")
	Check("UIImage fields", image.Texture == "Assets/Textures/UIPanel.png" and image.Slice[1] == 10 and image.SliceScale == 2)
	local rect = panel:GetComponent("UIRect")
	rect.Position = Vec2(-10, 30)
	panel:SetComponent("UIRect", rect)
	Check("UIRect SetComponent moves the rect", RectIs(UI.GetRect(panel), canvas.x - 24 - 370, 54, 360, 120))
	Check("UIRect moves its children", RectIs(UI.GetRect(label), canvas.x - 24 - 350, 66, 320, 96))
	rect.Position = Vec2(0, 0)
	panel:SetComponent("UIRect", rect)
	ExpectError("UIRect anchors out of range", function() panel:SetComponent("UIRect", { AnchorMin = Vec2(-1, 0) }) end, "AnchorMin.x must be between 0 and 1")
	ExpectError("UIRect anchors crossed", function() panel:SetComponent("UIRect", { AnchorMin = Vec2(1, 1), AnchorMax = Vec2(0, 1) }) end, "AnchorMax.x must not be less than AnchorMin.x")
	ExpectError("UIText font size", function() label:SetComponent("UIText", { FontSize = 1000 }) end, "FontSize must be between 4 and 512")
	ExpectError("UIText alignment", function() label:SetComponent("UIText", { HorizontalAlign = "Middle" }) end, "HorizontalAlign")
	ExpectError("UIImage slice scale", function() panel:SetComponent("UIImage", { SliceScale = 0 }) end, "SliceScale must be between")
	Check("Rejected UI patches change nothing", RectIs(UI.GetRect(panel), canvas.x - 24 - 360, 24, 360, 120)
		and label:GetComponent("UIText").FontSize == 52 and label:GetComponent("UIText").HorizontalAlign == "Right" and panel:GetComponent("UIImage").SliceScale == 2)

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
	local music = Scene.FindEntityByName("Music")
	local musicSource = music:GetComponent("AudioSource")
	Check("AudioSource Bus and Stream", musicSource.Bus == "Music" and musicSource.Stream == true and speaker:GetComponent("AudioSource").Bus == "SFX")
	Check("Streamed music plays", music:IsAudioPlaying())
	Check("Audio.GetBusVolume", Audio.GetBusVolume("Master") == 1 and Audio.GetBusVolume("Music") == 1)
	Check("Audio.SetBusVolume", Audio.SetBusVolume("SFX", 0.25) == true and Near(Audio.GetBusVolume("SFX"), 0.25) and Audio.GetBusVolume("UI") == 1)
	Audio.SetBusVolume("SFX", 1)
	Check("Audio.PlayOneShot on a bus", Audio.PlayOneShot("Assets/Audio/Beep.wav", nil, 0.5, "UI"))
	ExpectError("Audio bus name", function() Audio.SetBusVolume("Voice", 1) end, "Audio.SetBusVolume: unknown audio bus 'Voice' (valid: Master, Music, SFX, UI)")
	ExpectError("Audio.GetBusVolume bus name", function() Audio.GetBusVolume("music") end, "Audio.GetBusVolume: unknown audio bus 'music'")
	ExpectError("Audio.PlayOneShot bus name", function() Audio.PlayOneShot("Assets/Audio/Beep.wav", nil, 1, "Voice") end, "Audio.PlayOneShot: unknown audio bus")
	ExpectError("Audio bus volume range", function() Audio.SetBusVolume("Music", -1) end, "Audio.SetBusVolume: volume must be a finite number >= 0")
	ExpectError("Audio bus volume finite", function() Audio.SetBusVolume("Music", 0 / 0) end, "volume must be a finite number >= 0")
	ExpectError("Audio.PlayOneShot volume", function() Audio.PlayOneShot("Assets/Audio/Beep.wav", nil, -1) end, "Audio.PlayOneShot: volume must be a finite number >= 0 (got -1)")
	ExpectError("Audio.PlayOneShot volume finite", function() Audio.PlayOneShot("Assets/Audio/Beep.wav", nil, 0 / 0) end, "Audio.PlayOneShot: volume must be a finite number >= 0")
	ExpectError("PlayAudio needs an AudioSource", function() Scene.FindEntityByName("HUD"):PlayAudio() end, "PlayAudio needs the AudioSource component on 'HUD'")
	ExpectError("StopAudio needs an AudioSource", function() Scene.FindEntityByName("HUD"):StopAudio() end, "StopAudio needs the AudioSource component")
	ExpectError("AudioSource volume range", function() music:SetComponent("AudioSource", { Volume = -1 }) end, "Volume must be a finite number >= 0 (got -1)")
	ExpectError("AudioSource pitch range", function() music:SetComponent("AudioSource", { Pitch = 0 }) end, "Pitch must be a finite number > 0")
	ExpectError("AudioSource bus name", function() music:SetComponent("AudioSource", { Bus = "Voice" }) end, "invalid value 'Voice'")
	Check("Rejected AudioSource patches change nothing", Near(music:GetComponent("AudioSource").Volume, 0.2) and music:GetComponent("AudioSource").Pitch == 1 and music:GetComponent("AudioSource").Bus == "Music")

	-- Particles. Sparks and Smoke play from the start; Confetti only emits what scripts ask for.
	local sparks = Scene.FindEntityByName("Sparks")
	Check("ParticleEmitter PlayOnStart", sparks:IsEmittingParticles() and sparks:GetParticleCount() == 0)
	local confetti = Scene.FindEntityByName("Confetti")
	Check("ParticleEmitter without PlayOnStart", not confetti:IsEmittingParticles())
	Check("EmitParticles", confetti:EmitParticles(30) == 30 and confetti:GetParticleCount() == 30)
	confetti:PlayParticles()
	Check("PlayParticles", confetti:IsEmittingParticles())
	confetti:StopParticles()
	Check("StopParticles keeps live particles", not confetti:IsEmittingParticles() and confetti:GetParticleCount() == 30)
	local smoke = Scene.FindEntityByName("Smoke")
	smoke:EmitParticles(5)
	smoke:StopParticles(true)
	Check("StopParticles(true) clears", smoke:GetParticleCount() == 0 and not smoke:IsEmittingParticles())
	smoke:PlayParticles()
	Check("ParticleEmitter fields", sparks:GetComponent("ParticleEmitter").Shape == "Cone" and sparks:GetComponent("ParticleEmitter").Blend == "Additive")
	local crate = Scene.FindEntityByName("Crate")
	ExpectError("EmitParticles without a ParticleEmitter", function() crate:EmitParticles(1) end, "needs the ParticleEmitter component")
	ExpectError("PlayParticles without a ParticleEmitter", function() crate:PlayParticles() end, "needs the ParticleEmitter component")
	ExpectError("StopParticles without a ParticleEmitter", function() crate:StopParticles() end, "needs the ParticleEmitter component")
	Check("Particle queries without a ParticleEmitter", not crate:IsEmittingParticles() and crate:GetParticleCount() == 0)
	ExpectError("EmitParticles with a negative count", function() confetti:EmitParticles(-1) end, "between 0 and")
	ExpectError("StopParticles with a non-boolean clear", function() confetti:StopParticles(1) end, "clear must be")
	ExpectError("math.randomseed with a fraction", function() math.randomseed(1.5) end, "no integer representation")
	ExpectError("ParticleEmitter validation", function() confetti:SetComponent("ParticleEmitter", { LifetimeMin = 0 }) end, "LifetimeMin must be between")

	-- Input names are validated.
	ExpectError("Unknown key name", function() Input.IsKeyDown("Hyperspace") end, "unknown key")
	ExpectError("Unknown mouse button", function() Input.IsMouseButtonDown("Thumb") end, "unknown mouse button")

	-- Sandbox: no file access from scripts.
	Check("Sandbox: dofile removed", dofile == nil and loadfile == nil and io == nil and os == nil)
	local util = require("Assets.Scripts.Lib.FeatureUtil")
	Check("require project module", util.Triple(3) == 9)
end

-- Timers, coroutines, events and scene information. Scene switching and Storage are exercised by the
-- SceneSwitchA/B scenes (SceneSwitchManager.lua), which the batch plays after this scene.
local function TestGameplay(self)
	local g = { Fired = {}, Every = 0, Trace = {} }
	self.Gameplay = g
	self.AfterTimer = Timer.After(0.25, function(handle)
		table.insert(g.Fired, "after@" .. Time.GetFrame())
		g.AfterHandle = handle
	end)
	self.EveryTimer = Timer.Every(0.1, function() g.Every = g.Every + 1 end, 3)
	local cancelled = Timer.After(0.05, function() table.insert(g.Fired, "cancelled") end)
	Check("TaskHandle:Cancel", cancelled:Cancel() and not cancelled:IsActive() and not cancelled:Cancel())
	Check("TaskHandle:IsActive", self.AfterTimer:IsActive() and self.EveryTimer:IsActive())
	ExpectError("Timer.After rejects negative delays", function() Timer.After(-1, function() end) end, "finite number >= 0")
	ExpectError("Timer.Every needs a positive interval", function() Timer.Every(0, function() end) end, "> 0")
	ExpectError("Timer.After needs a function", function() Timer.After(1, "nope") end, "expected a function")

	-- Coroutines run right away, up to their first wait.
	self.Routine = self:StartCoroutine(function(label)
		table.insert(g.Trace, label .. "@" .. Time.GetFrame())
		WaitFrames(2)
		table.insert(g.Trace, "frames@" .. Time.GetFrame())
		Wait(0.2)
		table.insert(g.Trace, "seconds@" .. Time.GetFrame())
		coroutine.yield()
		table.insert(g.Trace, "yield@" .. Time.GetFrame())
	end, "start")
	Check("StartCoroutine runs to the first wait", #g.Trace == 1 and self.Routine:IsActive())
	local looping = self:StartCoroutine(function() while true do Wait(1) end end)
	Check("StopCoroutine", self:StopCoroutine(looping) and not looping:IsActive())
	Check("StopCoroutine of a finished coroutine", self:StopCoroutine(self:StartCoroutine(function() end)) == false)
	ExpectError("StopCoroutine only stops coroutines", function() self:StopCoroutine(self.AfterTimer) end, "is a timer")
	ExpectError("Wait outside a coroutine", function() Wait(1) end, "can only be called in a coroutine")
	ExpectError("WaitFrames needs a whole number", function() WaitFrames(0) end, "integer >= 1")

	-- Events are synchronous; Once handlers end after their first call.
	local heard = {}
	local handle = Events.On("FeatureTest.Ping", function(a, b) table.insert(heard, a .. b) end)
	local once = Events.Once("FeatureTest.Ping", function() table.insert(heard, "once") end)
	Check("Events.Emit", Events.Emit("FeatureTest.Ping", "x", 1) == 2 and Events.Emit("FeatureTest.Ping", "y", 2) == 1 and table.concat(heard, ",") == "x1,once,y2")
	Check("EventHandle", not once:IsActive() and handle:Cancel() and not handle:IsActive() and Events.Emit("FeatureTest.Ping", "z", 3) == 0)
	Events.On("FeatureTest.Later", function(value) g.Later = value end)
	Timer.After(0.1, function() Events.Emit("FeatureTest.Later", 42) end)
	ExpectError("Events.On needs a name", function() Events.On("", function() end) end, "non-empty string")

	Check("Scene.GetName", Scene.GetName() == "FeatureTest")
	Check("Scene.GetPath", Scene.GetPath() == "Assets/Scenes/FeatureTest.bscene")
	ExpectError("Scene.Load missing file", function() Scene.Load("Assets/Scenes/Missing.bscene") end, "cannot read")
	ExpectError("Scene.Load outside the project", function() Scene.Load("../Other.bscene") end, "inside the project")
	ExpectError("DontDestroyOnLoad needs a root", function() Scene.DontDestroyOnLoad(Scene.FindEntityByName("TexturedCube")) end, "not a root entity")
end

-- Widgets: the scene's menu (PlayButton, VolumeSlider, MusicCheckbox call this driver), immediate-mode widgets,
-- focus and gamepads. The batch clicks and navigates them after the run; FeatureTest.CheckWidgets() checks what
-- happened.
local function TestWidgets(self)
	self.WidgetLog = {}
	local function RectIs(rect, x, y, w, h)
		return rect ~= nil and Near(rect.x, x) and Near(rect.y, y) and Near(rect.z, w) and Near(rect.w, h)
	end
	local play = Scene.FindEntityByName("PlayButton")
	local volume = Scene.FindEntityByName("VolumeSlider")
	local music = Scene.FindEntityByName("MusicCheckbox")
	-- The panel sits at the HUD's left middle: (24, 410), 360 x 260.
	Check("Widget rects", RectIs(UI.GetRect(play), 44, 430, 320, 64) and RectIs(UI.GetRect(volume), 44, 514, 320, 32) and RectIs(UI.GetRect(music), 44, 566, 320, 40))
	local button = play:GetComponent("UIButton")
	Check("UIButton fields", button.Interactable and button.OnClick == "OnPlayClicked" and button.Target == self.Entity.ID and button.NormalTint[1] < 1)
	local slider = volume:GetComponent("UISlider")
	Check("UISlider fields", Near(slider.Value, 0.5) and slider.Min == 0 and slider.Max == 1 and Near(slider.Step, 0.1) and slider.HandleWidth == 20 and slider.OnValueChanged == "OnSettingChanged")
	local checkbox = music:GetComponent("UICheckbox")
	Check("UICheckbox fields", checkbox.Checked == false and checkbox.OnValueChanged == "OnSettingChanged" and #checkbox.CheckColor == 4)
	ExpectError("UISlider empty range", function() volume:SetComponent("UISlider", { Min = 1 }) end, "Max must be greater than Min")
	ExpectError("UISlider value out of range", function() volume:SetComponent("UISlider", { Value = 2 }) end, "Value must be between 0 and 1")
	ExpectError("UIButton callback name", function() play:SetComponent("UIButton", { OnClick = "Start Game" }) end, "OnClick must be the name of a script method")
	Check("Rejected widget patches change nothing", Near(volume:GetComponent("UISlider").Value, 0.5) and play:GetComponent("UIButton").OnClick == "OnPlayClicked")

	-- Immediate-mode widgets: nothing happened to them yet.
	Check("UI.Button", UI.Button("Probe button", 20, 20, 100, 40) == false)
	Check("UI.Checkbox", UI.Checkbox("Probe checkbox", true, 20, 80, 200, 40, { CheckColor = Vec4(1, 0, 0, 1) }) == true)
	Check("UI.Slider", UI.Slider("", 0.25, 0, 1, 20, 140, 200, 30, { Id = "Probe slider", Step = 0.25, HandleWidth = 10 }) == 0.25)
	ExpectError("UI.Button duplicate id", function() UI.Button("Probe button", 0, 0, 10, 10) end, "already uses the id 'Probe button'")
	ExpectError("UI.Slider empty range", function() UI.Slider("x", 0, 1, 1, 0, 0, 10, 10) end, "max must be greater than min")
	ExpectError("UI.Checkbox unknown option", function() UI.Checkbox("y", false, 0, 0, 10, 10, { Step = 1 }) end, "unknown option 'Step'")
	Check("UI.GetFocus", UI.GetFocus() == nil)
	UI.SetFocus(play)
	Check("UI.SetFocus entity", UI.GetFocus() == play)
	UI.SetFocus("Probe button")
	Check("UI.SetFocus id", UI.GetFocus() == "Probe button")
	UI.SetFocus(nil)
	ExpectError("UI.SetFocus non-widget", function() UI.SetFocus(Scene.FindEntityByName("HUD")) end, "has no UIButton, UISlider or UICheckbox")
	Check("UI.IsPointerOverWidget", UI.IsPointerOverWidget() == false)
	UI.SetNavigationEnabled(false)
	Check("UI.SetNavigationEnabled", UI.IsNavigationEnabled() == false)
	UI.SetNavigationEnabled(true)
	Check("UI.IsNavigationEnabled", UI.IsNavigationEnabled() == true)

	-- Gamepads: only types here. The batch runs headless (no device polling) and injects one later; the GPU tests run
	-- this scene in the runtime, where a developer's own gamepad may be connected.
	Check("Input.GetGamepads", type(Input.GetGamepads()) == "table")
	Check("Input.IsGamepadConnected", type(Input.IsGamepadConnected(0)) == "boolean")
	Check("Input.GetGamepadAxis", type(Input.GetGamepadAxis(0, "LeftX")) == "number")
	Check("Input.IsGamepadButtonDown", type(Input.IsGamepadButtonDown(0, "A")) == "boolean" and type(Input.IsGamepadButtonPressed(0, "Start")) == "boolean" and type(Input.IsGamepadButtonReleased(0, "B")) == "boolean")
	Check("Input.GetGamepadName", Input.GetGamepadName(15) == nil or type(Input.GetGamepadName(15)) == "string")
	ExpectError("Gamepad index range", function() Input.IsGamepadConnected(16) end, "integer from 0 to 15")
	ExpectError("Gamepad button name", function() Input.IsGamepadButtonDown(0, "Turbo") end, "unknown gamepad button")
	ExpectError("Gamepad axis name", function() Input.GetGamepadAxis(0, "Wheel") end, "unknown gamepad axis")
end

-- Called by the batch after it clicked PLAY (a click does not focus), pressed Down twice (the first shows the focus
-- on PLAY, the second moves it to the slider), pushed the slider right and the stick down to the checkbox with an
-- injected gamepad, toggled it with A, clicked the immediate-mode button and disconnected the gamepad.
function FeatureTest.CheckWidgets()
	local driver = Scene.FindEntityByName("FeatureTestDriver"):GetScript()
	Check("Widget callbacks", table.concat(driver.WidgetLog, ",") == "PlayButton,VolumeSlider=0.6,MusicCheckbox=true")
	Check("Widget values", Near(Scene.FindEntityByName("VolumeSlider"):GetComponent("UISlider").Value, 0.6) and Scene.FindEntityByName("MusicCheckbox"):GetComponent("UICheckbox").Checked)
	Check("Volume slider drives the Music bus", Near(Audio.GetBusVolume("Music"), 0.6))
	Check("A click drops the focus", UI.GetFocus() == nil)
	Check("Immediate-mode button clicked", FeatureTest.ImmediateClicks == 1)
	Check("Gamepad connected callback", FeatureTestEvents.GamepadConnected == 0 and FeatureTestEvents.GamepadName == "Injected Gamepad" and FeatureTestEvents.GamepadCount == 1)
	Check("Gamepad disconnected callback", FeatureTestEvents.GamepadDisconnected == 0 and not Input.IsGamepadConnected(0))
	return FeatureTest.Report()
end

-- ---------------------------------------------------------------------------------------------
-- Lifecycle
-- ---------------------------------------------------------------------------------------------

local function TestGraphics()
	local original = Graphics.GetSettings()
	Check("Graphics.GetSettings", type(original.VSync) == "boolean" and original.RenderScale > 0)
	Graphics.SetSettings({ VSync = false, RenderScale = 0.5, ShadowQuality = "Low", SSAOQuality = "Off" })
	local changed = Graphics.GetSettings()
	Check("Graphics.SetSettings", not changed.VSync and changed.RenderScale == 0.5 and changed.ShadowQuality == "Low" and changed.SSAOQuality == "Off")
	ExpectError("Graphics invalid quality", function() Graphics.SetSettings({ShadowQuality="Ultra"}) end, "ShadowQuality")
	Graphics.SetSettings(original)
	local entity = Scene.FindEntityByName("PostProcessing")
	local before = entity:GetComponent("PostProcess")
	entity:SetComponent("PostProcess", { BloomEnabled=true, FXAAEnabled=true, AutoExposure=true, MinExposure=0.25, MaxExposure=4 })
	local post = entity:GetComponent("PostProcess")
	Check("PostProcess settings", post.BloomEnabled and post.FXAAEnabled and post.AutoExposure and post.MinExposure==0.25 and post.MaxExposure==4)
	ExpectError("PostProcess invalid exposure", function() entity:SetComponent("PostProcess", {MinExposure=8,MaxExposure=1}) end, "MinExposure")
	entity:SetComponent("PostProcess", before)
end

function Driver:OnCreate()
	self.Frame = 0
	self.Late = 0
	TestGraphics()
	TestMath()
	TestEntities(self)
	TestSystems(self)
	TestGameplay(self)
	TestWidgets(self)
	FeatureTest.Phase = "running"
end

function Driver:OnUpdate(dt)
	self.Frame = self.Frame + 1
	local frame = self.Frame
	Check("Time.GetDelta", Near(Time.GetDelta(), dt))

	-- Immediate-mode UI every frame (the GPU goldens show it): an anchored nine-slice panel with TTF text.
	UI.Image("Assets/Textures/UIPanel.png", 24, -112, 300, 96, { Anchor = "BottomLeft", Pivot = Vec2(0, 1), Slice = Vec4(10, 10, 10, 10), SliceScale = 2 })
	UI.Text("LIVES 3", 174, -160, { Anchor = "BottomLeft", Pivot = Vec2(0.5, 0.5), Size = 44, Font = "Assets/Fonts/Karla-Regular.ttf", Color = Vec4(0.6, 0.9, 1, 1) })

	-- The batch turns these on after the run (the GPU goldens do not show them).
	if FeatureTest.ShowImmediateWidgets then
		if UI.Button("Immediate button", 20, 700, 240, 60) then
			FeatureTest.ImmediateClicks = (FeatureTest.ImmediateClicks or 0) + 1
		end
	end

	if frame == 1 then
		-- The batch holds Space and the left mouse button from the first frame.
		Check("Input.IsKeyPressed", Input.IsKeyPressed("Space"))
		Check("Input.IsKeyDown", Input.IsKeyDown("Space") and not Input.IsKeyDown("A"))
		Check("Input.IsMouseButtonPressed", Input.IsMouseButtonPressed("Left") and Input.IsMouseButtonDown("Left"))
		Check("Input.GetMousePosition", Input.GetMousePosition() == Vec2(320, 240))
		Check("Input.GetMouseScroll", Input.GetMouseScroll() == Vec2(0, 0))
		Check("No release on the press frame", not Input.IsKeyReleased("Space") and not Input.IsMouseButtonReleased("Left"))

		-- Character controller: the hero walks towards a step, jumps on frame 25 and walks again from frame 100.
		local hero = Scene.FindEntityByName("Hero")
		local controller = hero:GetComponent("CharacterController")
		Check("CharacterController fields", controller.SlopeLimit == 40 and Near(controller.StepHeight, 0.35, 1e-6) and controller.Layer == "Default")
		ExpectError("Move without a CharacterController", function() Scene.FindEntityByName("Crate"):Move(Vec3(1, 0, 0)) end, "needs the CharacterController component")
		Check("IsGrounded without a character", not Scene.FindEntityByName("Crate"):IsGrounded() and Scene.FindEntityByName("Crate"):GetGroundNormal() == nil)
		ExpectError("Move with a non-finite velocity", function() hero:Move(Vec3(0 / 0, 0, 0)) end, "must be finite")
		hero:Move(Vec3(2, 0, 0))
		-- Two more characters (no meshes, so the renders are unchanged): from frame 2, once physics has built
		-- them, one walks into the other and pushes it.
		for i, name in ipairs({ "Walker", "Standee" }) do
			local character = Scene.CreateEntity(name)
			character.Translation = Vec3(-16.5 + 1.5 * i, 1, -18)
			character:AddComponent("CapsuleCollider", {})
			character:AddComponent("CharacterController", {})
		end
		-- 200 m/s covers 3.3 m per step; only the continuous sweep stops it at the 5 cm wall.
		Scene.FindEntityByName("Bullet"):SetLinearVelocity(Vec3(200, 0, 0))
		Check("Destroyed entity is gone", not self.Doomed:IsValid())
		ExpectError("Stale entity access", function() return self.Doomed.Name end, "no longer exists")

		-- An emitter added during play: one burst, then its particles expire (checked on frame 30).
		local puff = Scene.CreateEntity("Puff")
		puff:AddComponent("ParticleEmitter", { BurstCount = 10, Rate = 0, Looping = false, Duration = 0.1, LifetimeMin = 0.2, LifetimeMax = 0.2 })
		self.Puff = puff
		Check("Emitter added during play plays", puff:IsEmittingParticles() and puff:GetParticleCount() == 0)
		-- PlayOnStart = false holds for emitters added during play too (they wait for PlayParticles).
		local trigger = Scene.CreateEntity("Trigger")
		trigger:AddComponent("ParticleEmitter", { PlayOnStart = false, BurstCount = 10 })
		Check("Emitter added during play without PlayOnStart waits", not trigger:IsEmittingParticles())
		trigger:Destroy()

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
		-- Its collider's own material: grippier than the body's 0.5 and a little bouncy.
		dynamic:AddComponent("BoxCollider", { HalfExtents = { 0.25, 0.25, 0.25 }, OverrideMaterial = true, Friction = 0.8, Restitution = 0.25 })
		dynamic:AddComponent("Mesh", { Mesh = "builtin://Cube" })
		local runtimeCollider = dynamic:GetComponent("BoxCollider")
		Check("Collider material override", runtimeCollider.OverrideMaterial and Near(runtimeCollider.Friction, 0.8, 1e-6) and runtimeCollider.Restitution == 0.25)
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

		-- Joint entities: the rung hangs from two rope entities; a third is added at runtime.
		local rung = Scene.FindEntityByName("Rung")
		local ropeLeft = Scene.FindEntityByName("RungRopeLeft")
		Check("Joint BodyEntity is the entity ID", ropeLeft:GetComponent("Joint").BodyEntity == rung.ID)
		Check("HasJoint on a joint entity", ropeLeft:HasJoint() and Scene.FindEntityByName("RungRopeRight"):HasJoint() and not rung:HasJoint())
		local extraRope = Scene.CreateEntity("RungRopeMiddle")
		extraRope:AddComponent("Joint", { Type = "Distance", BodyEntity = rung, ConnectedAnchor = { -18, 4, 0 } })
		Check("Joint BodyEntity accepts an entity", extraRope:GetComponent("Joint").BodyEntity == rung.ID)
		self.ExtraRope = extraRope

		-- Soft limits, cone and six-DOF joints, each knocked sideways and tracked every frame.
		local slider = Scene.FindEntityByName("SoftSlider")
		Check("Joint LimitSpring fields", slider:GetComponent("Joint").LimitSpringFrequency == 2 and Near(slider:GetComponent("Joint").LimitSpringDamping, 0.1, 1e-6))
		slider:SetLinearVelocity(Vec3(4, 0, 0))
		local arm = Scene.FindEntityByName("RagdollArm")
		local armJoint = arm:GetComponent("Joint")
		Check("Joint six-DOF fields", armJoint.Type == "SixDOF" and armJoint.AngularLimitMax[2] == 45 and armJoint.SecondaryAxis[1] == 1)
		Scene.FindEntityByName("ConeLamp"):SetLinearVelocity(Vec3(0, 0, 4))
		arm:SetLinearVelocity(Vec3(0, 0, 4))
		self.SoftPeak = 0
		self.ConeTilt = 0
		self.ArmTilt = 0

		-- Powered ragdoll: a six-DOF limb driven to a target orientation, and a sled whose free axis gets a
		-- velocity motor through SetComponent.
		local limb = Scene.FindEntityByName("PoweredLimb")
		local limbJoint = limb:GetComponent("Joint")
		Check("Joint six-DOF motor fields", limbJoint.AngularMotorMode[3] == "Position" and limbJoint.AngularMotorTarget[3] == 40 and limbJoint.MotorSpringFrequency == 4)
		Check("GetJointRotation", NearVec(limb:GetJointRotation(), Vec3(0, 0, 0), 0.5))
		-- Its own torque cap, still ample to reach the target (checked later).
		limb:SetComponent("Joint", { MotorMaxTorque = 500 })
		Check("Joint MotorMaxTorque", limb:GetComponent("Joint").MotorMaxTorque == 500 and limb:GetComponent("Joint").MotorMaxForce == limbJoint.MotorMaxForce)
		Check("GetJointRotation without a cone or six-DOF joint", door:GetJointRotation() == nil and Scene.FindEntityByName("Crate"):GetJointRotation() == nil)
		local sled = Scene.FindEntityByName("PoweredSled")
		sled:SetComponent("Joint", { LinearMotorMode = { "Velocity", "Off", "Off" }, LinearMotorTarget = { 1, 0, 0 } })
		local sledJoint = sled:GetComponent("Joint")
		Check("Joint FreeLinearAxes and LinearMotorMode", sledJoint.FreeLinearAxes[1] == true and sledJoint.FreeLinearAxes[2] == false and sledJoint.LinearMotorMode[1] == "Velocity")

		-- Runtime-added script.
		local scripted = Scene.CreateEntity("RuntimeScripted")
		scripted:AddComponent("Script", { Script = "Assets/Scripts/Spinner.lua", Properties = { Label = "runtime" } })
		Check("AddComponent Script starts the script", scripted:GetScript() ~= nil and scripted:GetScript().Label == "runtime")
	end

	-- How far each knocked joint gets (degrees from hanging straight down for the cone and the arm).
	if self.SoftPeak then
		local function tilt(name)
			local up = Scene.FindEntityByName(name):GetUp()
			return math.deg(math.acos(math.max(-1, math.min(1, up.y))))
		end
		self.SoftPeak = math.max(self.SoftPeak, Scene.FindEntityByName("SoftSlider").Translation.x + 22)
		self.ConeTilt = math.max(self.ConeTilt, tilt("ConeLamp"))
		self.ArmTilt = math.max(self.ArmTilt, tilt("RagdollArm"))
	end

	if frame == 2 then
		Check("Burst of an emitter added during play", self.Puff:GetParticleCount() == 10)
		Scene.FindEntityByName("Walker"):Move(Vec3(2, 0, 0))
		-- The batch released Space and the mouse button and moved the mouse by (10, 10) before this frame.
		Check("Input.IsKeyReleased", Input.IsKeyReleased("Space") and not Input.IsKeyDown("Space"))
		Check("Input.IsMouseButtonReleased", Input.IsMouseButtonReleased("Left") and not Input.IsMouseButtonDown("Left"))
		Check("Input.GetMouseDelta", Input.GetMouseDelta() == Vec2(10, 10) and Input.GetMousePosition() == Vec2(330, 250))

		-- One physics step applied the force (60 N * 1/60 s / 2 kg = +0.5 m/s) and torque.
		local crate = Scene.FindEntityByName("Crate")
		Check("AddForce", Near(crate:GetLinearVelocity().z, 1.5, 0.02))
		Check("AddTorque", crate:GetAngularVelocity().y > 1.1)
	end

	if frame == 25 then
		local hero = Scene.FindEntityByName("Hero")
		Check("Character IsGrounded", hero:IsGrounded())
		Check("Character GetGroundNormal", hero:GetGroundNormal() ~= nil and NearVec(hero:GetGroundNormal(), Vec3(0, 1, 0), 1e-3))
		Check("Character Move walks", hero.Translation.x > 8.6 and Near(hero:GetLinearVelocity().x, 2, 0.05))
		hero:Move(Vec3(0, 5, 0))
		self.HeroJumpStart = hero.Translation.y
	end

	if frame == 34 then
		local hero = Scene.FindEntityByName("Hero")
		Check("Character Move up jumps", not hero:IsGrounded() and hero:GetGroundNormal() == nil and hero.Translation.y > self.HeroJumpStart + 0.2 and hero:GetLinearVelocity().y > 0)
		-- A held upward Move would jump again on landing.
		hero:Move(Vec3(0, 0, 0))
	end

	if frame == 100 then
		local hero = Scene.FindEntityByName("Hero")
		Check("Character lands", hero:IsGrounded() and Near(hero.Translation.y, self.HeroJumpStart, 0.02))
		hero:Move(Vec3(2, 0, 0))
	end

	if frame == 30 then
		local hit = Physics.Raycast(Vec3(0, 10, 8), Vec3(0, -1, 0), 50)
		Check("Physics.Raycast hit", hit ~= nil and hit.Entity.Name == "Ground" and Near(hit.Point.y, 0, 0.05) and NearVec(hit.Normal, Vec3(0, 1, 0), 1e-3) and Near(hit.Distance, 10, 0.05))
		Check("Physics.Raycast miss", Physics.Raycast(Vec3(0, 10, 8), Vec3(0, 1, 0), 50) == nil)
		Check("Physics.Raycast ignore", Physics.Raycast(Vec3(0, 10, 8), Vec3(0, -1, 0), 50, Scene.FindEntityByName("Ground")) == nil)
		Check("Kinematic/dynamic motion", Scene.FindEntityByName("Crate").Translation.x > self.CrateStart.x)
		Check("Runtime physics body falls", self.RuntimeBox.Translation.y < 4)
		Check("Hinge motor opens the gate", Scene.FindEntityByName("GateDoor"):GetJointPosition() > 20)
		Check("Joint breaks and calls OnJointBreak", FeatureTestEvents["JointBreak:WeakLink"] == 1 and FeatureTestEvents["JointBrokeWith:WeakLink"] == "world")
		Check("Broken joint component is removed", not Scene.FindEntityByName("WeakLink"):HasComponent("Joint"))
		Check("HasJoint after a break", not Scene.FindEntityByName("WeakLink"):HasJoint())
		Check("Joint entity breaks and calls OnJointBreak", FeatureTestEvents["JointBreak:LanternChain"] == 1 and FeatureTestEvents["JointBrokeWith:LanternChain"] == "world"
			and not Scene.FindEntityByName("LanternChain"):HasComponent("Joint") and Scene.FindEntityByName("Lantern").Translation.y < 1.5)
		local rung = Scene.FindEntityByName("Rung")
		Check("Two joint entities hold one body", self.ExtraRope:HasJoint() and Near(rung.Translation.y, 2, 0.05) and Near(rung:GetRight().y, 0, 0.02))
		Check("Soft limit overshoots", self.SoftPeak > 0.6)
		-- A free lamp would swing to about 80 degrees; the solver lets the light lamp overshoot 20 slightly.
		Check("Cone joint limits the swing", self.ConeTilt > 10 and self.ConeTilt < 27)
		Check("Six-DOF joint limits the swing", self.ArmTilt > 20 and self.ArmTilt < 50)
		Check("Six-DOF motor drives the limb", Near(Scene.FindEntityByName("PoweredLimb"):GetJointRotation().z, 40, 4))
		-- 1 m/s for about 0.48 s of play since the motor was set.
		Check("Six-DOF motor drives a free axis", Near(Scene.FindEntityByName("PoweredSled").Translation.x, -36.52, 0.05))
		Check("Continuous body stops at a thin wall", Scene.FindEntityByName("Bullet").Translation.x < 15)
		Check("RigidBody Layer and Continuous fields", Scene.FindEntityByName("GhostBox"):GetComponent("RigidBody").Layer == "Ghost" and Scene.FindEntityByName("Bullet"):GetComponent("RigidBody").Continuous)
		Check("RigidBody AllowSleeping field", Scene.FindEntityByName("Bullet"):GetComponent("RigidBody").AllowSleeping == false and Scene.FindEntityByName("GhostBox"):GetComponent("RigidBody").AllowSleeping == true)
		-- The bullet (AllowSleeping false) never sleeps, a static body never counts as asleep and an entity without a body
		-- has no answer. ScriptingTests and PhysicsTests check bodies that do fall asleep; the Marble Run sample checks that
		-- never-sleeping marbles stay awake at rest.
		Check("Entity:IsSleeping", Scene.FindEntityByName("Bullet"):IsSleeping() == false and Scene.FindEntityByName("Ground"):IsSleeping() == false and self.Entity:IsSleeping() == nil)
		Check("Physics.GetLayers", table.concat(Physics.GetLayers(), ",") == "Default,Ghost,Debris")

		-- 29 particle updates (scripts run before particles each frame): the 20-particle burst and 40/s for
		-- 29/60 s, none old enough to expire.
		local sparkCount = Scene.FindEntityByName("Sparks"):GetParticleCount()
		Check("Particles emit at their rate", sparkCount >= 38 and sparkCount <= 39)
		Check("Particles live their lifetime", Scene.FindEntityByName("Confetti"):GetParticleCount() == 30)
		Check("Particles expire; a one-shot emitter stops", self.Puff:GetParticleCount() == 0 and not self.Puff:IsEmittingParticles())
		self.Puff:Destroy()
	end

	if frame == 150 then
		local events = FeatureTestEvents
		Check("Trigger enter", (events.TriggerEnter or 0) >= 1)
		Check("Trigger exit", (events.TriggerExit or 0) >= 1)
		-- Where bodies started overlapping a trigger, and where trigger overlaps and collisions ended.
		local function zeroImpulseContact(contact)
			return contact ~= nil and Near(contact.Normal:Length(), 1, 0.01) and contact.Impulse == 0
		end
		Check("OnTriggerEnter contact", zeroImpulseContact(events.TriggerEnterContact))
		Check("OnTriggerExit contact", zeroImpulseContact(events.TriggerExitContact))
		Check("Collision begin", (events.CollisionBegin or 0) >= 1)
		Check("Ball rests on ground", Near(Scene.FindEntityByName("Ball").Translation.y, 0.5, 0.05))
		-- The ball (2 kg) lands at about 11 m/s; its normal points up, away from the ground.
		local contact = events["Contact:Ball"]
		Check("OnCollisionBegin contact", contact and Near(contact.Normal.y, 1, 0.01) and Near(contact.Point.y, 0, 0.2) and contact.Impulse > 10)
		local ballBody = Scene.FindEntityByName("Ball"):GetComponent("RigidBody")
		Check("RigidBody combine modes", ballBody.FrictionCombine == "Average" and ballBody.RestitutionCombine == "Min")
		Check("Characters push each other", Scene.FindEntityByName("Standee").Translation.x > -12 and Near(Scene.FindEntityByName("Standee").Translation.x - Scene.FindEntityByName("Walker").Translation.x, 1, 0.1))
		local hero = Scene.FindEntityByName("Hero")
		Check("Character climbs a step", hero.Translation.x > 10 and Near(hero.Translation.y, self.HeroJumpStart + 0.3, 0.03) and hero:IsGrounded())

		-- Layers: Ghost ignores Default (the ground), Debris collides with it.
		local debris = Scene.FindEntityByName("DebrisBox")
		Check("Ignored layer pair passes through", Scene.FindEntityByName("GhostBox").Translation.y < -5)
		Check("Colliding layer pair rests", Near(debris.Translation.y, 0.5, 0.05))
		-- Mesh colliders (no MeshComponent, so the golden renders are unchanged): a static glTF mesh (exact
		-- triangles) and a dynamic cylinder (convex hull) resting on it.
		Check("Dynamic MeshCollider rests on a static one", Near(Scene.FindEntityByName("MeshRock").Translation.y, 2.5, 0.05))
		local meshHit = Physics.Raycast(Vec3(17.8, 10, -10.8), Vec3(0, -1, 0), 50)
		Check("Raycast hits a MeshCollider", meshHit ~= nil and meshHit.Entity.Name == "MeshBlock" and Near(meshHit.Point.y, 2, 0.01))
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
		ExpectError("Query with no layers", function() return Physics.Raycast(top, down, 50, { Layers = {} }) end, "non-empty")
		ExpectError("All on an overlap", function() return Physics.OverlapSphere(top, 1, { All = true }) end, "only applies to casts")
		ExpectError("Cast with a zero direction", function() return Physics.SphereCast(top, 1, Vec3(0, 0, 0), 10) end, "direction must not be zero")
		Check("Cast starting inside a body, moving out", Physics.SphereCast(debris.Translation, 0.1, Vec3(0, 1, 0), 5).Entity == debris)
		Check("Projectile destroyed itself", events.ProjectilesDestroyed == 1)
		Check("Projectile hit something", (events.ProjectileHits or 0) >= 1)
		Check("Spinner turned", Scene.FindEntityByName("Turntable"):GetScript().Turns > 0.5)
		Check("Late update runs", Scene.FindEntityByName("Turntable"):GetScript().LateUpdates == frame - 1)
		Check("Driver late update runs", self.Late == frame - 1)
		Check("Time advances", Time.GetFrame() == frame - 1 and Time.GetElapsed() > 2.0)
		Check("Audio clip finished", not Scene.FindEntityByName("Speaker"):IsAudioPlaying())
		-- Launch the resting ball off the ground to end the contact.
		Scene.FindEntityByName("Ball"):SetLinearVelocity(Vec3(0, 6, 0))
	end

	if frame == 40 then
		-- Game time: 0.25 s is update 15 (Time.GetFrame() 14); the coroutine's Wait(0.2) started on update 2.
		local g = self.Gameplay
		Check("Timer.After fired once", table.concat(g.Fired, ",") == "after@14" and g.AfterHandle == self.AfterTimer and not self.AfterTimer:IsActive())
		Check("Timer.Every count", g.Every == 3 and not self.EveryTimer:IsActive())
		Check("Coroutine waits", table.concat(g.Trace, " ") == "start@0 frames@1 seconds@13 yield@14" and not self.Routine:IsActive())
		Check("Events from a timer", g.Later == 42)
		local spinning = self:StartCoroutine(function() while true do WaitFrames(1) end end)
		local timer = Timer.After(10, function() end)
		self:StopAllCoroutines()
		Check("StopAllCoroutines keeps timers", not spinning:IsActive() and timer:IsActive() and timer:Cancel())
	end

	if frame == 160 then
		Check("Collision end", (FeatureTestEvents.CollisionEnd or 0) >= 1)
		-- It left the ground upward, just above where it rested.
		local parted = FeatureTestEvents.CollisionEndContact
		Check("OnCollisionEnd contact", parted ~= nil and math.abs(parted.Normal.y) > 0.99 and parted.Impulse == 0 and Near(parted.Point.y, 0, 0.3))
		Expect("Game.Quit", function() Game.Quit() end)
		FeatureTest.Quit = true
		FeatureTest.Phase = "done"
	end
end

function Driver:OnPlayClicked(button)
	table.insert(self.WidgetLog, button.Name)
end

function Driver:OnSettingChanged(widget, value)
	local text = type(value) == "number" and string.format("%.1f", value) or tostring(value)
	table.insert(self.WidgetLog, widget.Name .. "=" .. text)
	-- The settings-menu pattern: a slider drives a mixer bus.
	if widget.Name == "VolumeSlider" then
		Audio.SetBusVolume("Music", value)
	end
end

function Driver:OnGamepadConnected(index)
	FeatureTestEvents.GamepadConnected = index
	FeatureTestEvents.GamepadName = Input.GetGamepadName(index)
	FeatureTestEvents.GamepadCount = #Input.GetGamepads()
end

function Driver:OnGamepadDisconnected(index)
	FeatureTestEvents.GamepadDisconnected = index
end

function Driver:OnLateUpdate(dt)
	self.Late = self.Late + 1
end

function Driver:OnDestroy()
	FeatureTestEvents.DriverDestroyed = true
end

-- Invoked after the visual/gameplay checks. Bodies at x=1000 cannot touch the feature scene;
-- zero gravity keeps the parents inside the trigger. They have no rendering components.
function FeatureTest.BeginRuntimeSpawnCheck()
	FeatureTest.RuntimeCreates = 0
	FeatureTest.RuntimeContacts = 0
	local zone = Scene.CreateEntity("RuntimeSpawnZone")
	zone.Translation = Vec3(1000, 0, 0)
	zone:AddComponent("RigidBody", { IsTrigger = true })
	zone:AddComponent("BoxCollider", { HalfExtents = Vec3(10, 1, 10) })
	for i = 1, 3 do
		local body = Scene.CreateEntity("RuntimeSpawn" .. i)
		body.Translation = Vec3(1000 + i * 2, 0, 0)
		body:AddComponent("RigidBody", { Type = "Dynamic", GravityFactor = 0 })
		body:AddComponent("BoxCollider")
		body:AddComponent("SphereCollider")
		body:AddComponent("Script", { Script = "Assets/Scripts/RuntimeSpawnContact.lua" })
	end
end

function FeatureTest.CheckRuntimeSpawns()
	Check("Runtime scripts all create", FeatureTest.RuntimeCreates == 3)
	Check("Spawned compound contacts coalesce", FeatureTest.RuntimeContacts == 3)
	for i = 1, 3 do
		local body = Scene.FindEntityByName("RuntimeSpawn" .. i)
		local child = Scene.FindEntityByName("RuntimeSpawn" .. i .. "Child")
		Check("Contact-driven spawn exists", child ~= nil and body ~= nil)
		if child and body then
			Check("Spawned joint is live", child:HasJoint())
			Check("Spawned fixed joint holds its offset", math.abs((child.Translation - body.Translation).y - 2) < 0.05)
		end
	end
end

return Driver
