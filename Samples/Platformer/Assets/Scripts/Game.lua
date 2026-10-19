-- Runs the platformer: counts coins, remembers the last checkpoint and puts the hero back there after a fall into the
-- pit, blows the gust onto the hero's hair and cape (SpringBone.Wind) on the windy stretch, and keeps the HUD.
local Game = {}

Game.Properties = {
	-- The gust's acceleration (m/s²) along X on the windy stretch.
	Gust = 6.0
}

local function RequireEntity(name, component)
	local entity = Scene.FindEntityByName(name)
	if not entity then error("Game: the scene has no entity named '" .. name .. "'", 2) end
	if component and not entity:HasComponent(component) then error("Game: '" .. name .. "' has no " .. component, 2) end
	return entity
end

-- Checks that the entity's script has the function `name` (all instances exist before any OnCreate runs) and returns
-- the script.
local function RequireScriptFunction(entity, name)
	local script = entity:GetScript()
	if not script or type(script[name]) ~= "function" then
		error("Game: '" .. entity.Name .. "' has no script with " .. name .. ", or its script failed to start (see the error above)", 2)
	end
	return script
end

function Game:OnCreate()
	self.Hero = RequireEntity("Hero", "SpringBone")
	self.Camera = RequireEntity("Camera", "Camera")
	RequireScriptFunction(self.Hero, "Respawn")
	RequireScriptFunction(self.Camera, "Snap")
	self.CoinText = RequireEntity("CoinText", "UIText")
	self.MessageText = RequireEntity("MessageText", "UIText")
	-- Coin1, Coin2, ... up to the first missing number.
	self.CoinCount = 0
	while Scene.FindEntityByName("Coin" .. (self.CoinCount + 1)) do self.CoinCount = self.CoinCount + 1 end
	if self.CoinCount == 0 then Log.Warn("Platformer: the scene has no Coin1") end
	-- Read by the gameplay test.
	self.Coins = 0
	self.Deaths = 0
	self.Won = false
	self.Checkpoint = "Start"
	self.Respawn = self.Hero.WorldPosition
	Events.On("CoinCollected", function(coin) self:OnCoin(coin) end)
	Events.On("ZoneEntered", function(kind, zone) self:OnZone(kind, zone, true) end)
	Events.On("ZoneLeft", function(kind, zone) self:OnZone(kind, zone, false) end)
	self:UpdateHud()
end

function Game:OnCoin(coin)
	self.Coins = self.Coins + 1
	-- The HUD counts Coin1..CoinN; a coin named outside that sequence was left out of the total.
	local number = tonumber(coin.Name:match("^Coin(%d+)$"))
	if not number or number > self.CoinCount then
		Log.Warn("Platformer: coin '" .. coin.Name .. "' is not one of Coin1..Coin" .. self.CoinCount .. ", so the total leaves it out")
	end
	self:UpdateHud()
end

function Game:OnZone(kind, zone, entered)
	if kind == "Wind" then
		local springs = self.Hero:GetComponent("SpringBone")
		springs.Wind = entered and { self.Gust, 0, 0 } or { 0, 0, 0 }
		self.Hero:SetComponent("SpringBone", springs)
		return
	end
	if not entered then return end
	if kind == "Checkpoint" then
		-- The hero comes back standing on the bottom of the checkpoint's box (an unrotated box without Offset).
		local box = zone:GetComponent("BoxCollider")
		if not box then error("Game: checkpoint '" .. zone.Name .. "' needs a BoxCollider") end
		local position = zone.WorldPosition
		self.Checkpoint = zone.Name
		self.Respawn = Vec3(position.x, position.y - box.HalfExtents[2] * zone.Scale.y, position.z)
	elseif kind == "Kill" then
		self.Deaths = self.Deaths + 1
		RequireScriptFunction(self.Hero, "Respawn"):Respawn(self.Respawn)
		RequireScriptFunction(self.Camera, "Snap"):Snap()
	elseif kind == "Goal" and not self.Won then
		self.Won = true
		self.MessageText:SetComponent("UIText", { Text = string.format("COURSE CLEAR!  %d / %d COINS", self.Coins, self.CoinCount) })
	end
end

function Game:UpdateHud()
	self.CoinText:SetComponent("UIText", { Text = string.format("COINS %d / %d", self.Coins, self.CoinCount) })
end

return Game
