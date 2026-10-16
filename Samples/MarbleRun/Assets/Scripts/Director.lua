-- Runs the marble run: releases marbles, cycles the gate, follows the sensors' events and keeps the HUD current.
-- The HUD widgets call OnDrop, OnSpeedChanged and OnHoldGate (their Target is this entity).
local Director = {}

Director.Properties = {
	MarbleCount = 5,
	FirstDrop = 0.5,
	DropInterval = 2.5,
	GateOpenTime = 1.0,
	GateClosedTime = 1.5,
	GateLift = 1.0
}

local MarblePrefab = "Assets/Prefabs/Marble.bprefab"
-- Each marble gets the next color, so they can be told apart on the course.
local Colors = {
	{ 0.85, 0.12, 0.10, 1 },
	{ 0.10, 0.35, 0.90, 1 },
	{ 0.95, 0.70, 0.08, 1 },
	{ 0.12, 0.70, 0.30, 1 },
	{ 0.65, 0.20, 0.80, 1 }
}

function Director:OnCreate()
	self.Dropped = 0
	self.Scored = 0
	self.Lost = 0
	self.Checkpoints = 0
	self.DropTimer = self.FirstDrop
	self.GateTimer = 0
	self.GateOpen = false
	self.HoldGate = false
	self.Spawn = Scene.FindEntityByName("Spawn")
	self.Gate = Scene.FindEntityByName("Gate")
	self.Spinner = Scene.FindEntityByName("Spinner")
	self.Scoreboard = Scene.FindEntityByName("Scoreboard")
	self.Progress = Scene.FindEntityByName("Progress")
	-- The sensors report through events, so they need not know who keeps score.
	Events.On("MarbleScored", function()
		self.Scored = self.Scored + 1
		Log.Info("MarbleRun: scored " .. self.Scored)
		self:UpdateHud()
	end)
	Events.On("MarbleLost", function()
		self.Lost = self.Lost + 1
		Log.Warn("MarbleRun: lost a marble")
		self:UpdateHud()
	end)
	Events.On("CheckpointPassed", function() self.Checkpoints = self.Checkpoints + 1 end)
	self:UpdateHud()
end

-- Releases the next marble, if any are left. Returns whether one was released.
function Director:Drop()
	if self.Dropped >= self.MarbleCount then return false end
	self.Dropped = self.Dropped + 1
	local marble = Scene.Instantiate(MarblePrefab, self.Spawn.WorldPosition)
	marble:SetComponent("Material", { AlbedoColor = Colors[(self.Dropped - 1) % #Colors + 1] })
	self.DropTimer = self.DropInterval
	self:UpdateHud()
	return true
end

function Director:IsFinished()
	return self.Scored + self.Lost == self.MarbleCount
end

function Director:OnUpdate(dt)
	self.DropTimer = self.DropTimer - dt
	if self.DropTimer <= 0 then self:Drop() end

	-- The gate opens and closes on a cycle, unless the HUD checkbox holds it shut.
	self.GateTimer = self.GateTimer - dt
	if self.GateTimer <= 0 then
		self.GateOpen = not self.GateOpen
		self.GateTimer = self.GateOpen and self.GateOpenTime or self.GateClosedTime
	end
	local open = self.GateOpen and not self.HoldGate
	if open ~= self.GateShownOpen then
		self.GateShownOpen = open
		self.Gate:SetComponent("Joint", { MotorTarget = open and self.GateLift or 0 })
	end
end

function Director:UpdateHud()
	local text = string.format("Scored %d / %d   Lost %d", self.Scored, self.MarbleCount, self.Lost)
	if self:IsFinished() then text = text .. "   Done!" end
	self.Scoreboard:SetComponent("UIText", { Text = text })
	self.Progress:SetComponent("UISlider", { Value = self.Scored / self.MarbleCount })
end

-- HUD callbacks.
function Director:OnDrop(button)
	self:Drop()
end

function Director:OnSpeedChanged(slider, value)
	self.Spinner:SetComponent("Joint", { MotorTarget = value })
end

function Director:OnHoldGate(checkbox, checked)
	self.HoldGate = checked
end

return Director
