-- Tetris: the whole game in one controller script.
--
-- Controls: Left/Right move, Up rotate, Down soft drop, Space hard drop, R restart.
-- The board is a 10x20 grid of cube entities created at start; cells are shown or hidden and recolored
-- as the game state changes. Game state is exposed as fields (Score, Lines, Level, GameOver, Trauma) so it can be
-- inspected and tested through the automation API. Cleared lines burst into particles and shake the camera.

local Tetris = {}

Tetris.Properties = {
	Width = 10,
	Height = 20,
	Seed = 7,
	StartInterval = 0.8,  -- seconds per row at level 1
	RepeatDelay = 0.17,   -- held-key auto-repeat delay
	RepeatRate = 0.05,
	ShakeStrength = 0.4,  -- largest horizontal camera offset (world units) at full trauma
	ShakeDecay = 1.0,     -- trauma lost per second
	BurstParticles = 60,  -- particles per cleared line
}

-- Pieces as lists of rotations; each rotation is four {x, y} cells (y up).
local Shapes = {
	I = { { {0,1},{1,1},{2,1},{3,1} }, { {1,0},{1,1},{1,2},{1,3} }, { {0,2},{1,2},{2,2},{3,2} }, { {2,0},{2,1},{2,2},{2,3} } },
	O = { { {1,1},{2,1},{1,2},{2,2} } },
	T = { { {0,1},{1,1},{2,1},{1,2} }, { {1,0},{1,1},{1,2},{2,1} }, { {0,1},{1,1},{2,1},{1,0} }, { {1,0},{1,1},{1,2},{0,1} } },
	S = { { {0,1},{1,1},{1,2},{2,2} }, { {1,2},{1,1},{2,1},{2,0} }, { {0,0},{1,0},{1,1},{2,1} }, { {0,2},{0,1},{1,1},{1,0} } },
	Z = { { {0,2},{1,2},{1,1},{2,1} }, { {2,2},{2,1},{1,1},{1,0} }, { {0,1},{1,1},{1,0},{2,0} }, { {1,2},{1,1},{0,1},{0,0} } },
	J = { { {0,2},{0,1},{1,1},{2,1} }, { {1,0},{1,1},{1,2},{2,2} }, { {0,1},{1,1},{2,1},{2,0} }, { {0,0},{1,0},{1,1},{1,2} } },
	L = { { {0,1},{1,1},{2,1},{2,2} }, { {1,2},{1,1},{1,0},{2,0} }, { {0,0},{0,1},{1,1},{2,1} }, { {0,2},{1,2},{1,1},{1,0} } },
}
local Order = { "I", "O", "T", "S", "Z", "J", "L" }
local Colors = {
	I = { 0.1, 0.8, 0.9, 1 }, O = { 0.95, 0.85, 0.1, 1 }, T = { 0.65, 0.2, 0.85, 1 }, S = { 0.2, 0.85, 0.25, 1 },
	Z = { 0.9, 0.15, 0.15, 1 }, J = { 0.15, 0.3, 0.95, 1 }, L = { 0.95, 0.5, 0.1, 1 },
}
local LineScores = { 100, 300, 500, 800 }
-- Screen-shake trauma a clear adds, by lines cleared at once (a Tetris shakes hardest).
local LineTrauma = { 0.4, 0.55, 0.7, 1.0 }

function Tetris:Cell(x, y)
	return self.Board[y * self.Width + x + 1]
end

function Tetris:Fits(piece, rotation, px, py)
	for _, cell in ipairs(Shapes[piece][rotation]) do
		local x, y = px + cell[1], py + cell[2]
		if x < 0 or x >= self.Width or y < 0 then return false end
		if y < self.Height and self:Cell(x, y) ~= false then return false end
	end
	return true
end

function Tetris:NextPiece()
	-- 7-bag randomizer: every piece once per bag.
	if #self.Bag == 0 then
		for i, name in ipairs(Order) do self.Bag[i] = name end
		for i = #self.Bag, 2, -1 do
			local j = Math.RandomInt(1, i)
			self.Bag[i], self.Bag[j] = self.Bag[j], self.Bag[i]
		end
	end
	return table.remove(self.Bag)
end

function Tetris:Spawn()
	self.Piece = self:NextPiece()
	self.Rotation = 1
	self.PieceX = math.floor(self.Width / 2) - 2
	self.PieceY = self.Height - 3
	if not self:Fits(self.Piece, self.Rotation, self.PieceX, self.PieceY) then
		self.GameOver = true
		-- Force every cell to redraw in the game-over color.
		for i = 1, #self.Board do self.Shown[i] = nil end
		Log.Info("Tetris: game over with score", self.Score)
	end
end

function Tetris:Reset()
	self.Board = {}
	for i = 1, self.Width * self.Height do self.Board[i] = false end
	self.Bag = {}
	self.Score, self.Lines, self.Level = 0, 0, 1
	self.GameOver = false
	self.DropTimer = 0
	self:StopShake()
	-- Force every cell to redraw (cells may still show the game-over color).
	for i = 1, #self.Board do self.Shown[i] = nil end
	self:Spawn()
end

function Tetris:Lock()
	local overflow = false
	for _, cell in ipairs(Shapes[self.Piece][self.Rotation]) do
		local x, y = self.PieceX + cell[1], self.PieceY + cell[2]
		if y < self.Height then
			self.Board[y * self.Width + x + 1] = self.Piece
		else
			overflow = true
		end
	end
	-- A piece locked partly above the well ends the game.
	if overflow then
		self.GameOver = true
		for i = 1, #self.Board do self.Shown[i] = nil end
		Log.Info("Tetris: game over with score", self.Score)
		return
	end

	-- Clear full rows, top to bottom so indices stay valid (and each y is where the row was on screen).
	local cleared = 0
	for y = self.Height - 1, 0, -1 do
		local full = true
		for x = 0, self.Width - 1 do
			if self:Cell(x, y) == false then full = false break end
		end
		if full then
			cleared = cleared + 1
			self:BurstRow(y)
			for row = y, self.Height - 2 do
				for x = 0, self.Width - 1 do
					self.Board[row * self.Width + x + 1] = self.Board[(row + 1) * self.Width + x + 1]
				end
			end
			for x = 0, self.Width - 1 do self.Board[(self.Height - 1) * self.Width + x + 1] = false end
		end
	end
	if cleared > 0 then
		self.Lines = self.Lines + cleared
		self.Score = self.Score + LineScores[cleared] * self.Level
		self.Level = 1 + math.floor(self.Lines / 10)
		self.Trauma = math.min(1, self.Trauma + LineTrauma[cleared])
	end
	self:Spawn()
end

-- Sparks along a cleared row. The emitter simulates in world space, so moving it between bursts leaves the
-- earlier rows' particles where they were.
function Tetris:BurstRow(y)
	self.LineBurst.Translation = Vec3((self.Width - 1) / 2, y, 0.6)
	self.LineBurst:EmitParticles(self.BurstParticles)
end

-- A triangle wave in -1..1 with period 1. Only arithmetic and floor, which give the same result on every platform
-- (math.sin may differ in the last bit), so a shake moves the camera, whose transform is part of the replay hash,
-- identically everywhere.
local function Wave(x)
	return 4 * math.abs(x - math.floor(x) - 0.5) - 1
end

-- Offsets the camera from its rest position by the current trauma (squared, so small shakes stay subtle) and
-- lets the trauma decay. Driven by waves of the shake's own clock, never by Math.Random or Math.RandomInt: the
-- piece bag draws from that generator, and a shake must not change which pieces come next (replays depend on it).
function Tetris:OnLateUpdate(dt)
	if self.Trauma <= 0 then return end
	self.Trauma = math.max(0, self.Trauma - self.ShakeDecay * dt)
	if self.Trauma == 0 then
		self:StopShake()
		return
	end
	self.ShakeTime = self.ShakeTime + dt
	if not self.Camera then return end
	local amount = self.ShakeStrength * self.Trauma * self.Trauma
	local offset = Vec3(Wave(self.ShakeTime * 7.3) * amount, Wave(self.ShakeTime * 9.7 + 0.25) * amount * 0.8, 0)
	self.Camera.Translation = self.CameraRest + offset
end

function Tetris:StopShake()
	self.Trauma = 0
	self.ShakeTime = 0
	if self.Camera then self.Camera.Translation = self.CameraRest end
end

function Tetris:Move(dx, dy)
	if self:Fits(self.Piece, self.Rotation, self.PieceX + dx, self.PieceY + dy) then
		self.PieceX, self.PieceY = self.PieceX + dx, self.PieceY + dy
		return true
	end
	return false
end

function Tetris:Rotate()
	local count = #Shapes[self.Piece]
	local next = self.Rotation % count + 1
	-- Simple wall kicks: try in place, then one cell left/right, then two (for the I piece).
	for _, kick in ipairs({ 0, -1, 1, -2, 2 }) do
		if self:Fits(self.Piece, next, self.PieceX + kick, self.PieceY) then
			self.Rotation, self.PieceX = next, self.PieceX + kick
			return
		end
	end
end

-- Auto-repeat for held keys.
function Tetris:Repeat(key, dt)
	if Input.IsKeyPressed(key) then
		-- The first repeat fires RepeatDelay after the press, then every RepeatRate.
		self.Held[key] = self.RepeatRate - self.RepeatDelay
		return true
	end
	if not Input.IsKeyDown(key) then
		self.Held[key] = nil
		return false
	end
	self.Held[key] = (self.Held[key] or 0) + dt
	if self.Held[key] >= self.RepeatRate then
		self.Held[key] = 0
		return true
	end
	return false
end

function Tetris:OnCreate()
	Math.Seed(self.Seed)
	self.Held = {}

	-- Board cells and a frame around the well.
	self.Cells = {}
	self.Shown = {}
	for y = 0, self.Height - 1 do
		for x = 0, self.Width - 1 do
			local cell = Scene.CreateEntity("Cell")
			cell.Translation = Vec3(x, y, 0)
			cell.Scale = Vec3(0, 0, 0)
			cell:AddComponent("Mesh", { Mesh = "builtin://Cube" })
			cell:AddComponent("Material", { Roughness = 0.35 })
			cell:SetParent(self.Entity)
			self.Cells[y * self.Width + x + 1] = cell
			self.Shown[y * self.Width + x + 1] = false
		end
	end
	local function Wall(name, position, scale)
		local wall = Scene.CreateEntity(name)
		wall.Translation = position
		wall.Scale = scale
		wall:AddComponent("Mesh", { Mesh = "builtin://Cube" })
		wall:AddComponent("Material", { AlbedoColor = { 0.18, 0.18, 0.22, 1 }, Roughness = 0.8 })
		wall:SetParent(self.Entity)
	end
	Wall("WallLeft", Vec3(-1, self.Height / 2 - 0.5, 0), Vec3(1, self.Height + 1, 1))
	Wall("WallRight", Vec3(self.Width, self.Height / 2 - 0.5, 0), Vec3(1, self.Height + 1, 1))
	Wall("Floor", Vec3(self.Width / 2 - 0.5, -1, 0), Vec3(self.Width + 2, 1, 1))
	Wall("Back", Vec3(self.Width / 2 - 0.5, self.Height / 2 - 0.5, -0.75), Vec3(self.Width, self.Height, 0.5))

	-- Line-clear sparks: a flat box the width of the well that only emits on demand (BurstRow).
	self.LineBurst = Scene.CreateEntity("LineBurst")
	self.LineBurst:AddComponent("ParticleEmitter", {
		PlayOnStart = false, Looping = false, Rate = 0, MaxParticles = 800,
		Shape = "Box", BoxExtents = { self.Width / 2, 0.4, 0.4 },
		LifetimeMin = 0.5, LifetimeMax = 1.0, SpeedMin = 2, SpeedMax = 6, GravityScale = 1.5, Drag = 0.5,
		StartColor = { 3, 1.9, 0.6, 1 }, EndColor = { 1.6, 0.45, 0.1, 0 }, StartSize = 0.25, EndSize = 0.05,
		Blend = "Additive", SimulationSpace = "World" })
	self.LineBurst:SetParent(self.Entity)

	-- The camera shakes around where the scene placed it; without one the game plays on, unshaken.
	self.Camera = Scene.GetPrimaryCamera()
	if self.Camera then
		self.CameraRest = self.Camera.Translation
	else
		Log.Warn("Tetris: no primary camera, so cleared lines do not shake the screen")
	end

	self:Reset()
	self:Render()
end

function Tetris:OnUpdate(dt)
	if Input.IsKeyPressed("R") then self:Reset() end
	self:DrawHud()
	if self.GameOver then
		self:Render()
		return
	end

	if self:Repeat("Left", dt) then self:Move(-1, 0) end
	if self:Repeat("Right", dt) then self:Move(1, 0) end
	if Input.IsKeyPressed("Up") then self:Rotate() end
	if Input.IsKeyPressed("Space") then
		while self:Move(0, -1) do self.Score = self.Score + 2 end
		self:Lock()
	else
		local interval = math.max(0.05, self.StartInterval * (0.85 ^ (self.Level - 1)))
		if Input.IsKeyDown("Down") then interval = math.min(interval, 0.05) end
		self.DropTimer = self.DropTimer + dt
		while self.DropTimer >= interval and not self.GameOver do
			self.DropTimer = self.DropTimer - interval
			if not self:Move(0, -1) then
				self:Lock()
				self.DropTimer = 0
			end
		end
	end
	self:Render()
end

-- Mirrors the board plus the falling piece onto the cell entities, touching only cells that changed.
function Tetris:Render()
	local view = {}
	for i = 1, #self.Board do view[i] = self.Board[i] end
	if not self.GameOver then
		for _, cell in ipairs(Shapes[self.Piece][self.Rotation]) do
			local x, y = self.PieceX + cell[1], self.PieceY + cell[2]
			if y < self.Height then view[y * self.Width + x + 1] = self.Piece end
		end
	end

	for i, piece in ipairs(view) do
		if self.Shown[i] ~= piece then
			local cell = self.Cells[i]
			if piece then
				cell.Scale = Vec3(0.92, 0.92, 0.92)
				local color = self.GameOver and { 0.35, 0.35, 0.35, 1 } or Colors[piece]
				cell:SetComponent("Material", { AlbedoColor = color, EmissiveColor = { color[1] * 0.15, color[2] * 0.15, color[3] * 0.15 } })
			else
				cell.Scale = Vec3(0, 0, 0)
			end
			self.Shown[i] = piece
		end
	end
end

-- Screen-space HUD (UI coordinates: 1080 units tall, origin top-left).
function Tetris:DrawHud()
	local size = UI.GetSize()
	local x = size.x * 0.5 + 330
	UI.Rect(x - 20, 140, 300, 240, Vec4(0, 0, 0, 0.35))
	UI.Text("SCORE", x, 160, 30, Vec4(0.7, 0.7, 0.8, 1))
	UI.Text(tostring(self.Score), x, 195, 56)
	UI.Text("LINES  " .. self.Lines, x, 280, 34)
	UI.Text("LEVEL  " .. self.Level, x, 330, 34)
	UI.Text("Arrows move / rotate   Space drop   R restart", size.x * 0.5, size.y - 50, 26, Vec4(1, 1, 1, 0.7), "Center")
	if self.GameOver then
		UI.Rect(0, size.y * 0.5 - 90, size.x, 180, Vec4(0, 0, 0, 0.6))
		UI.Text("GAME OVER", size.x * 0.5, size.y * 0.5 - 70, 96, Vec4(1, 0.3, 0.25, 1), "Center")
		UI.Text("Press R to play again", size.x * 0.5, size.y * 0.5 + 30, 36, Vec4(1, 1, 1, 1), "Center")
	end
end

-- Leftmost board column of the falling piece (for tests).
function Tetris:PieceLeft()
	local left = math.huge
	for _, cell in ipairs(Shapes[self.Piece][self.Rotation]) do
		left = math.min(left, self.PieceX + cell[1])
	end
	return left
end

-- Number of occupied board cells (for tests).
function Tetris:FilledCells()
	local count = 0
	for _, piece in ipairs(self.Board) do
		if piece then count = count + 1 end
	end
	return count
end

return Tetris
