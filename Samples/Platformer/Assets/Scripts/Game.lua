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

function Game:OnCreate()
	self.Hero = RequireEntity("Hero", "SpringBone")
	self.Camera = RequireEntity("Camera", "Camera")
	self.CoinText = RequireEntity("CoinText", "UIText")
	self.MessageText = RequireEntity("MessageText", "UIText")
	-- Coin1, Coin2, ...
	self.CoinCount = 0
	while Scene.FindEntityByName("Coin" .. (self.CoinCount + 1)) do self.CoinCount = self.CoinCount + 1 end
	-- Read by the gameplay test.
	self.Coins = 0
	self.Deaths = 0
	self.Won = false
	self.Checkpoint = "Start"
	self.Respawn = self.Hero.WorldPosition
	Events.On("CoinCollected", function() self:OnCoin() end)
	Events.On("ZoneEntered", function(kind, zone) self:OnZone(kind, zone, true) end)
	Events.On("ZoneLeft", function(kind, zone) self:OnZone(kind, zone, false) end)
	self:UpdateHud()
end

function Game:OnCoin()
	self.Coins = self.Coins + 1
	self:UpdateHud()
end

function Game:OnZone(kind, zone, entered)
	if kind == "Checkpoint" and entered then
		self.Checkpoint = zone.Name
		local position = zone.WorldPosition
		self.Respawn = Vec3(position.x, position.y - zone:GetComponent("BoxCollider").HalfExtents[2], position.z)
	elseif kind == "Kill" and entered then
		self.Deaths = self.Deaths + 1
		self.Hero:GetScript():Respawn(self.Respawn)
		self.Camera:GetScript():Snap()
	elseif kind == "Goal" and entered and not self.Won then
		self.Won = true
		self.MessageText:SetComponent("UIText", { Text = string.format("COURSE CLEAR!  %d / %d COINS", self.Coins, self.CoinCount) })
	elseif kind == "Wind" then
		local springs = self.Hero:GetComponent("SpringBone")
		springs.Wind = entered and { self.Gust, 0, 0 } or { 0, 0, 0 }
		self.Hero:SetComponent("SpringBone", springs)
	end
end

function Game:UpdateHud()
	self.CoinText:SetComponent("UIText", { Text = string.format("COINS %d / %d", self.Coins, self.CoinCount) })
end

return Game
