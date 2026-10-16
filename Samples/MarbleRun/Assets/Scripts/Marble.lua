-- A marble clacks when it hits something hard enough: its own spatial AudioSource, pitched by how hard, so the
-- sound follows the marble down the course.
local Marble = {}

Marble.Properties = {
	MinImpulse = 0.4,
	MinInterval = 0.08
}

function Marble:OnCreate()
	self.SinceClack = self.MinInterval
	self.Clacks = 0
end

function Marble:OnUpdate(dt)
	self.SinceClack = self.SinceClack + dt
end

function Marble:OnCollisionBegin(other, contact)
	if contact.Impulse < self.MinImpulse or self.SinceClack < self.MinInterval then return end
	self.SinceClack = 0
	self.Clacks = self.Clacks + 1
	-- Harder hits are louder and lower.
	local strength = math.min(contact.Impulse / 4, 1)
	self.Entity:SetComponent("AudioSource", { Volume = 0.3 + 0.7 * strength, Pitch = 1.3 - 0.5 * strength })
	self.Entity:PlayAudio()
end

return Marble
