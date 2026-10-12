-- Scene-switch feature test: a persistent manager (Scene.DontDestroyOnLoad) that switches SceneSwitchA ->
-- SceneSwitchB -> SceneSwitchA from a coroutine and checks what survives, plus the Storage API.
--
-- FeatureTest.batch.json plays SceneSwitchA, calls SceneSwitchTest.Report(), then plays it again and calls
-- SceneSwitchTest.CheckSaved(), which needs the save data written at the end of the first session. ctest points
-- BASALT_USER_DATA_DIR at the build directory, so this never touches the real user data directory.

local Checks = require("Assets.Scripts.Lib.Checks")
-- Bound to the session's SceneSwitchTest table in OnCreate.
local Check, ExpectError

local function CountNamed(name)
	return #Scene.FindEntitiesByName(name)
end

local function Report()
	if #SceneSwitchTest.Failures > 0 then
		error(string.format("%d of %d checks failed: %s", #SceneSwitchTest.Failures, SceneSwitchTest.Checks, table.concat(SceneSwitchTest.Failures, "; ")))
	end
	if SceneSwitchTest.Phase ~= "done" then
		error("scene switch test did not finish (phase: " .. SceneSwitchTest.Phase .. ")")
	end
	return string.format("PASS: %d checks", SceneSwitchTest.Checks)
end

-- Second session: the first one saved this right before it finished.
local function CheckSaved()
	local saved = SceneSwitchTest.Previous
	if type(saved) ~= "table" or saved.Score ~= 42 or saved.LevelsStarted ~= 3 or saved.Last ~= "SceneSwitchA" then
		error("Storage did not keep the first session's result: " .. tostring(saved))
	end
	return "PASS: saved data survived the session"
end

local function TestStorage()
	Check("Storage.Get default", Storage.Get("SceneSwitch.Missing", 7) == 7 and Storage.Get("SceneSwitch.Missing") == nil)
	Storage.Set("SceneSwitch.Number", 3.5)
	Storage.Set("SceneSwitch.Text", "hello")
	Storage.Set("SceneSwitch.Table", { Level = 2, Flags = { true, false } })
	Storage.Set("SceneSwitch.Vector", Vec3(1, 2, 3))
	Check("Storage.Set/Get", Storage.Get("SceneSwitch.Number") == 3.5 and Storage.Get("SceneSwitch.Text") == "hello" and Storage.Get("SceneSwitch.Table").Flags[2] == false)
	local vector = Storage.Get("SceneSwitch.Vector")
	Check("Storage keeps vectors as arrays", type(vector) == "table" and vector[1] == 1 and vector[3] == 3)
	Check("Storage.Has", Storage.Has("SceneSwitch.Text") and not Storage.Has("SceneSwitch.Missing"))
	Check("Storage.Delete", Storage.Delete("SceneSwitch.Text") and not Storage.Delete("SceneSwitch.Text") and not Storage.Has("SceneSwitch.Text"))
	Storage.Set("SceneSwitch.Number", nil)
	Check("Storage.Set nil deletes", not Storage.Has("SceneSwitch.Number"))
	Check("Storage.GetKeys", table.concat(Storage.GetKeys(), ",") == "SceneSwitch.Table,SceneSwitch.Vector")
	local saved, saveError = Storage.Save()
	Check("Storage.Save", saved == true and saveError == nil)
	Storage.Set("SceneSwitch.Unsaved", true)
	local loaded = Storage.Load()
	Check("Storage.Load drops unsaved changes", loaded == true and not Storage.Has("SceneSwitch.Unsaved") and Storage.Has("SceneSwitch.Table"))
	Storage.Clear()
	Check("Storage.Clear", #Storage.GetKeys() == 0)
	ExpectError("Storage.Set needs a key", function() Storage.Set("", 1) end, "non-empty string")
	ExpectError("Storage.Set rejects functions", function() Storage.Set("SceneSwitch.Fn", function() end) end, "Storage.Set('SceneSwitch.Fn')")
	ExpectError("Storage.Set rejects NaN", function() Storage.Set("SceneSwitch.N", 0 / 0) end, "numbers must be finite")
	ExpectError("Storage.Set rejects invalid UTF-8", function() Storage.Set("SceneSwitch.S", "a\255") end, "valid UTF-8")
	Storage.Set("SceneSwitch.Text", "Caf\195\169")
	Check("Storage keeps UTF-8 text", Storage.Get("SceneSwitch.Text") == "Caf\195\169")
	Storage.Delete("SceneSwitch.Text")
	Check("Storage.Save after Clear", Storage.Save() == true)
end

local Manager = {}

function Manager:OnCreate()
	SceneSwitchTest = { Checks = 0, Failures = {}, Phase = "A", Levels = {}, SawFloor = {}, Unloaded = 0, Report = Report, CheckSaved = CheckSaved }
	local checks = Checks.New(SceneSwitchTest, "SceneSwitchTest")
	Check, ExpectError = checks.Check, checks.ExpectError
	-- What the previous session saved, read before this one clears the data.
	SceneSwitchTest.Previous = Storage.Get("SceneSwitch.Result")
	Storage.Clear()
	TestStorage()

	Scene.DontDestroyOnLoad(self.Entity)
	ExpectError("DontDestroyOnLoad needs a root", function() Scene.DontDestroyOnLoad(Scene.FindEntityByName("SwitchManagerChild")) end, "not a root entity")
	Check("Scene.GetName/GetPath", Scene.GetName() == "SceneSwitchA" and Scene.GetPath() == "Assets/Scenes/SceneSwitchA.bscene")

	self.Ticks = 0
	self.Ticker = Timer.Every(0.05, function() self.Ticks = self.Ticks + 1 end)
	Events.On("SceneSwitch.LevelStarted", function(name, level)
		table.insert(SceneSwitchTest.Levels, name)
		SceneSwitchTest.LastLevel = level
	end)

	self:StartCoroutine(function()
		WaitFrames(2)
		Check("Level A started", table.concat(SceneSwitchTest.Levels, ",") == "SceneSwitchA")
		Scene.Load("Assets/Scenes/SceneSwitchB.bscene")
		Check("Scene.Load waits for the end of the frame", Scene.FindEntityByName("LevelA") ~= nil)
		local ticks = self.Ticks
		SceneSwitchTest.Phase = "B"
		WaitFrames(1)

		Check("Scene.Load replaced the scene", Scene.GetName() == "SceneSwitchB" and Scene.GetPath() == "Assets/Scenes/SceneSwitchB.bscene")
		Check("Old entities are gone", Scene.FindEntityByName("LevelA") == nil and Scene.FindEntityByName("CameraA") == nil and SceneSwitchTest.Unloaded == 1)
		Check("New entities exist", Scene.FindEntityByName("LevelB") ~= nil and Scene.GetPrimaryCamera() == Scene.FindEntityByName("CameraB"))
		Check("The persistent manager is kept, its copy in the file skipped", CountNamed("SwitchManager") == 1 and Scene.FindEntityByName("SwitchManagerChild") ~= nil and Scene.FindEntityByName("OtherManagerChild") == nil)
		Check("Persistent state survives", self.Ticks > ticks and self.Ticker:IsActive())
		Check("Events reach the persistent manager", table.concat(SceneSwitchTest.Levels, ",") == "SceneSwitchA,SceneSwitchB" and SceneSwitchTest.LastLevel == Scene.FindEntityByName("LevelB"))
		Check("Loaded bodies exist in OnCreate", SceneSwitchTest.SawFloor.SceneSwitchB == true and SceneSwitchTest.SawFloor.SceneSwitchA == false)
		Check("Loaded scene settings apply", Physics.GetGravity() == Vec3(0, -3, 0))
		Check("Loaded PlayOnStart audio plays", Scene.FindEntityByName("LevelB"):IsAudioPlaying())

		Scene.Load("Assets/Scenes/SceneSwitchA.bscene")
		SceneSwitchTest.Phase = "A again"
		WaitFrames(1)

		Check("Scene.Load back to A", Scene.GetName() == "SceneSwitchA" and Scene.FindEntityByName("LevelA") ~= nil and Scene.FindEntityByName("LevelB") == nil)
		Check("Still one manager", CountNamed("SwitchManager") == 1 and SceneSwitchTest.Unloaded == 2)
		Check("Gravity back to A's", Physics.GetGravity() == Vec3(0, -9.81, 0))
		Storage.Set("SceneSwitch.Result", { Score = 42, LevelsStarted = #SceneSwitchTest.Levels, Last = Scene.GetName() })
		Check("Storage.Save at the end", Storage.Save() == true)
		SceneSwitchTest.Phase = "done"
	end)
end

return Manager
