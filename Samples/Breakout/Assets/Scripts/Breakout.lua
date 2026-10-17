-- Runs Breakout: lays out the bricks, moves the paddle from the input actions (and the mouse), serves and steers
-- the ball, keeps score and lives, and shows the HUD. The ball and the bricks report what they hit through events
-- (Ball.lua, Brick.lua); the Restart button calls OnRestart (its Target is this entity).
--
-- Every value that feeds the simulation avoids the platform's trigonometry (see "Math types" in the scripting
-- guide), so the game plays identically on every platform: bounce directions are normalized vectors, not angles.
local Breakout = {}

Breakout.Properties = {
	Columns = 8,
	Rows = 5,
	BrickWidth = 1.8,
	BrickHeight = 0.6,
	BrickGap = 0.15,
	-- The center of the top row.
	TopRow = 12.5,
	Lives = 3,
	BallSpeed = 9.0,
	PaddleSpeed = 14.0,
	PaddleY = 1.0,
	PaddleHalfWidth = 1.2,
	-- The paddle's center stays within this distance of the middle (the walls' inner faces minus half a paddle).
	PaddleLimit = 6.8,
	-- How far the paddle travels when the mouse crosses the whole view.
	MouseTravel = 24.0,
	-- The sideways part of a bounce off the paddle's edge, for an upward part of 1 (about 56 degrees off vertical).
	MaxBounce = 1.5,
	-- When the upward or downward part of the ball's direction drops below this, it is pushed back up to about this,
	-- so the ball cannot bounce sideways between the walls forever.
	MinVertical = 0.35,
	-- Demo mode: the paddle follows the ball and serves by itself (an attract screen; the gameplay test uses it).
	Autopilot = false
}

local BrickPrefab = "Assets/Prefabs/Brick.bprefab"
local BallRadius = 0.25
-- Top row first: the higher a brick, the more it is worth.
local RowColors = {
	{ 0.90, 0.06, 0.10, 1 },
	{ 0.95, 0.38, 0.02, 1 },
	{ 0.95, 0.78, 0.04, 1 },
	{ 0.08, 0.70, 0.18, 1 },
	{ 0.08, 0.35, 0.95, 1 }
}
-- Where on the paddle the autopilot aims, in turn (fractions of its half width), so its bounces vary.
local AimOffsets = { 0.55, -0.35, 0.8, -0.7, 0.2 }
local Sounds = { Bounce = "Assets/Audio/Bounce.wav", Break = "Assets/Audio/Break.wav", Lose = "Assets/Audio/Lose.wav" }
local Messages = {
	Serve = "Press Space, A or click to launch",
	Play = "",
	GameOver = "GAME OVER",
	Won = "YOU WIN!"
}

-- A missing piece of the scene (or one without the component it is driven through) is reported by name.
local function RequireEntity(name, component)
	local entity = Scene.FindEntityByName(name)
	if not entity then error("Breakout: the scene has no entity named '" .. name .. "'", 2) end
	if component and not entity:HasComponent(component) then error("Breakout: '" .. name .. "' has no " .. component, 2) end
	return entity
end

function Breakout:OnCreate()
	self.Paddle = RequireEntity("Paddle", "RigidBody")
	self.Ball = RequireEntity("Ball", "RigidBody")
	self.Burst = RequireEntity("BrickBurst", "ParticleEmitter")
	self.ScoreText = RequireEntity("ScoreText", "UIText")
	self.LivesText = RequireEntity("LivesText", "UIText")
	self.Message = RequireEntity("Message", "UIText")
	self.RestartButton = RequireEntity("RestartButton", "UIButton")
	-- Read by the gameplay test.
	self.PaddleHits = 0
	self.Aim = 1
	Events.On("BrickBroken", function(brick, points) self:OnBrickBroken(brick, points) end)
	Events.On("BallHitPaddle", function() self:OnBallHitPaddle() end)
	Events.On("BallHitWall", function() self:Play("Bounce", 0.4) end)
	Events.On("BallLost", function() self:OnBallLost() end)
	self:Restart()
end

function Breakout:Restart()
	for _, brick in ipairs(Scene.FindEntitiesByName("Brick")) do brick:Destroy() end
	self.Score = 0
	self.LivesLeft = self.Lives
	self.BricksLeft = 0
	self.PaddleX = 0
	self.PaddleVelocity = 0
	local step = self.BrickWidth + self.BrickGap
	local left = -(self.Columns - 1) * step / 2
	for row = 1, self.Rows do
		local color = RowColors[(row - 1) % #RowColors + 1]
		for column = 1, self.Columns do
			local position = Vec3(left + (column - 1) * step, self.TopRow - (row - 1) * (self.BrickHeight + self.BrickGap), 0)
			local brick = Scene.Instantiate(BrickPrefab, position)
			brick:SetComponent("Material", { AlbedoColor = color, EmissiveColor = { color[1] * 0.35, color[2] * 0.35, color[3] * 0.35 } })
			local script = brick:GetScript()
			if not script then error("Breakout: " .. BrickPrefab .. " has no running Brick script") end
			script.Points = (self.Rows - row + 1) * 10
			self.BricksLeft = self.BricksLeft + 1
		end
	end
	-- The key or button press that restarted the game (Space, Enter or A on the focused button) must not also serve
	-- the ball. A click restarts on release, when nothing is held any more.
	self.WaitForRelease = Input.IsActionDown("Launch")
	self:Serve()
end

-- The ball waits on the paddle until the player launches it.
function Breakout:Serve()
	self.State = "Serve"
	self:StopBall()
	self:FollowPaddle()
	self:UpdateHud()
end

function Breakout:FollowPaddle()
	self.Ball.Translation = Vec3(self.PaddleX, self.PaddleY + 0.2 + BallRadius + 0.05, 0)
end

function Breakout:Launch()
	self.State = "Play"
	-- Up and a little to the side of the paddle's last movement, so a straight serve does not repeat forever.
	local side = self.PaddleVelocity < 0 and -1 or 1
	self:SetBallDirection(Vec2(0.35 * side, 1))
	self:UpdateHud()
end

function Breakout:SetBallDirection(direction)
	local d = direction:Normalized()
	self.Ball:SetLinearVelocity(Vec3(d.x, d.y, 0) * self.BallSpeed)
end

function Breakout:StopBall()
	self.Ball:SetLinearVelocity(Vec3(0, 0, 0))
end

function Breakout:OnUpdate(dt)
	self:MovePaddle(dt)
	if self.WaitForRelease and not Input.IsActionDown("Launch") then self.WaitForRelease = false end
	local launch = not self.WaitForRelease and (Input.IsActionPressed("Launch") or self.Autopilot)
	if self.State == "Serve" then
		self:FollowPaddle()
		if launch then self:Launch() end
	elseif self.State == "Play" then
		self:KeepBallMoving()
	end
	-- The cursor is hidden while the paddle follows the mouse; the menus need it.
	Input.SetCursorMode((self.State == "Serve" or self.State == "Play") and "Hidden" or "Normal")
end

function Breakout:MovePaddle(dt)
	local before = self.PaddleX
	local x = before
	if self.Autopilot then
		local target = self.Ball.Translation.x - AimOffsets[self.Aim] * self.PaddleHalfWidth
		local reach = self.PaddleSpeed * 1.5 * dt
		x = x + Math.Clamp(target - x, -reach, reach)
	else
		x = x + Input.GetAction("MoveX") * self.PaddleSpeed * dt
		-- The mouse moves the paddle by how far it moved across the view, so it works the same in a window of any
		-- size and in the editor's Game view.
		local view = Input.GetViewSize()
		if view.x > 0 then x = x + Input.GetMouseDelta().x / view.x * self.MouseTravel end
	end
	self.PaddleX = Math.Clamp(x, -self.PaddleLimit, self.PaddleLimit)
	self.PaddleVelocity = dt > 0 and (self.PaddleX - before) / dt or 0
	self.Paddle.Translation = Vec3(self.PaddleX, self.PaddleY, 0)
end

-- Bounces keep the speed only approximately and can drift out of the plane: put both back every frame.
function Breakout:KeepBallMoving()
	local velocity = self.Ball:GetLinearVelocity()
	local direction = Vec2(velocity.x, velocity.y)
	if direction:Length() < 1e-3 then direction = Vec2(0, 1) end
	local normalized = direction:Normalized()
	if math.abs(normalized.y) < self.MinVertical then
		direction = Vec2(normalized.x, (normalized.y < 0 and -1 or 1) * self.MinVertical)
	end
	self:SetBallDirection(direction)
	local position = self.Ball.Translation
	if position.z ~= 0 then self.Ball.Translation = Vec3(position.x, position.y, 0) end
end

-- Where the ball meets the paddle sets where it goes: straight up from the middle, steeper sideways toward the edges.
function Breakout:OnBallHitPaddle()
	if self.State ~= "Play" then return end
	self.PaddleHits = self.PaddleHits + 1
	self.Aim = self.Aim % #AimOffsets + 1
	local offset = Math.Clamp((self.Ball.Translation.x - self.PaddleX) / (self.PaddleHalfWidth + BallRadius), -1, 1)
	self:SetBallDirection(Vec2(offset * self.MaxBounce, 1))
	self:Play("Bounce", 0.6)
end

function Breakout:OnBrickBroken(brick, points)
	self.Score = self.Score + points
	self.BricksLeft = self.BricksLeft - 1
	local burst = self.Burst
	burst.Translation = brick.Translation
	local color = brick:GetComponent("Material").AlbedoColor
	burst:SetComponent("ParticleEmitter", { StartColor = color, EndColor = { color[1], color[2], color[3], 0 } })
	burst:EmitParticles(16)
	self:Play("Break", 0.7)
	if self.BricksLeft <= 0 then
		self.State = "Won"
		self:StopBall()
		Log.Info("Breakout: cleared every brick")
	end
	self:UpdateHud()
end

function Breakout:OnBallLost()
	if self.State ~= "Play" then return end
	self.LivesLeft = self.LivesLeft - 1
	self:Play("Lose", 0.8)
	if self.LivesLeft <= 0 then
		self.State = "GameOver"
		self:StopBall()
		self:FollowPaddle()
		Log.Info("Breakout: game over")
		self:UpdateHud()
	else
		self:Serve()
	end
end

function Breakout:Play(sound, volume)
	local clip = Sounds[sound]
	if not clip then error("Breakout: unknown sound '" .. tostring(sound) .. "'", 2) end
	Audio.PlayOneShot(clip, self.Ball.WorldPosition, volume)
end

function Breakout:UpdateHud()
	self.ScoreText:SetComponent("UIText", { Text = string.format("SCORE %d", self.Score) })
	self.LivesText:SetComponent("UIText", { Text = string.format("LIVES %d", self.LivesLeft) })
	local message = Messages[self.State]
	if message == nil then error("Breakout: no message for state '" .. tostring(self.State) .. "'") end
	self.Message:SetComponent("UIText", { Text = message })
	local menu = self.State == "GameOver" or self.State == "Won"
	self.RestartButton:SetComponent("UIRect", { Visible = menu })
	-- Focused, the button also restarts from the keyboard and gamepads (Space, Enter, A).
	if menu then UI.SetFocus(self.RestartButton) end
end

-- The Restart button (shown and focused on game over and after a win).
function Breakout:OnRestart(button)
	self:Restart()
end

return Breakout
