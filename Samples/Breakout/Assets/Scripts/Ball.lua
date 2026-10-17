-- The ball reports what it hit; Breakout.lua decides what happens. Bricks report their own breaking (Brick.lua).
local Ball = {}

function Ball:OnCollisionBegin(other, contact)
	if other.Name == "Paddle" then
		Events.Emit("BallHitPaddle")
	elseif other.Name ~= "Brick" then
		Events.Emit("BallHitWall")
	end
end

function Ball:OnTriggerEnter(other, contact)
	if other.Name == "KillZone" then Events.Emit("BallLost") end
end

return Ball
