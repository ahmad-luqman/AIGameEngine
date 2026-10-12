-- Level entity of the scene-switch feature test: announces itself to the persistent manager when its scene starts.
local Level = {}

function Level:OnCreate()
	-- Physics bodies of a loaded scene exist before OnCreate, as at play start.
	SceneSwitchTest.SawFloor[Scene.GetName()] = Physics.Raycast(Vec3(0, 5, 0), Vec3(0, -1, 0), 10) ~= nil
	Events.Emit("SceneSwitch.LevelStarted", Scene.GetName(), self.Entity)
end

function Level:OnDestroy()
	SceneSwitchTest.Unloaded = SceneSwitchTest.Unloaded + 1
end

return Level
