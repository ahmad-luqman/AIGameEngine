-- Drives the joint motors and shows what is happening. Everything else is plain Joint components.
local Director = {}

function Director:OnCreate()
	self.Time = 0
	self.Breaks = 0
	self.Door = Scene.FindEntityByName("Door")
	self.Piston = Scene.FindEntityByName("Piston")
	self.Ball = Scene.FindEntityByName("WreckingBall")
	self.BallHook = Scene.FindEntityByName("BallHook")
	self.Chain = { Scene.FindEntityByName("ChainHook") }
	for i = 0, 7 do
		table.insert(self.Chain, Scene.FindEntityByName("Link" .. i))
	end
end

function Director:OnUpdate(dt)
	self.Time = self.Time + dt

	-- Door: a position motor swings it open and shut every two seconds.
	local open = math.floor(self.Time / 2) % 2 == 1
	self.Door:SetComponent("Joint", { MotorTarget = open and 90 or 0 })

	-- Piston: a position motor follows a sine wave. Setting the component every frame is cheap.
	self.Piston:SetComponent("Joint", { MotorTarget = 1.0 + math.sin(self.Time * 2.0) })

	-- Ropes are not meshes: draw the wrecking ball's cable and the chain's links as lines.
	local cable = Vec4(0.15, 0.15, 0.15, 1)
	Debug.DrawLine(self.BallHook.Translation, self.Ball.Translation, cable)
	for i = 2, #self.Chain do
		Debug.DrawLine(self.Chain[i - 1].Translation, self.Chain[i].Translation, cable)
	end

	UI.Text("Basalt joints", 40, 40, 44)
	UI.Text(string.format("Door hinge  %5.1f deg", self.Door:GetJointPosition() or 0), 40, 100, 30)
	UI.Text(string.format("Piston slider  %4.2f m", self.Piston:GetJointPosition() or 0), 40, 140, 30)
	UI.Text(string.format("Welds broken  %d", JointsDemoBreaks or 0), 40, 180, 30)
	UI.Text("Chain: point joints   Wrecking ball: distance joint   Tower: breakable fixed joints", 40, 1030, 24, Vec4(1, 1, 1, 0.7))
end

return Director
