-- Runs the arena's match on the peer that decides it (the host, or a game playing alone): spawns a player for every
-- peer (Player.bprefab, owned by that peer and predicted on it), scores the tags players land, respawns whoever was
-- tagged and ends the round when someone reaches WinScore. Scripts run on every peer: on a client it keeps the HUD's
-- status line and, when the host goes away, ends the session and goes back to practice. Scores reach clients through
-- the Scoreboard's RPC.
--
-- This entity is not networked on purpose: a client keeps it (and its Network.On handlers) when the host's networked
-- entities replace its own.
local Arena = require("Assets.Scripts.Lib.Arena")

local Match = {}

Match.Properties = {
	-- Tags that win a round.
	WinScore = 5,
	-- Seconds the winner is shown before the next round starts.
	RoundBreak = 3.0,
	-- Whether the host plays too: "Auto" (unless it is headless: a dedicated server), "Yes" or "No".
	HostPlays = "Auto"
}

local PlayerPrefab = "Assets/Prefabs/Player.bprefab"
local BodyName = "PlayerBody"

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

local function RequireEntity(name, component)
	local entity = Scene.FindEntityByName(name)
	if not entity then error("Match: the scene has no entity named '" .. name .. "'", 2) end
	if component and not entity:HasComponent(component) then error("Match: '" .. name .. "' has no " .. component, 2) end
	return entity
end

-- Whether this peer decides the match: the host, or a game with no session.
local function Decides()
	return not Network.IsClient()
end

function Match:OnCreate()
	self:HostPlaysNow()
	self.StatusText = RequireEntity("StatusText", "UIText")
	self.Scoreboard = RequireEntity("Scoreboard", "Networked")
	-- On the deciding peer, by peer ID: { Entity = the player, Score = its tags this round, Held = where it waits out the
	-- break }. Clients show the Scoreboard's copy of the scores instead.
	self.Roster = {}
	-- The peer that won the round, while its break lasts (nil during play), and the seconds left of the break.
	self.Winner = nil
	self.BreakLeft = 0
	-- The scores changed and not every client has them yet (a send can fail: the message rate or bandwidth is spent).
	self.PublishPending = true
	self.PublishFailing = false
	-- Why this peer's last session as a client ended, shown while it practises again.
	self.LeftReason = nil
	self.Role = Network.GetRole()
	Network.On("PeerConnected", function(peer)
		Log.Info("Arena: " .. Arena.PeerName(peer) .. " joined")
		-- RPCs are not replayed to clients that join later: the newcomer gets the whole table now. (Here a join also adds a
		-- row, which publishes anyway; this keeps it right for state a join does not change.)
		self.PublishPending = true
	end)
	Network.On("PeerDisconnected", function(peer, reason) Log.Info("Arena: " .. Arena.PeerName(peer) .. " left (" .. reason .. ")") end)
	Network.On("Connected", function(peer) Log.Info("Arena: joined as " .. Arena.PeerName(peer)) end)
	-- The host went away (or refused this client): end the session and practise again. What the host spawned stays as it
	-- was; SyncPlayers removes the players it left behind.
	Network.On("Disconnected", function(reason)
		self.LeftReason = reason
		Network.Disconnect()
	end)
	self:UpdateStatus()
end

-- Whether the host has a player of its own now (HostPlays is checked every time, as scripts may change it in play).
function Match:HostPlaysNow()
	local mode = HostPlaysModes[self.HostPlays]
	if not mode then error("Match: HostPlays must be \"Auto\", \"Yes\" or \"No\", not " .. tostring(self.HostPlays)) end
	return mode()
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
	local body = player:FindChild(BodyName)
	if not body then
		player:Destroy()
		error("Match: " .. PlayerPrefab .. " has no child named " .. BodyName)
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
			if entry.Entity ~= except and entry.Entity:IsValid() then
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

-- Spawns and removes players so that every wanted peer has one: peers come and go, and a session can start or end
-- under a game already playing (the host's own player goes when it turns into a dedicated server). Players this peer
-- does not keep (a host's, left behind when a client's session ended) are removed.
function Match:SyncPlayers()
	local wanted = self:WantedPeers()
	local changed = false
	for peer, entry in pairs(self.Roster) do
		if not wanted[peer] or not entry.Entity:IsValid() then
			if entry.Entity:IsValid() then entry.Entity:Destroy() end
			self.Roster[peer] = nil
			changed = true
		end
	end
	for _, entity in ipairs(Scene.GetEntitiesWith("Script")) do
		local script = entity:GetScript()
		if script and script.IsPlayer and not self:PeerOf(entity) then entity:Destroy() end
	end
	for _, peer in ipairs(Arena.SortedKeys(wanted)) do
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
		for _, entry in pairs(self.Roster) do entry.Score = 0 end
		self.Winner = nil
		self.PublishPending = true
		if role ~= nil then self.LeftReason = nil end
	end
	self:UpdateStatus()
	if not Decides() then return end
	if self:SyncPlayers() then self.PublishPending = true end
	if self.Winner ~= nil then
		self.BreakLeft = self.BreakLeft - dt
		if self.BreakLeft <= 0 then
			self:StartRound()
		else
			self:HoldPlayers()
		end
	end
	if self.PublishPending then self:Publish() end
end

-- Called on the deciding peer by a player whose tag hit `target`, a player or a bot entity.
function Match:OnTag(shooter, target)
	if self.Winner ~= nil then return end
	local shooterPeer = self:PeerOf(shooter)
	local targetPeer = self:PeerOf(target)
	local bot = target:GetScript()
	if not shooterPeer or not (targetPeer or (bot and bot.IsBot)) then return end
	local entry = self.Roster[shooterPeer]
	entry.Score = entry.Score + 1
	if targetPeer then
		target.Translation = self:FreeSpawn(target)
	else
		bot:Respawn()
	end
	Log.Info("Arena: " .. Arena.PeerName(shooterPeer) .. " tagged " .. target.Name .. " (" .. entry.Score .. ")")
	if entry.Score >= self.WinScore then self:EndRound(shooterPeer) end
	-- At once, so the scoreboard shows the tag the frame it lands.
	self:Publish()
end

-- Someone won: show it for RoundBreak seconds, with everyone held where they stand.
function Match:EndRound(winner)
	self.Winner = winner
	self.BreakLeft = self.RoundBreak
	for _, entry in pairs(self.Roster) do entry.Held = entry.Entity.Translation end
end

-- During the break players stand still. Their scripts stop moving them, but the host moves a predicted character from
-- whatever its owner sends, so a modified client could walk on: the host puts it back.
function Match:HoldPlayers()
	for _, entry in pairs(self.Roster) do
		local held = entry.Held
		if held and entry.Entity:IsValid() and Arena.FlatOffset(entry.Entity.Translation, held):Length() > 0.01 then
			entry.Entity.Translation = held
		end
	end
end

function Match:StartRound()
	self.Winner = nil
	-- Everyone back to a spawn point, in peer order so every run agrees; a player not placed yet still counts where
	-- it stands, which at worst leaves two players a little closer than they could be.
	for _, peer in ipairs(Arena.SortedKeys(self.Roster)) do
		local entry = self.Roster[peer]
		entry.Score = 0
		entry.Held = nil
		entry.Entity.Translation = self:FreeSpawn(entry.Entity)
	end
	for _, entity in ipairs(Scene.GetEntitiesWith("Script")) do
		local script = entity:GetScript()
		if script and script.IsBot then script:Respawn() end
	end
	self.PublishPending = true
end

-- Sends the scoreboard rows (by peer) and the winner (false during play) to this peer's scoreboard and every client's,
-- whole, so one message brings a client up to date. A send that fails is tried again next frame.
function Match:Publish()
	local rows = {}
	for _, peer in ipairs(Arena.SortedKeys(self.Roster)) do rows[#rows + 1] = { Peer = peer, Score = self.Roster[peer].Score } end
	local winner = self.Winner ~= nil and self.Winner or false
	self.Scoreboard:GetScript():ShowScores(0, rows, winner)
	self.PublishPending = false
	if Network.IsHost() then
		local sent, reason = Network.Rpc(self.Scoreboard, "ShowScores", "Clients", rows, winner)
		if not sent then
			if not self.PublishFailing then Log.Warn("Arena: could not send the scores to every client (trying again each frame): " .. tostring(reason)) end
			self.PublishFailing = true
			self.PublishPending = true
			return
		end
	end
	self.PublishFailing = false
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
		text = "Disconnected: " .. tostring(Network.GetDisconnectReason())
	end
	if self.StatusText:GetComponent("UIText").Text ~= text then self.StatusText:SetComponent("UIText", { Text = text }) end
end

return Match
