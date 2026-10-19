-- A moving platform that ferries the hero across a gap: it waits at its start until the hero has stood on it for a
-- moment, glides to the far end (Travel, in meters along each axis), waits there until the hero has left, and returns.
-- It is a Kinematic body moved through its transform, so physics gives it a velocity that carries the hero along.
local Shuttle = {}

Shuttle.Properties = {
	TravelX = 0.0,
	TravelZ = 6.0,
	-- Seconds for a crossing and seconds to wait at either end.
	Duration = 3.0,
	Wait = 0.5
}

function Shuttle:OnCreate()
	self.Hero = Scene.FindEntityByName("Hero")
	self.Start = self.Entity.WorldPosition
	self.Travel = Vec3(self.TravelX, 0, self.TravelZ)
	-- Read by the gameplay test: "Start", "Going", "End", "Returning"; and how many crossings it began.
	self.State = "Start"
	self.Trips = 0
	self.Progress = 0
	self.Timer = 0
end

-- Whether the hero stands on this platform (the ground right under it is this entity).
function Shuttle:Carrying()
	if not self.Hero or not self.Hero:IsGrounded() then return false end
	local hit = Physics.Raycast(self.Hero.WorldPosition + Vec3(0, 0.2, 0), Vec3(0, -1, 0), 0.5, self.Hero)
	return hit ~= nil and hit.Entity == self.Entity
end

function Shuttle:OnUpdate(dt)
	local carrying = self:Carrying()
	if self.State == "Start" or self.State == "End" then
		-- Leave the start once the hero has stood on it for Wait seconds; leave the end Wait seconds after it got off.
		local ready = (self.State == "Start") == carrying
		self.Timer = ready and self.Timer + dt or 0
		if self.Timer >= self.Wait then
			self.Timer = 0
			if self.State == "Start" then
				self.State = "Going"
				self.Trips = self.Trips + 1
			else
				self.State = "Returning"
			end
		end
	else
		local direction = self.State == "Going" and 1 or -1
		self.Progress = Math.Clamp(self.Progress + direction * dt / self.Duration, 0, 1)
		if self.Progress == 1 then self.State = "End" elseif self.Progress == 0 then self.State = "Start" end
	end
	-- Smoothstep: eases in and out at the ends.
	local t = self.Progress * self.Progress * (3 - 2 * self.Progress)
	self.Entity.WorldPosition = self.Start + self.Travel * t
end

return Shuttle
