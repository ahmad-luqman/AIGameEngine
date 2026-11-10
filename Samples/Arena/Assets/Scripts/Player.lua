-- A player in the arena. Its Networked component names the peer that owns it, with Predicted authority: the owner
-- moves it from its input at once (Move), the host moves it from the inputs the owner sends, and corrects the owner
-- when they disagree (a respawn after a tag). Only the owner reads input.
--
-- Space (or the pad's A or right bumper) tags: the owner asks the host to cast a ray along where it faces, at the
-- moment the owner saw (Network.GetViewTime), and the host casts it against the bodies where they stood then
-- (Network.Raycast, lag compensation). The host decides every hit, so a modified client cannot tag what it did not aim
-- at, nor tag faster than the cooldown.
local Player = {}

Player.Properties = {
	Speed = 5.0,
	-- Seconds between tags (the host allows a little less, for the jitter of the owner's frames).
	Cooldown = 0.5,
	-- How far a tag reaches, in meters.
	Range = 25.0
}

-- Tells a player's script apart from a bot's (Bot.IsBot) when a tag hits it.
Player.IsPlayer = true

-- Which peers may call which methods: the owner (or the host) tags; only the host shows a tag on clients.
Player.Rpcs = { Tag = "Owner", ShowTag = "Host" }

-- The height a tag leaves from and where its ray starts ahead of the player (outside its own capsule).
local ChestHeight = 1.0
local MuzzleOffset = 0.5

-- A vector a peer sent (an array of three numbers), or nil when it is anything else. Vectors arrive as arrays, and
-- what a client sends is untrusted: a bad value must not raise an error in the host's script.
local function ReadVec3(value)
	if type(value) ~= "table" then return nil end
	local x, y, z = value[1], value[2], value[3]
	for _, number in ipairs({ x, y, z }) do
		if type(number) ~= "number" or number ~= number or math.abs(number) > 1e6 then return nil end
	end
	return Vec3(x, y, z)
end

local function FindMatch()
	local match = Scene.FindEntityByName("Match")
	return match and match:GetScript()
end

local function FindScoreboard()
	local board = Scene.FindEntityByName("Scoreboard")
	return board and board:GetScript()
end

function Player:OnCreate()
	self.NextTag = 0
	self.LastTagTime = -math.huge
	-- Read by the gameplay test: the tags this player asked for, those the host accepted, and those it refused as
	-- malformed (a tag within the cooldown, or during the break between rounds, is ignored without counting).
	self.TagsRequested = 0
	self.TagsAccepted = 0
	self.TagsRefused = 0
end

-- Whether the round is over (the scoreboard shows the winner): players stand still until the next one.
local function RoundOver()
	local board = FindScoreboard()
	return board ~= nil and board.Over == true
end

function Player:OnUpdate(dt)
	if not Network.IsOwner(self.Entity) then return end
	local move, fire
	local match = FindMatch()
	if match and match.Autoplay then
		move, fire = self:Autoplay()
	else
		move = Vec3(Input.GetAction("MoveX"), 0, -Input.GetAction("MoveY"))
		fire = Input.IsActionPressed("Fire")
	end
	if RoundOver() then
		move, fire = Vec3(0, 0, 0), false
	end
	if move:Length() > 1 then move = move:Normalized() end
	self.Entity:Move(move * self.Speed)
	if move:Length() > 0.1 then self.Entity.Rotation = Quat.LookRotation(move:Normalized()) end
	self.NextTag = math.max(self.NextTag - dt, 0)
	if fire and self.NextTag <= 0 then
		self.NextTag = self.Cooldown
		self.TagsRequested = self.TagsRequested + 1
		local direction = self.Entity:GetForward()
		if Network.IsClient() then
			Network.Rpc(self.Entity, "Tag", "Server", direction, Network.GetViewTime())
		else
			-- The host's own player (or a game with no session) tags directly.
			self:Tag(0, { direction.x, direction.y, direction.z }, Network.GetViewTime())
		end
	end
end

-- Runs on the deciding peer: the host (from the owner's RPC, or its own player) or a game with no session.
function Player:Tag(sender, rawDirection, time)
	if Network.IsClient() then return end
	local direction = ReadVec3(rawDirection)
	if not direction or type(time) ~= "number" or time ~= time then
		self.TagsRefused = self.TagsRefused + 1
		Log.Warn("Arena: " .. self.Entity.Name .. " sent a tag that is not a direction and a time")
		return
	end
	-- Shots stay level: the arena is flat, and a shot aimed up or down would pass over or under everyone.
	direction = Vec3(direction.x, 0, direction.z)
	if direction:Length() < 0.01 then return end
	direction = direction:Normalized()
	local now = Network.GetTime()
	if now - self.LastTagTime < self.Cooldown * 0.8 then return end
	if RoundOver() then return end
	self.LastTagTime = now
	self.TagsAccepted = self.TagsAccepted + 1
	-- From where the host has the player now, not where the owner says it is.
	local position = self.Entity.WorldPosition
	local origin = Vec3(position.x, position.y + ChestHeight, position.z) + direction * MuzzleOffset
	local hit = Network.Raycast(origin, direction, self.Range, time, { Ignore = self.Entity })
	local stop = hit and hit.Point or (origin + direction * self.Range)
	self:ShowTag(0, { origin.x, origin.y, origin.z }, { stop.x, stop.y, stop.z })
	if Network.IsHost() then
		Network.Rpc(self.Entity, "ShowTag", "Clients", { origin.x, origin.y, origin.z }, { stop.x, stop.y, stop.z })
	end
	local target = hit and hit.Entity
	local script = target and target:GetScript()
	if script and (script.IsBot or script.IsPlayer) then
		local match = FindMatch()
		if match then match:OnTag(self.Entity, target) end
	end
end

-- Draws the tag's beam for a moment on every peer (`from` and `to` arrive as arrays).
function Player:ShowTag(sender, rawFrom, rawTo)
	local from, to = ReadVec3(rawFrom), ReadVec3(rawTo)
	if not from or not to then return end
	local length = (to - from):Length()
	if length < 0.01 then return end
	local beam = Scene.CreateEntity("TagBeam")
	beam.Translation = (from + to) * 0.5
	beam.Rotation = Quat.LookRotation((to - from):Normalized())
	beam.Scale = Vec3(0.08, 0.08, length)
	local color = self.Entity:FindChild("PlayerBody"):GetComponent("Material").AlbedoColor
	beam:AddComponent("Mesh", { Mesh = "builtin://Cube" })
	beam:AddComponent("Material", { AlbedoColor = color, EmissiveColor = { color[1], color[2], color[3] }, EmissiveIntensity = 4 })
	beam:AddComponent("Script", { Script = "Assets/Scripts/Fade.lua" })
end

-- Autoplay: the computer's input for this player (see Match.Autoplay). Player 1 walks to a spot on the near side and
-- tags along it every cooldown; player 2 walks to the far end of that side and stands there, and walks back along the
-- right-hand side after every tag (which puts it in the far corner), so player 1 wins the round however the peers'
-- timing goes. Others stand still.
function Player:Autoplay()
	local peer = self.Entity:GetComponent("Networked").Owner
	local spot, aim
	if peer == 1 then
		spot, aim = Vec3(-6, 0, -8), Vec3(1, 0, 0)
	elseif peer == 2 then
		spot = Vec3(8, 0, -8)
	else
		return Vec3(0, 0, 0), false
	end
	local position = self.Entity.WorldPosition
	local offset = Vec3(spot.x - position.x, 0, spot.z - position.z)
	local distance = offset:Length()
	local move = Vec3(0, 0, 0)
	if distance > 0.05 then
		-- Slow down on arrival rather than overshoot (a step at full speed covers 8 cm).
		move = offset:Normalized() * math.min(1, distance * 2)
	elseif aim then
		self.Entity.Rotation = Quat.LookRotation(aim)
	end
	return move, aim ~= nil and distance <= 0.05
end

return Player
