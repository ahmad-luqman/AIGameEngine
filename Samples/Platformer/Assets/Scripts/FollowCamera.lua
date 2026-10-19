-- Follows the hero from behind and above, always looking along +Z, so "forward" on the stick is down the course.
local FollowCamera = {}

FollowCamera.Properties = {
	-- Where the camera sits relative to the hero, and how quickly it catches up (1/s, exponential).
	Height = 3.2,
	Distance = 6.5,
	Follow = 4.0
}

function FollowCamera:OnCreate()
	self.Hero = Scene.FindEntityByName("Hero")
	if not self.Hero then error("FollowCamera: the scene has no entity named 'Hero'") end
end

-- Where the camera wants to be: behind and above the hero.
function FollowCamera:Wanted()
	return self.Hero.WorldPosition + Vec3(0, self.Height, -self.Distance)
end

function FollowCamera:Place(position)
	local hero = self.Hero.WorldPosition
	self.Entity.WorldPosition = position
	-- The look target keeps the camera's x, so it never turns sideways (the input stays aligned with the course).
	self.Entity:LookAt(Vec3(position.x, hero.y + 1.1, hero.z))
end

-- Jumps straight behind the hero (after a respawn): catching up from where the hero fell would look the wrong way.
function FollowCamera:Snap()
	self:Place(self:Wanted())
end

function FollowCamera:OnLateUpdate(dt)
	local position = self.Entity.WorldPosition
	self:Place(position + (self:Wanted() - position) * (1 - math.exp(-self.Follow * dt)))
end

return FollowCamera
