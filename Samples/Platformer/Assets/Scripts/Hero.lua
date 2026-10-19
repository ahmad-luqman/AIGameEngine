-- The player character: camera-relative movement through a CharacterController, with the animation state machine
-- (Assets/Animations/Hero.banimgraph) choosing Locomotion (an Idle/Walk/Run blend space whose root motion moves the
-- character), Jump, Fall and Land from the parameters this script sets every frame.
local Hero = {}

Hero.Properties = {
	-- The root speeds of the Walk and Run clips (m/s); they must match the X values of the Locomotion blend space's
	-- samples, so the Speed parameter picks the blend that moves at that speed.
	WalkSpeed = 1.5,
	RunSpeed = 4.0,
	-- How fast the ground speed follows the input (m/s²), and how quickly the facing turns toward it and the air
	-- velocity follows it (1/s, exponential).
	Acceleration = 8.0,
	TurnSpeed = 10.0,
	AirControl = 3.0,
	-- Take-off speed (m/s): about 1.3 m high under the default gravity.
	JumpSpeed = 5.0
}

local Up = Vec3(0, 1, 0)

local function Approach(value, target, step)
	if value < target then return math.min(value + step, target) end
	return math.max(value - step, target)
end

-- The fraction of the way to a target that exponential smoothing at `rate` (1/s) covers in `dt`.
local function Ease(rate, dt)
	return 1 - math.exp(-rate * dt)
end

function Hero:OnCreate()
	self.Camera = Scene.FindEntityByName("Camera")
	if not self.Camera then error("Hero: the scene has no entity named 'Camera'") end
	-- Coyote time: the controller still jumps this long after walking off a ledge, and the animation waits as long
	-- before calling it a fall (so a step down a stair is not one).
	self.CoyoteTime = self.Entity:GetComponent("CharacterController").JumpGrace
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
	-- Seconds since the controller last stood on ground, and the time left to keep asking for the jump until the
	-- controller takes off (Move applies at the next physics step, which a short frame may not run).
	self.Ungrounded = 0
	self.JumpHold = 0
	self.Entity:Move(Vec3(0, 0, 0))
	-- A respawn farther than the teleport check's reach (about 1.7 m in a 60 Hz frame) restarts the hair and cape by
	-- itself; a nearer one would swing them across, so restart them here.
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

-- Leaves the ground (a jump, or walking off a ledge) carrying the ground speed.
function Hero:TakeOff(facing)
	self.Airborne = true
	self.AirVelocity = facing * self.Speed
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
		self.Yaw = self.Yaw + delta * Ease(self.TurnSpeed, dt)
		entity.Rotation = Quat.AngleAxis(self.Yaw, Up)
	end
	local facing = Vec3(math.sin(self.Yaw), 0, math.cos(self.Yaw))

	if self.Airborne then
		self.AirVelocity = self.AirVelocity + (wish * topSpeed - self.AirVelocity) * Ease(self.AirControl, dt)
		if grounded and self.JumpHold <= 0 then
			-- Landing keeps the air speed but runs along the facing, which turned toward the input in the air.
			self.Airborne = false
			self.Speed = math.min(self.AirVelocity:Length(), self.RunSpeed)
		end
	else
		self.Speed = Approach(self.Speed, amount * topSpeed, self.Acceleration * dt)
		if self.Ungrounded > self.CoyoteTime then
			-- Walked off a ledge: keep the momentum.
			self:TakeOff(facing)
		end
	end

	-- Pressed, not held: holding the button never jumps again on landing. Within the coyote time after walking off a
	-- ledge (not yet Airborne) it still jumps: the controller's JumpGrace takes the upward request once.
	if Input.IsActionPressed("Jump") and not self.Airborne then
		self:TakeOff(facing)
		self.JumpHold = 0.1
		self.Jumps = self.Jumps + 1
		entity:SetAnimationTrigger("Jump")
	end
	local vertical = 0
	if self.JumpHold > 0 then
		vertical = self.JumpSpeed
		-- Rising: the controller took the jump.
		self.JumpHold = entity:GetLinearVelocity().y > 1 and 0 or self.JumpHold - dt
	end

	-- On the ground the Locomotion state's root motion replaces the horizontal part (through a fade into or out of it,
	-- it shares it with this velocity); the vertical part stays, so the jump still takes off. Land and the air states
	-- move by this velocity.
	local horizontal = self.Airborne and self.AirVelocity or facing * self.Speed
	entity:Move(horizontal + Up * vertical)
	entity:SetAnimationParameter("Speed", self.Speed)
	entity:SetAnimationParameter("Grounded", not self.Airborne)
	entity:SetAnimationParameter("VerticalSpeed", entity:GetLinearVelocity().y)
end

return Hero
