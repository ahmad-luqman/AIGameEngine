-- A glowing orb circling the walker's head, swooping close every few seconds.
local Orb = {}

function Orb:OnCreate()
	self.Walker = Scene.FindEntityByName("Walker")
	self.Time = 0
end

function Orb:OnUpdate(dt)
	self.Time = self.Time + dt
	local t = self.Time
	local radius = 1.35 + 0.65 * math.cos(t * 0.9)
	local center = self.Walker.WorldPosition
	self.Entity.WorldPosition = center + Vec3(math.cos(t * 0.7) * radius, 1.55 + 0.45 * math.sin(t * 1.6), math.sin(t * 0.7) * radius)
end

return Orb
