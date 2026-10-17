-- Runs the First Person sample: scores the targets the player shoots (each drops and stands back up after a while),
-- muffles the generator behind its wall (a raycast from the listener: AudioSource.LowPass), switches the reverb
-- between the open courtyard and the stone hall (Audio.SetReverb and AudioSource.ReverbSend), and keeps the HUD.
local Game = {}

Game.Properties = {
	-- Seconds a target stays down.
	TargetRespawn = 4.0,
	-- The generator's cutoff while something is between it and the listener.
	OccludedLowPass = 500.0
}

-- The acoustics of each area: the room every bus's reverb simulates and how much the player's own sounds (shots
-- and footsteps) feed it. The generator keeps a small fixed send (0.08) into whichever room is active; the drone
-- sends none.
local Areas = {
	Courtyard = { Reverb = { RoomSize = 0.3, Damping = 0.6, Width = 1.0 }, Send = 0.08 },
	Hall = { Reverb = { RoomSize = 0.92, Damping = 0.15, Width = 1.0 }, Send = 0.55 }
}

local function RequireEntity(name, component)
	local entity = Scene.FindEntityByName(name)
	if not entity then error("Game: the scene has no entity named '" .. name .. "'", 2) end
	if component and not entity:HasComponent(component) then error("Game: '" .. name .. "' has no " .. component, 2) end
	return entity
end

function Game:OnCreate()
	self.Player = RequireEntity("Player", "CharacterController")
	self.Camera = RequireEntity("Camera", "AudioListener")
	self.Gun = RequireEntity("Gun", "AudioSource")
	self.Generator = RequireEntity("Generator", "AudioSource")
	-- Player.lua reports entering and leaving this trigger as area changes.
	RequireEntity("HallZone", "RigidBody")
	self.ScoreText = RequireEntity("ScoreText", "UIText")
	self.AreaText = RequireEntity("AreaText", "UIText")
	self.HintText = RequireEntity("HintText", "UIText")
	self.Targets = {}
	-- Target1, Target2, ...: the entity sits at the target's base (its Face child is the board), so tipping it
	-- pivots about the ground.
	self.TargetCount = 0
	while true do
		local target = Scene.FindEntityByName("Target" .. (self.TargetCount + 1))
		if not target then break end
		self.TargetCount = self.TargetCount + 1
		self.Targets[target.ID] = { Entity = target, Down = false, Standing = target.Rotation }
	end
	if self.TargetCount == 0 then error("Game: the scene has no Target1") end
	-- Read by the gameplay test.
	self.Score = 0
	self.Occluded = nil
	self.Area = nil
	Events.On("Shot", function(entity, point) self:OnShot(entity, point) end)
	Events.On("AreaChanged", function(area) self:SetArea(area) end)
	Events.On("CursorLockChanged", function(locked) self:OnCursorLockChanged(locked) end)
	self:SetArea("Courtyard")
	self:OnCursorLockChanged(Input.GetCursorMode() == "Locked")
	self:UpdateOcclusion()
end

function Game:OnShot(entity, point)
	local target = entity and self.Targets[entity.ID]
	if not target or target.Down then return end
	target.Down = true
	-- Tipped over backwards about its base, below the line of fire; a shot aimed down at it scores nothing until it
	-- stands again.
	target.Entity.Rotation = Quat.AngleAxis(Math.Radians(-80), Vec3(1, 0, 0)) * target.Standing
	self.Score = self.Score + 1
	self.ScoreText:SetComponent("UIText", { Text = "TARGETS " .. self.Score })
	Audio.PlayOneShot("Assets/Audio/Ding.wav", point, 0.8)
	Log.Info("Target hit: " .. self.Score)
	Timer.After(self.TargetRespawn, function()
		if target.Entity:IsValid() then
			target.Entity.Rotation = target.Standing
			target.Down = false
		end
	end)
end

function Game:SetArea(area)
	local settings = Areas[area]
	if not settings then error("Game: unknown area '" .. tostring(area) .. "'") end
	if area == self.Area then return end
	self.Area = area
	Audio.SetReverb(settings.Reverb)
	self.Gun:SetComponent("AudioSource", { ReverbSend = settings.Send })
	self.Player:SetComponent("AudioSource", { ReverbSend = settings.Send })
	self.AreaText:SetComponent("UIText", { Text = string.upper(area) })
	Log.Info("Entered the " .. area)
end

function Game:OnCursorLockChanged(locked)
	self.HintText:SetComponent("UIText", { Text = locked and "WASD move   Space jump   Click shoot   Esc release the mouse" or "Click to look around" })
end

-- Muffles the generator while anything solid is between it and the listener. Runs after physics, so it sees where
-- the player ended up this frame.
function Game:UpdateOcclusion()
	local from = self.Camera.WorldPosition
	local to = self.Generator.WorldPosition
	local hit = Physics.Raycast(from, to - from, from:Distance(to), self.Player)
	local occluded = hit ~= nil and hit.Entity ~= self.Generator
	if occluded ~= self.Occluded then
		self.Occluded = occluded
		self.Generator:SetComponent("AudioSource", { LowPass = occluded and self.OccludedLowPass or 0 })
	end
end

function Game:OnLateUpdate(dt)
	self:UpdateOcclusion()
end

return Game
