-- The player character: camera-relative movement through a CharacterController, with the animation state machine
-- (Assets/Animations/Hero.banimgraph) choosing Locomotion (an Idle/Walk/Run blend space whose root motion moves the
-- character), Jump, Fall and Land from the parameters this script sets every frame.
local Hero = {}

Hero.Properties = {
	-- The root speeds of the Walk and Run clips (m/s): the Speed parameter picks the blend between them.
	WalkSpeed = 1.5,
	RunSpeed = 4.0,
	-- How fast the ground speed follows the input (m/s²) and the facing turns toward it (1/s).
	Acceleration = 8.0,
	TurnSpeed = 10.0,
	-- Take-off speed (m/s): about 1.3 m high under the default gravity.
	JumpSpeed = 5.0,
	-- How fast the air velocity follows the input (1/s).
	AirControl = 3.0,
	-- Seconds after walking off a ledge that still count as standing: a late jump works and a step down a stair is
	-- not a fall.
	CoyoteTime = 0.12
}

local Up = Vec3(0, 1, 0)

function Hero:OnCreate()
	self.Camera = Scene.FindEntityByName("Camera")
	if not self.Camera then error("Hero: the scene has no entity named 'Camera'") end
	self:Respawn(self.Entity.WorldPosition)
	-- Read by the gameplay test.
	self.Jumps = 0
end

-- Puts the hero at `position`, standing, facing +Z.
function Hero:Respawn(position)
	self.Entity.WorldPosition = position
	self.Yaw = 0
	self.Entity.Rotation = Quat.Identity()
	-- Ground speed along the facing, and the horizontal velocity while airborne.
	self.Speed = 0
	self.AirVelocity = Vec3(0, 0, 0)
	self.Airborne = false
	-- Seconds since the controller last stood on ground, and the time left to push a jump off the ground (Move
	-- applies at the next physics step, which a short frame may not run).
	self.Ungrounded = 0
	self.JumpHold = 0
	self.Entity:Move(Vec3(0, 0, 0))
	-- The teleport check would restart the hair and cape too; a short respawn would not trip it.
	self.Entity:ResetSpringBones()
end

-- The movement input in world space (length up to 1), relative to where the camera looks.
function Hero:Wish()
	local forward = self.Camera:GetForward()
	forward.y = 0
	if forward:Length() < 1e-3 then forward = Vec3(0, 0, 1) end
	forward = forward:Normalized()
	local right = Vec3(-forward.z, 0, forward.x)
	local wish = right * Input.GetAction("MoveX") + forward * Input.GetAction("MoveY")
	if wish:Length() > 1 then wish = wish:Normalized() end
	return wish
end

local function Approach(value, target, step)
	if value < target then return math.min(value + step, target) end
	return math.max(value - step, target)
end

function Hero:OnUpdate(dt)
	local entity = self.Entity
	local wish = self:Wish()
	local amount = wish:Length()
	local topSpeed = Input.IsActionDown("Run") and self.RunSpeed or self.WalkSpeed
	local grounded = entity:IsGrounded()
	self.Ungrounded = grounded and 0 or self.Ungrounded + dt

	if amount > 0.1 then
		-- Turn toward the input the short way round.
		local yaw = math.atan(wish.x, wish.z)
		local delta = (yaw - self.Yaw + math.pi) % (2 * math.pi) - math.pi
		self.Yaw = self.Yaw + delta * math.min(1, dt * self.TurnSpeed)
		entity.Rotation = Quat.AngleAxis(self.Yaw, Up)
	end
	local facing = Vec3(math.sin(self.Yaw), 0, math.cos(self.Yaw))

	if self.Airborne then
		self.AirVelocity = self.AirVelocity + (wish * topSpeed - self.AirVelocity) * math.min(1, dt * self.AirControl)
		if grounded and self.JumpHold <= 0 then
			self.Airborne = false
			self.Speed = math.min(self.AirVelocity:Length(), self.RunSpeed)
		end
	else
		self.Speed = Approach(self.Speed, amount * topSpeed, self.Acceleration * dt)
		if self.Ungrounded > self.CoyoteTime then
			-- Walked off a ledge: keep the momentum.
			self.Airborne = true
			self.AirVelocity = facing * self.Speed
		end
	end

	-- Pressed, not held: holding the button never jumps again on landing.
	if Input.IsActionPressed("Jump") and not self.Airborne then
		self.Airborne = true
		self.AirVelocity = facing * self.Speed
		self.JumpHold = 0.1
		self.Jumps = self.Jumps + 1
		entity:SetAnimationTrigger("Jump")
	end
	local vertical = 0
	if self.JumpHold > 0 then
		vertical = self.JumpSpeed
		self.JumpHold = grounded and self.JumpHold - dt or 0
	end

	-- On the ground the Locomotion state's root motion replaces the horizontal part (the vertical part stays, so the
	-- jump still takes off); Land and the air states move by this velocity.
	local horizontal = self.Airborne and self.AirVelocity or facing * self.Speed
	entity:Move(horizontal + Up * vertical)
	entity:SetAnimationParameter("Speed", self.Speed)
	entity:SetAnimationParameter("Grounded", not self.Airborne)
	entity:SetAnimationParameter("VerticalSpeed", entity:GetLinearVelocity().y)
end

return Hero
