-- Drives the joint motors and shows what is happening. Everything else is plain Joint components.
local Director = {}

function Director:OnCreate()
	self.Time = 0
	self.Door = Scene.FindEntityByName("Door")
	self.Piston = Scene.FindEntityByName("Piston")
	self.Ball = Scene.FindEntityByName("WreckingBall")
	self.BallHook = Scene.FindEntityByName("BallHook")
	self.Chain = { Scene.FindEntityByName("ChainHook") }
	for i = 0, 7 do
		table.insert(self.Chain, Scene.FindEntityByName("Link" .. i))
	end
	self.Ladder = {}
	for i = 0, 2 do
		table.insert(self.Ladder, Scene.FindEntityByName("LadderRung" .. i))
	end
	self.Forearm = Scene.FindEntityByName("Forearm")
	self.Hand = Scene.FindEntityByName("Hand")
	self.Bungee = Scene.FindEntityByName("BungeeBall")
	self.BungeeHook = Scene.FindEntityByName("BungeeHook")
	self.Pushes = 0
end

-- The ends of a ladder rung, where its two rope joints attach (1.08 m either side of its centre).
local function RungEnds(rung)
	local half = rung:GetRight() * 1.08
	return rung.Translation - half, rung.Translation + half
end

function Director:OnUpdate(dt)
	self.Time = self.Time + dt

	-- Door: a position motor swings it open and shut every two seconds.
	local open = math.floor(self.Time / 2) % 2 == 1
	self.Door:SetComponent("Joint", { MotorTarget = open and 90 or 0 })

	-- Piston: a position motor follows a sine wave. Setting the component every frame is cheap.
	self.Piston:SetComponent("Joint", { MotorTarget = 1.0 + math.sin(self.Time * 2.0) })

	-- Knock the ladder and the bungee once, and swing the arm from the hand every 2.5 seconds.
	if not self.Kicked and self.Time > 0.3 then
		self.Ladder[3]:SetLinearVelocity(Vec3(0, 0, 3))
		self.Bungee:SetLinearVelocity(Vec3(0, -6, 0))
		self.Kicked = true
	end
	local pushes = math.floor(self.Time / 2.5)
	if pushes > self.Pushes then
		self.Pushes = pushes
		local side = pushes % 2 == 1 and 1 or -1
		self.Hand:SetLinearVelocity(Vec3(5 * side, 2, 3))
	end

	-- Ropes are not meshes: draw the wrecking ball's cable, the chain's links, the ladder's ropes and the
	-- bungee cord as lines.
	local cable = Vec4(0.15, 0.15, 0.15, 1)
	Debug.DrawLine(self.BallHook.Translation, self.Ball.Translation, cable)
	for i = 2, #self.Chain do
		Debug.DrawLine(self.Chain[i - 1].Translation, self.Chain[i].Translation, cable)
	end
	local aboveLeft, aboveRight = Vec3(-7.08, 6, -4), Vec3(-4.92, 6, -4)
	for _, rung in ipairs(self.Ladder) do
		local left, right = RungEnds(rung)
		Debug.DrawLine(aboveLeft, left, cable)
		Debug.DrawLine(aboveRight, right, cable)
		aboveLeft, aboveRight = left, right
	end
	Debug.DrawLine(self.BungeeHook.Translation, self.Bungee.Translation, Vec4(0.2, 0.75, 0.35, 1))

	UI.Text("Basalt joints", 40, 40, 44)
	UI.Text(string.format("Door hinge  %5.1f deg", self.Door:GetJointPosition() or 0), 40, 100, 30)
	UI.Text(string.format("Piston slider  %4.2f m", self.Piston:GetJointPosition() or 0), 40, 140, 30)
	UI.Text(string.format("Welds broken  %d", JointsDemoBreaks or 0), 40, 180, 30)
	UI.Text(string.format("Elbow hinge  %5.1f deg", self.Forearm:GetJointPosition() or 0), 40, 220, 30)
	UI.Text("Rope ladder: two joint entities per rung   Arm: six-DOF shoulder, soft-stop elbow, cone wrist   Bungee: distance spring", 40, 990, 24, Vec4(1, 1, 1, 0.7))
	UI.Text("Chain: point joints   Wrecking ball: distance joint   Tower: breakable fixed joints", 40, 1030, 24, Vec4(1, 1, 1, 0.7))
end

return Director
