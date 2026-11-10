-- A player in the arena. Its Networked component names the peer that owns it, with Predicted authority: the owner
-- moves it from its input at once (Move), the host moves it from the inputs the owner sends, and corrects the owner
-- when they disagree (a respawn after a tag). Only the owner reads input.
--
-- Space (or the pad's A or right bumper) tags: the owner asks the host to cast a ray along where it faces, at the
-- moment the owner saw (Network.GetViewTime), and the host casts it against the bodies where they stood then
-- (Network.Raycast, lag compensation). The host decides every hit from where it has the shooter, no more often than the
-- cooldown, and only as far back as the shooter's view can lag. A modified client still chooses the direction.
local Arena = require("Assets.Scripts.Lib.Arena")

local Player = {}

Player.Properties = {
	Speed = 5.0,
	-- Seconds between tags (the host allows 80% of it, for jitter in the owner's frames and in when its RPCs arrive).
	Cooldown = 0.5,
	-- How far a tag reaches, in meters.
	Range = 25.0
}

-- Which peers may call which methods: the owner (or the host) tags; only the host shows a tag on clients.
Player.Rpcs = { Tag = "Owner", ShowTag = "Host" }

-- The height a tag leaves from, and how far in front of the player the beam starts (the cast ignores the player's own
-- capsule either way).
local ChestHeight = 1.0
local MuzzleOffset = 0.5

function Player:OnCreate()
	-- Seconds until the owner may ask for another tag, and until the host accepts one. Each counts down by frame time:
	-- Network.GetTime jumps when a session starts or ends, frame time does not.
	self.OwnerCooldown = 0
	self.HostCooldown = 0
	-- Read by the gameplay test: the tags this player asked for, and on the host those it accepted, those it ignored
	-- (within the cooldown, or during the break between rounds) and those it refused as malformed.
	self.TagsRequested = 0
	self.TagsAccepted = 0
	self.TagsIgnored = 0
	self.TagsRefused = 0
	self.WarnedBeam = false
end

-- This frame's input: the move (a Vec3 across the floor, at most 1 long), whether to tag, and where to face (nil: where
-- it moves). The player's own, or the computer's when the scene has an Autoplay entity (the network test).
function Player:ReadInput()
	local autoplay = Arena.FindScript("Autoplay")
	if autoplay then return autoplay:InputFor(self.Entity) end
	return Vec3(Input.GetAction("MoveX"), 0, -Input.GetAction("MoveY")), Input.IsActionPressed("Fire"), nil
end

function Player:OnUpdate(dt)
	self.HostCooldown = math.max(self.HostCooldown - dt, 0)
	self.OwnerCooldown = math.max(self.OwnerCooldown - dt, 0)
	if not Network.IsOwner(self.Entity) then return end
	local move, fire, aim = self:ReadInput()
	if Arena.RoundOver() then
		move, fire, aim = Vec3(0, 0, 0), false, nil
	end
	if move:Length() > 1 then move = move:Normalized() end
	self.Entity:Move(move * self.Speed)
	if not aim and move:Length() > 0.1 then aim = move end
	if aim then self.Entity.Rotation = Quat.LookRotation(aim:Normalized()) end
	if fire and self.OwnerCooldown <= 0 then
		self.OwnerCooldown = self.Cooldown
		self.TagsRequested = self.TagsRequested + 1
		local direction = self.Entity:GetForward()
		if Network.IsClient() then
			local sent, reason = Network.Rpc(self.Entity, "Tag", "Server", direction, Network.GetViewTime())
			if not sent then Log.Warn("Arena: could not send a tag to the host: " .. tostring(reason)) end
		else
			-- The host's own player (or a game with no session) tags directly.
			self:Tag(0, Arena.ToArray(direction), Network.GetViewTime())
		end
	end
end

-- A tag the host turns away as malformed: counted, and logged once per player (a client could send a hundred a
-- second).
function Player:Refuse(why)
	self.TagsRefused = self.TagsRefused + 1
	if self.TagsRefused == 1 then Log.Warn("Arena: " .. self.Entity.Name .. " sent a tag that " .. why .. " (refused; not logged again)") end
end

-- Runs on the deciding peer: the host (from the owner's RPC, or its own player) or a game with no session.
function Player:Tag(sender, rawDirection, rawTime)
	if Network.IsClient() then return end
	local direction, time = Arena.ReadVec3(rawDirection), Arena.ReadTime(rawTime)
	if not direction or not time then return self:Refuse("is not a direction and a time") end
	-- Shots stay level: the arena is flat, and a shot aimed up or down would pass over or under everyone.
	direction = Vec3(direction.x, 0, direction.z)
	if direction:Length() < 0.01 then return self:Refuse("has no direction across the floor") end
	direction = direction:Normalized()
	if self.HostCooldown > 0 or Arena.RoundOver() then
		self.TagsIgnored = self.TagsIgnored + 1
		return
	end
	local now = Network.GetTime()
	-- No further back than the sender's view can lag (Network.Raycast treats a time from the future as now).
	time = math.max(time, now - (Network.GetRoundTripTime(sender) or 0) - Arena.MaxViewLag)
	-- From where the host has the player now, not where the owner says it is.
	local position = self.Entity.WorldPosition
	local origin = Vec3(position.x, position.y + ChestHeight, position.z) + direction * MuzzleOffset
	local hit = Network.Raycast(origin, direction, self.Range, time, { Ignore = self.Entity })
	self.HostCooldown = self.Cooldown * 0.8
	self.TagsAccepted = self.TagsAccepted + 1
	local from, to = Arena.ToArray(origin), Arena.ToArray(hit and hit.Point or (origin + direction * self.Range))
	self:ShowTag(0, from, to)
	if Network.IsHost() then
		local sent, reason = Network.Rpc(self.Entity, "ShowTag", "Clients", from, to)
		if not sent then Log.Warn("Arena: could not show a tag on every client: " .. tostring(reason)) end
	end
	-- Match scores it (an event, so an error there stops Match's script rather than this one).
	if hit then Events.Emit("ArenaTagged", self.Entity, hit.Entity) end
end

-- Draws the tag's beam for a moment on every peer (`from` and `to` arrive as arrays). Only the host sends it (Rpcs), so a
-- bad value is a bug there: warned about once.
function Player:ShowTag(sender, rawFrom, rawTo)
	local from, to = Arena.ReadVec3(rawFrom), Arena.ReadVec3(rawTo)
	local body = self.Entity:FindChild(Arena.BodyName)
	local material = body and body:GetComponent("Material")
	if not from or not to or not material then
		if not self.WarnedBeam then Log.Warn("Arena: " .. self.Entity.Name .. " cannot draw a tag from " .. tostring(rawFrom) .. " to " .. tostring(rawTo) .. " (not logged again)") end
		self.WarnedBeam = true
		return
	end
	-- A tag that hit something right in front of the player (a wall within the muzzle offset) leaves no beam to draw.
	local length = (to - from):Length()
	if length < 0.01 then return end
	local color = material.AlbedoColor
	local beam = Scene.CreateEntity("TagBeam")
	beam.Translation = (from + to) * 0.5
	beam.Rotation = Quat.LookRotation((to - from):Normalized())
	beam.Scale = Vec3(0.08, 0.08, length)
	beam:AddComponent("Mesh", { Mesh = "builtin://Cube" })
	beam:AddComponent("Material", { AlbedoColor = color, EmissiveColor = { color[1], color[2], color[3] }, EmissiveIntensity = 4 })
	beam:AddComponent("Script", { Script = "Assets/Scripts/Fade.lua" })
end

return Player
