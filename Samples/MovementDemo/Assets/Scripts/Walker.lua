-- Walks the mannequin back and forth over the stairs, ramp and stones (feet placed by IK), looks at the orb with its
-- head and spine, waves now and then (a masked layer fading in and out) and reaches for the orb when it comes close.
local Walker = {}

Walker.Properties = { Speed = 1.45, Extent = 7.5, WavePeriod = 7.0 }

function Walker:OnCreate()
	self.Direction = 1
	self.Yaw = 90
	self.Time = 0
	self.Orb = Scene.FindEntityByName("Orb")
	self.Entity:SetIKTarget("Head", self.Orb)
	self.Entity:SetIKTarget("Reach", self.Orb)
end

function Walker:OnUpdate(dt)
	self.Time = self.Time + dt
	local position = self.Entity.WorldPosition
	if self.Direction > 0 and position.x > self.Extent then self.Direction = -1 end
	if self.Direction < 0 and position.x < -self.Extent then self.Direction = 1 end
	-- Turn toward the walking direction (through facing the camera).
	local target = self.Direction > 0 and 90 or -90
	self.Yaw = self.Yaw + (target - self.Yaw) * math.min(1, dt * 3)
	local yaw = math.rad(self.Yaw)
	self.Entity.Rotation = Quat.AngleAxis(yaw, Vec3(0, 1, 0))
	local speed = math.abs(self.Yaw) > 60 and self.Speed or self.Speed * 0.4
	self.Entity:Move(Vec3(math.sin(yaw), 0, math.cos(yaw)) * speed)

	local phase = self.Time % self.WavePeriod
	local waving = phase > 3 and phase < 5.5
	if waving ~= self.Waving then
		self.Waving = waving
		self.Entity:SetAnimationLayerWeight("Wave", waving and 1 or 0, 0.35)
	end
	local shoulder = position + Vec3(0, 1.35, 0)
	local reaching = (self.Orb.WorldPosition - shoulder):Length() < 0.95 and not waving
	if reaching ~= self.Reaching then
		self.Reaching = reaching
		self.Entity:SetIKWeight("Reach", reaching and 1 or 0, 0.3)
	end
end

return Walker
