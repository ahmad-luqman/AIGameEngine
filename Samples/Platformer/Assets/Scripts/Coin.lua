-- A coin that spins in place until the hero touches it, then reports itself (CoinCollected) and disappears.
local Coin = {}

Coin.Properties = { SpinSpeed = 3.0 }

function Coin:OnCreate()
	self.Angle = 0
end

function Coin:OnUpdate(dt)
	self.Angle = (self.Angle + self.SpinSpeed * dt) % (2 * math.pi)
	self.Entity.Rotation = Quat.AngleAxis(self.Angle, Vec3(0, 1, 0))
end

function Coin:OnTriggerEnter(other)
	-- A destroyed coin lingers until the frame ends: it counts once.
	if other.Name ~= "Hero" or self.Taken then return end
	self.Taken = true
	if Events.Emit("CoinCollected", self.Entity) == 0 then
		Log.Warn("Platformer: nothing handles CoinCollected from '" .. self.Entity.Name .. "'")
	end
	self.Entity:Destroy()
end

return Coin
