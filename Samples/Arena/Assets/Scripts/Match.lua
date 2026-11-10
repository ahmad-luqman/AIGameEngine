-- Runs the arena's match on the peer that decides it (the host, or a game playing alone): spawns a player for every
-- peer (Player.bprefab, owned by that peer, and predicted on it for a client), scores the tags players land, respawns
-- whoever was tagged and ends the round when someone reaches WinScore. Scripts run on every peer: on a client it keeps
-- the HUD's status line and, once the host has gone away, lets the player go back to practice (Enter, or the pad's
-- Start). Scores reach clients through the Scoreboard's RPC.
--
-- This entity is not networked on purpose: a client keeps it (and its Network.On handlers) when the host's networked
-- entities replace its own.
local Arena = require("Assets.Scripts.Lib.Arena")

local Match = {}

Match.Properties = {
	-- Tags that win a round (a whole number, at least 1).
	WinScore = 5,
	-- Seconds the winner is shown before the next round starts (at least 0).
	RoundBreak = 3.0,
	-- Whether the host plays too: "Auto" (unless it is headless: a dedicated server), "Yes" or "No".
	HostPlays = "Auto"
}

local PlayerPrefab = "Assets/Prefabs/Player.bprefab"

-- Where players start and come back after a tag: the corners, then the middle of the north and south sides (north is
-- -Z, away from the camera).
local Spawns = { Vec3(-8, 0, -8), Vec3(8, 0, -8), Vec3(-8, 0, 8), Vec3(8, 0, 8), Vec3(0, 0, -8.5), Vec3(0, 0, 8.5) }
-- By peer ID (the host is 0: gold); peer IDs are never reused in a session, so a fifth client to join takes gold too.
local Colors = {
	{ 0.95, 0.85, 0.25, 1 },
	{ 0.9, 0.25, 0.2, 1 },
	{ 0.2, 0.45, 0.95, 1 },
	{ 0.25, 0.8, 0.3, 1 },
	{ 0.75, 0.3, 0.85, 1 }
}

-- Whether the host plays, by HostPlays.
local HostPlaysModes = {
	Auto = function() return not Game.IsHeadless() end,
	Yes = function() return true end,
	No = function() return false end
}

-- What each setting may hold. Scripts and tests may change them in play.
local SettingChecks = {
	WinScore = function(value) return math.type(value) == "integer" and value >= 1 end,
	RoundBreak = function(value) return type(value) == "number" and value >= 0 end,
	HostPlays = function(value) return HostPlaysModes[value] ~= nil end
}

local function RequireEntity(name, component)
	local entity = Scene.FindEntityByName(name)
	if not entity then error("Match: the scene has no entity named '" .. name .. "'", 2) end
	if component and not entity:HasComponent(component) then error("Match: '" .. name .. "' has no " .. component, 2) end
	return entity
end

-- The keys of a table, sorted, so every run visits peers in the same order.
local function SortedKeys(t)
	local keys = {}
	for key in pairs(t) do keys[#keys + 1] = key end
	table.sort(keys)
	return keys
end

-- Whether this peer decides the match: the host, or a game with no session.
local function Decides()
	return not Network.IsClient()
end

-- Whether an entity is a player: players, and only players, are predicted (bots are the host's).
local function IsPlayer(entity)
	local networked = entity:GetComponent("Networked")
	return networked ~= nil and networked.Authority == "Predicted"
end

function Match:OnCreate()
	-- A setting the scene gets wrong is an error now; one changed in play to a wrong value is warned about and the last
	-- good one kept (an error there would stop this script, and with it the match, every frame after).
	self.GoodSettings = {}
	for name, check in pairs(SettingChecks) do
		if not check(self[name]) then error("Match: " .. name .. " cannot be " .. tostring(self[name])) end
		self.GoodSettings[name] = self[name]
	end
	self.WarnedSetting = nil
	self.StatusText = RequireEntity("StatusText", "UIText")
	self.Scoreboard = RequireEntity("Scoreboard", "Networked")
	-- On the deciding peer, by peer ID: { Entity = the player, Score = its tags this round }. Clients show the
	-- Scoreboard's copy of the scores instead.
	self.Roster = {}
	-- While a round's break runs: { Winner = peer, Left = seconds, Holds = { [peer] = { Position, Grace } } }; nil during
	-- play.
	self.Break = nil
	-- The current table (rows by peer, and the winner or false), and the clients that have not had it yet: a send can
	-- fail when that client's message rate or bandwidth is spent.
	self.Rows, self.Winner = {}, false
	self.Unsent = {}
	self.SendFailing = false
	-- Why this peer's last session as a client ended, shown while it practises again.
	self.LeftReason = nil
	self.Role = Network.GetRole()
	Network.On("PeerConnected", function(peer)
		Log.Info("Arena: " .. Arena.PeerName(peer) .. " joined")
		-- RPCs are not replayed to clients that join later: the newcomer gets the whole table now. (Here a join also adds a
		-- row, which publishes anyway; this keeps it right for state a join does not change.)
		self.Unsent[peer] = true
	end)
	Network.On("PeerDisconnected", function(peer, reason) Log.Info("Arena: " .. Arena.PeerName(peer) .. " left (" .. reason .. ")") end)
	Network.On("Connected", function(peer) Log.Info("Arena: joined as " .. Arena.PeerName(peer)) end)
	-- A player's tag landed (Player:Tag). An event rather than a call, so an error in scoring stops this script, not the
	-- shooter's.
	Events.On("ArenaTagged", function(shooter, target) self:OnTag(shooter, target) end)
	self:UpdateStatus()
end

-- A setting's value, or its last good one when a script set it to something it cannot be (warned about once).
function Match:Setting(name)
	local value = self[name]
	if SettingChecks[name](value) then
		self.GoodSettings[name] = value
		return value
	end
	if self.WarnedSetting ~= name then Log.Warn("Match: " .. name .. " cannot be " .. tostring(value) .. "; it stays " .. tostring(self.GoodSettings[name])) end
	self.WarnedSetting = name
	return self.GoodSettings[name]
end

-- Whether the host has a player of its own now.
function Match:HostPlaysNow()
	return HostPlaysModes[self:Setting("HostPlays")]()
end

-- The peers that should have a player now.
function Match:WantedPeers()
	local wanted = {}
	if not Network.IsActive() then
		wanted[0] = true
		return wanted
	end
	if self:HostPlaysNow() then wanted[0] = true end
	for _, peer in ipairs(Network.GetPeers()) do wanted[peer] = true end
	return wanted
end

function Match:SpawnPlayer(peer)
	local player = Scene.Instantiate(PlayerPrefab, self:FreeSpawn(nil))
	local body = player:FindChild(Arena.BodyName)
	if not body or not IsPlayer(player) then
		player:Destroy()
		error("Match: " .. PlayerPrefab .. " needs Predicted authority and a child named " .. Arena.BodyName)
	end
	player.Name = "Player" .. peer
	-- Before the next tick, which spawns it on the clients with these values: the owner predicts it from then on.
	player:SetComponent("Networked", { Owner = peer })
	-- The body is a child that is not networked: the spawn carries its color once, which is all it needs.
	body:SetComponent("Material", { AlbedoColor = Colors[peer % #Colors + 1] })
	self.Roster[peer] = { Entity = player, Score = 0 }
end

-- The spawn point whose nearest other player is farthest away (bots do not count; the first of equals within a
-- millimetre, so every run agrees): where a new player starts and a tagged one comes back. `except` is the player being
-- placed.
function Match:FreeSpawn(except)
	local best, bestDistance = Spawns[1], -1
	for _, spawn in ipairs(Spawns) do
		local nearest = math.huge
		for _, entry in pairs(self.Roster) do
			if entry.Entity ~= except then
				nearest = math.min(nearest, Arena.FlatOffset(entry.Entity.WorldPosition, spawn):Length())
			end
		end
		if nearest > bestDistance + 0.001 then
			best, bestDistance = spawn, nearest
		end
	end
	return best
end

-- The peer whose player `entity` is, or nil.
function Match:PeerOf(entity)
	for peer, entry in pairs(self.Roster) do
		if entry.Entity == entity then return peer end
	end
	return nil
end

-- Spawns and removes players so that every wanted peer has one: peers come and go, a player can be destroyed by
-- something else, and a session can start or end under a game already playing (the host's own player goes when it
-- turns into a dedicated server). Players this peer does not keep (a host's, left behind when a client's session ended)
-- are removed. Every roster entity is valid afterwards.
function Match:SyncPlayers()
	local wanted = self:WantedPeers()
	local changed = false
	-- Destroyed at the end of the frame: until then they are still in the scene.
	local removed = {}
	for peer, entry in pairs(self.Roster) do
		if not wanted[peer] or not entry.Entity:IsValid() then
			if entry.Entity:IsValid() then
				entry.Entity:Destroy()
				removed[entry.Entity.ID] = true
			end
			self.Roster[peer] = nil
			changed = true
		end
	end
	for _, entity in ipairs(Scene.GetEntitiesWith("Networked")) do
		if IsPlayer(entity) and not removed[entity.ID] and not self:PeerOf(entity) then
			Log.Info("Arena: removing " .. entity.Name .. ", which is nobody's player here")
			entity:Destroy()
		end
	end
	for _, peer in ipairs(SortedKeys(wanted)) do
		if not self.Roster[peer] then
			self:SpawnPlayer(peer)
			changed = true
		end
	end
	return changed
end

function Match:OnUpdate(dt)
	local role = Network.GetRole()
	if role ~= self.Role then
		-- A session started or ended: a new match. (A client's practice player goes when the host accepts it, as it is
		-- networked; SyncPlayers forgets it when this peer decides again.)
		self.Role = role
		if role ~= nil then self.LeftReason = nil end
		self:ResetRound()
		self:Publish()
	end
	-- A client whose host went away stays in its ended session, so the runtime still reports it (a dialog, or exit code
	-- 8 for a --frames run), until its player asks to practise.
	if Network.GetState() == "Disconnected" and Input.IsActionPressed("Practice") then
		self.LeftReason = Network.GetDisconnectReason()
		Network.Disconnect()
	end
	self:UpdateStatus()
	if not Decides() then return end
	if self:SyncPlayers() then self:Publish() end
	if self.Break then
		self.Break.Left = self.Break.Left - dt
		if self.Break.Left <= 0 then
			self:StartRound()
		else
			self:HoldPlayers(dt)
		end
	end
	self:SendScores()
end

-- A player's tag hit `target` (whatever the ray hit; only players and bots score), on the deciding peer.
function Match:OnTag(shooter, target)
	if self.Break then return end
	local shooterPeer = self:PeerOf(shooter)
	local targetPeer = self:PeerOf(target)
	local bot = target:GetScript()
	if not shooterPeer or not (targetPeer or (bot and bot.IsBot)) then return end
	local winScore = self:Setting("WinScore")
	local entry = self.Roster[shooterPeer]
	entry.Score = entry.Score + 1
	if targetPeer then
		target.Translation = self:FreeSpawn(target)
	else
		bot:Respawn()
	end
	Log.Info("Arena: " .. Arena.PeerName(shooterPeer) .. " tagged " .. target.Name .. " (" .. entry.Score .. ")")
	if entry.Score >= winScore then self:EndRound(shooterPeer) end
	-- At once, so the scoreboard shows the tag the frame it lands.
	self:Publish()
end

-- Someone won: show it for RoundBreak seconds, with everyone standing still.
function Match:EndRound(winner)
	self.Break = { Winner = winner, Left = self:Setting("RoundBreak"), Holds = {} }
end

-- A new round, or a new match: no break, and every score back to 0.
function Match:ResetRound()
	self.Break = nil
	for _, entry in pairs(self.Roster) do entry.Score = 0 end
end

-- During the break players stand still. Their scripts stop moving them once they hear of it, but the host moves a
-- predicted character from whatever its owner sends, so a modified client could walk on: the host puts it back. Each
-- player (one that joins during the break too) is held where it stands once its owner has had time to hear: its round
-- trip and the view lag, during which an honest player's last steps are still arriving.
function Match:HoldPlayers(dt)
	for peer, entry in pairs(self.Roster) do
		local hold = self.Break.Holds[peer]
		if not hold then
			hold = { Grace = (Network.GetRoundTripTime(peer) or 0) + Arena.MaxViewLag }
			self.Break.Holds[peer] = hold
		end
		if hold.Grace > 0 then
			hold.Grace = hold.Grace - dt
			hold.Position = entry.Entity.Translation
		elseif Arena.FlatOffset(entry.Entity.Translation, hold.Position):Length() > 0.01 then
			entry.Entity.Translation = hold.Position
		end
	end
end

function Match:StartRound()
	self:ResetRound()
	-- Everyone back to a spawn point, in peer order so every run agrees; a player not placed yet still counts where
	-- it stands, which at worst leaves two players a little closer than they could be.
	for _, peer in ipairs(SortedKeys(self.Roster)) do
		local entry = self.Roster[peer]
		entry.Entity.Translation = self:FreeSpawn(entry.Entity)
	end
	for _, entity in ipairs(Scene.GetEntitiesWith("Script")) do
		local script = entity:GetScript()
		if script and script.IsBot then script:Respawn() end
	end
	self:Publish()
end

-- The scoreboard rows (by peer) and the winner (false during play), whole, so one message brings a client up to date:
-- shown on this peer at once, and sent to every client by SendScores.
function Match:Publish()
	local rows = {}
	for _, peer in ipairs(SortedKeys(self.Roster)) do rows[#rows + 1] = { Peer = peer, Score = self.Roster[peer].Score } end
	self.Rows, self.Winner = rows, self.Break and self.Break.Winner or false
	Arena.RequireScript("Scoreboard"):ShowScores(0, self.Rows, self.Winner)
	if Network.IsHost() then
		for _, peer in ipairs(Network.GetPeers()) do self.Unsent[peer] = true end
	end
end

-- Sends the current table to each client that has not had it. One whose send fails (its message rate or bandwidth is
-- spent) gets it next frame; the others are not sent it again.
function Match:SendScores()
	if not Network.IsHost() then
		self.Unsent = {}
		return
	end
	local connected = {}
	for _, peer in ipairs(Network.GetPeers()) do connected[peer] = true end
	local failure
	for _, peer in ipairs(SortedKeys(self.Unsent)) do
		local sent, reason = true, nil
		if connected[peer] then sent, reason = Network.Rpc(self.Scoreboard, "ShowScores", peer, self.Rows, self.Winner) end
		if sent then self.Unsent[peer] = nil else failure = failure or reason end
	end
	if failure and not self.SendFailing then Log.Warn("Arena: could not send the scores to every client (trying again each frame): " .. tostring(failure)) end
	if not failure and self.SendFailing then Log.Info("Arena: the scores reached every client again") end
	self.SendFailing = failure ~= nil
end

function Match:UpdateStatus()
	local text
	local state = Network.GetState()
	if not Network.IsActive() then
		text = "Practice: start with --host PORT or --connect ADDRESS to play together"
		if self.LeftReason then text = "Disconnected (" .. self.LeftReason .. "). " .. text end
	elseif Network.IsHost() then
		text = "Hosting on port " .. tostring(Network.GetPort()) .. ", " .. #Network.GetPeers() .. " client(s)"
	elseif state == "Connected" then
		text = "Playing as " .. Arena.PeerName(Network.GetPeerId())
	elseif state == "Connecting" then
		text = "Connecting..."
	else
		text = "Disconnected: " .. tostring(Network.GetDisconnectReason()) .. ". Press Enter to practise"
	end
	if self.StatusText:GetComponent("UIText").Text ~= text then self.StatusText:SetComponent("UIText", { Text = text }) end
end

return Match
