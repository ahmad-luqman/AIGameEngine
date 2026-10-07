-- Rotates its entity; exposes properties of every supported type for the inspector and the tests.
local Spinner = {}
Spinner.Properties = {
	Speed = 1.5,
	Axis = Vec3(0, 1, 0),
	Enabled = true,
	Label = "spinner",
}

function Spinner:OnCreate()
	self.Turns = 0
end

function Spinner:OnUpdate(dt)
	if not self.Enabled then return end
	local step = Quat.AngleAxis(self.Speed * dt, self.Axis)
	self.Entity.Rotation = step * self.Entity.Rotation
	self.Turns = self.Turns + self.Speed * dt / (2 * Math.Pi)
end

function Spinner:OnLateUpdate(dt)
	self.LateUpdates = (self.LateUpdates or 0) + 1
end

return Spinner
