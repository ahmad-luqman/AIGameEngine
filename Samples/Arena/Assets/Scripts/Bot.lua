-- A practice target: a character that walks back and forth along a lane in the middle of the arena. It is networked
-- with Server authority, so the host walks it and clients show it InterpolationDelay behind, which is what makes
-- lag compensation matter: a client aims at where it sees the bot, and the host checks the tag against where the bot
-- was at that moment.
local Bot = {}

Bot.Properties = {
	-- The lane: from Start to End (world positions on the floor), at Speed m/s.
	Start = Vec3(-6, 0, 0),
	End = Vec3(6, 0, 0),
	Speed = 3.0
}

-- Tells a bot's script apart from a player's when a tag hits it.
Bot.IsBot = true

function Bot:OnCreate()
	-- Heading to End first.
	self.Forward = true
end

-- Back to the start of the lane (after a tag, or for a new round).
function Bot:Respawn()
	self.Entity.Translation = self.Start
	self.Forward = true
end

function Bot:OnUpdate(dt)
	-- On clients the host's snapshots move it.
	if not Network.HasAuthority(self.Entity) then return end
	local board = Scene.FindEntityByName("Scoreboard")
	local script = board and board:GetScript()
	if script and script.Over then
		self.Entity:Move(Vec3(0, 0, 0))
		return
	end
	local target = self.Forward and self.End or self.Start
	local position = self.Entity.WorldPosition
	local offset = Vec3(target.x - position.x, 0, target.z - position.z)
	if offset:Length() < 0.1 then
		self.Forward = not self.Forward
		target = self.Forward and self.End or self.Start
		offset = Vec3(target.x - position.x, 0, target.z - position.z)
	end
	self.Entity:Move(offset:Normalized() * self.Speed)
	self.Entity.Rotation = Quat.LookRotation(offset:Normalized())
end

return Bot
