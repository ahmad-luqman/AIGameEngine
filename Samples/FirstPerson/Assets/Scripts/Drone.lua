-- A drone that flies back and forth along X at a steady speed, its rotor sound (AudioSource.Doppler = 1) rising as
-- it comes toward the player and falling as it goes away. It is moved by script, so the audio system takes its
-- velocity from its change in position each frame.
local Drone = {}

Drone.Properties = {
	Speed = 12.0,
	-- It turns around at -Extent and +Extent.
	Extent = 16.0
}

function Drone:OnCreate()
	-- +1 flying toward +X, -1 toward -X.
	self.Direction = 1
	-- How far it moved over the last frame, per second: what the audio system's Doppler sees (read by the gameplay
	-- test).
	self.Velocity = Vec3(0, 0, 0)
end

function Drone:OnUpdate(dt)
	local previous = self.Entity.Translation
	local position = self.Entity.Translation
	position.x = position.x + self.Direction * self.Speed * dt
	if position.x > self.Extent then
		position.x = self.Extent
		self.Direction = -1
	elseif position.x < -self.Extent then
		position.x = -self.Extent
		self.Direction = 1
	end
	self.Entity.Translation = position
	if dt > 0 then self.Velocity = (position - previous) / dt end
end

return Drone
