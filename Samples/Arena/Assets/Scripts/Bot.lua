-- A practice target: a character that walks back and forth along a lane across the arena. It is networked with Server
-- authority, so the host walks it and clients show it InterpolationDelay behind, which is what makes lag compensation
-- matter: a client aims at where it sees the bot, and the host checks the tag against where the bot was at that moment.
local Arena = require("Assets.Scripts.Lib.Arena")

local Bot = {}

Bot.Properties = {
	-- The lane: from Start to End (world positions on the floor), at Speed m/s.
	Start = Vec3(-6, 0, 0),
	End = Vec3(6, 0, 0),
	Speed = 3.0
}

-- Tells a bot's script apart from a player's (Player.IsPlayer).
Bot.IsBot = true

-- Ends closer than this turn the bot around.
local Arrived = 0.1

function Bot:OnCreate()
	if Arena.FlatOffset(self.Start, self.End):Length() <= 2 * Arrived then
		error("Bot: '" .. self.Entity.Name .. "' needs a lane: its Start and End are less than " .. 2 * Arrived .. " m apart")
	end
	self.Forward = true
end

-- Back to the start of the lane (after a tag, or for a new round).
function Bot:Respawn()
	self.Entity.Translation = self.Start
	self.Forward = true
end

-- The end of the lane it is heading to.
function Bot:Target()
	return self.Forward and self.End or self.Start
end

function Bot:OnUpdate(dt)
	-- On clients the host's snapshots move it.
	if not Network.HasAuthority(self.Entity) then return end
	if Arena.RoundOver() then
		self.Entity:Move(Vec3(0, 0, 0))
		return
	end
	local offset = Arena.FlatOffset(self.Entity.WorldPosition, self:Target())
	if offset:Length() < Arrived then
		self.Forward = not self.Forward
		offset = Arena.FlatOffset(self.Entity.WorldPosition, self:Target())
	end
	local direction = offset:Normalized()
	self.Entity:Move(direction * self.Speed)
	self.Entity.Rotation = Quat.LookRotation(direction)
end

return Bot
