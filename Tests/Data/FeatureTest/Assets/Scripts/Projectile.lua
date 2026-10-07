-- Spawned from the Projectile prefab: flies, reports hits, destroys itself after Lifetime seconds.
local Projectile = {}
Projectile.Properties = { Lifetime = 2.0 }

function Projectile:OnCreate()
	self.Age = 0
	FeatureTestEvents = FeatureTestEvents or {}
	FeatureTestEvents.ProjectilesCreated = (FeatureTestEvents.ProjectilesCreated or 0) + 1
end

function Projectile:OnUpdate(dt)
	self.Age = self.Age + dt
	if self.Age >= self.Lifetime then
		self.Entity:Destroy()
	end
end

function Projectile:OnCollisionBegin(other)
	FeatureTestEvents.ProjectileHits = (FeatureTestEvents.ProjectileHits or 0) + 1
end

function Projectile:OnDestroy()
	FeatureTestEvents.ProjectilesDestroyed = (FeatureTestEvents.ProjectilesDestroyed or 0) + 1
end

return Projectile
