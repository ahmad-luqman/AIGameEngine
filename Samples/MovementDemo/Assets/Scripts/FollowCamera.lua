-- Follows the walker from the front-side and keeps its chest in view.
local FollowCamera = {}

function FollowCamera:OnCreate()
	self.Walker = Scene.FindEntityByName("Walker")
end

function FollowCamera:OnLateUpdate(dt)
	local walker = self.Walker.WorldPosition
	local wanted = Vec3(walker.x + 1.2, 1.75, 4.6)
	local position = self.Entity.WorldPosition
	self.Entity.WorldPosition = position + (wanted - position) * math.min(1, dt * 2.5)
	self.Entity:LookAt(walker + Vec3(0, 0.95, 0))
end

return FollowCamera
