-- A brick breaks the first time the ball touches it: it reports its points (Breakout.lua keeps score) and removes
-- itself. Breakout sets Points by row when it lays the bricks out.
local Brick = {}

Brick.Properties = {
	Points = 10
}

function Brick:OnCollisionBegin(other, contact)
	-- A destroyed brick lingers until the frame ends and may be touched again before then: it breaks once.
	if other.Name ~= "Ball" or self.Broken then return end
	self.Broken = true
	Events.Emit("BrickBroken", self.Entity, self.Points)
	self.Entity:Destroy()
end

return Brick
