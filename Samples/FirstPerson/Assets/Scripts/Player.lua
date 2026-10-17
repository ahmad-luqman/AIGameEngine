-- The first-person player: mouse (or right stick) look, walking and jumping through the CharacterController, and a
-- hitscan gun that raycasts from the camera. The body turns left and right (yaw); the Camera child tilts up and down
-- (pitch), so the character's capsule stays upright. Shots and the hall's trigger are reported through events, which
-- Game.lua turns into score and acoustics.
local Player = {}

Player.Properties = {
	MoveSpeed = 5.0,
	JumpSpeed = 5.0,
	-- Degrees turned per game-view unit of mouse movement (the view is 1920 units wide headless).
	MouseSensitivity = 0.1,
	-- Degrees per second at full right-stick deflection.
	StickLookSpeed = 140.0,
	MaxPitch = 85.0,
	Range = 60.0,
	StepInterval = 0.42
}

local function RequireChild(parent, name)
	local child = parent:FindChild(name)
	if not child then error("Player: '" .. parent.Name .. "' has no child named '" .. name .. "'", 2) end
	return child
end

function Player:OnCreate()
	self.Camera = RequireChild(self.Entity, "Camera")
	self.Gun = RequireChild(self.Camera, "Gun")
	if not self.Gun:HasComponent("AudioSource") or not self.Entity:HasComponent("AudioSource") then
		error("Player: the Gun and the Player need an AudioSource (shot and footsteps)")
	end
	-- Degrees, read by the gameplay test.
	self.Yaw = 0.0
	self.Pitch = 0.0
	self.Shots = 0
	self.Steps = 0
	self.StepTimer = 0.0
	self:SetLocked(true)
end

-- Locked: the mouse turns the view and the cursor is hidden and held in place. Escape (or Start) releases it in the
-- runtime, as a pause menu would; the editor keeps Escape for itself and releases the cursor on its own. The next
-- click takes it back without firing.
function Player:SetLocked(locked)
	self.Locked = locked
	Input.SetCursorMode(locked and "Locked" or "Normal")
	Events.Emit("CursorLockChanged", locked)
end

function Player:OnUpdate(dt)
	if self.Locked and Input.IsActionPressed("Unlock") then
		self:SetLocked(false)
	elseif not self.Locked and Input.IsActionPressed("Fire") then
		self:SetLocked(true)
		return
	end

	self:Look(dt)
	self:Walk(dt)
	if self.Locked and Input.IsActionPressed("Fire") then
		self:Fire()
	end
end

function Player:Look(dt)
	if self.Locked then
		local delta = Input.GetMouseDelta()
		-- Mouse right turns right (a negative yaw about +Y); mouse down (+y) looks down.
		self.Yaw = self.Yaw - delta.x * self.MouseSensitivity
		self.Pitch = self.Pitch - delta.y * self.MouseSensitivity
	end
	self.Yaw = self.Yaw - Input.GetAction("LookX") * self.StickLookSpeed * dt
	self.Pitch = self.Pitch + Input.GetAction("LookY") * self.StickLookSpeed * dt
	self.Pitch = Math.Clamp(self.Pitch, -self.MaxPitch, self.MaxPitch)
	-- Keep the yaw in [-180, 180) so it never grows without bound (Lua's % rounds down, so this wraps both ways).
	self.Yaw = (self.Yaw + 180) % 360 - 180
	self.Entity.EulerAngles = Vec3(0, Math.Radians(self.Yaw), 0)
	self.Camera.EulerAngles = Vec3(Math.Radians(self.Pitch), 0, 0)
end

function Player:Walk(dt)
	local forward = self.Entity:GetForward()
	local right = self.Entity:GetRight()
	local direction = right * Input.GetAction("MoveX") + forward * Input.GetAction("MoveY")
	direction.y = 0
	-- Diagonals are no faster than straight lines; a half-tilted stick walks slower.
	if direction:Length() > 1 then direction = direction:Normalized() end
	local velocity = direction * self.MoveSpeed
	-- Held, not pressed: Move applies at the next physics step (see Character controllers in the scripting guide).
	if Input.IsActionDown("Jump") then velocity.y = self.JumpSpeed end
	self.Entity:Move(velocity)

	local grounded = self.Entity:IsGrounded()
	if grounded and direction:Length() > 0.1 then
		self.StepTimer = self.StepTimer - dt
		if self.StepTimer <= 0 then
			self.Entity:PlayAudio()
			self.Steps = self.Steps + 1
			self.StepTimer = self.StepInterval
		end
	else
		-- The first step sounds as soon as walking starts.
		self.StepTimer = 0
	end
end

-- A hitscan shot from the middle of the view. The player's own capsule is ignored; triggers are not hit.
function Player:Fire()
	self.Shots = self.Shots + 1
	self.Gun:PlayAudio()
	local origin = self.Camera.WorldPosition
	local hit = Physics.Raycast(origin, self.Camera:GetForward(), self.Range, self.Entity)
	Events.Emit("Shot", hit and hit.Entity, hit and hit.Point)
end

function Player:OnTriggerEnter(other)
	if other.Name == "HallZone" then Events.Emit("AreaChanged", "Hall") end
end

function Player:OnTriggerExit(other)
	if other.Name == "HallZone" then Events.Emit("AreaChanged", "Courtyard") end
end

return Player
