-- Runs the arena's match on the peer that decides it (the host, or a game playing alone): spawns a player for every
-- peer (Player.bprefab, owned by that peer and predicted on it), scores the tags players land, respawns whoever was
-- tagged and ends the round when someone reaches WinScore. Scripts run on every peer, so on a client it only keeps the
-- HUD's status line; the scores reach clients through the Scoreboard's RPC.
--
-- This entity is not networked on purpose: a client keeps it (and its Network.On handlers) when the host's networked
-- entities replace its own.
local Match = {}

Match.Properties = {
	-- Tags that win a round.
	WinScore = 5,
	-- Seconds the winner is shown before the next round starts.
	RoundBreak = 3.0,
	-- Whether the host plays too: "Auto" (unless it is headless, a dedicated server), "Yes" or "No".
	HostPlays = "Auto",
	-- The computer plays every peer's player for one round, then each peer logs the result and quits: the
	-- SampleArenaNetwork test runs a headless host and two clients this way (see README.md).
	Autoplay = false
}

local PlayerPrefab = "Assets/Prefabs/Player.bprefab"
-- Autoplay: seconds the host waits after the round ends before everyone reports, for clients to show the others at rest
-- (they show them InterpolationDelay, 0.1 s, behind).
local ResultDelay = 1.5

-- Where players start and come back after a tag: the corners, then the middle of the near and far sides.
local Spawns = { Vec3(-8, 0, -8), Vec3(8, 0, -8), Vec3(-8, 0, 8), Vec3(8, 0, 8), Vec3(0, 0, -8.5), Vec3(0, 0, 8.5) }
-- By peer ID (the host is 0: gold); peer IDs are never reused in a session, so a fifth client to join takes gold too.
local Colors = {
	{ 0.95, 0.85, 0.25, 1 },
	{ 0.9, 0.25, 0.2, 1 },
	{ 0.2, 0.45, 0.95, 1 },
	{ 0.25, 0.8, 0.3, 1 },
	{ 0.75, 0.3, 0.85, 1 }
}

local function RequireEntity(name, component)
	local entity = Scene.FindEntityByName(name)
	if not entity then error("Match: the scene has no entity named '" .. name .. "'", 2) end
	if component and not entity:HasComponent(component) then error("Match: '" .. name .. "' has no " .. component, 2) end
	return entity
end

-- The name a peer goes by on the scoreboard (the game alone, with no session, is "You").
function Match.PeerName(peer)
	if peer ~= 0 then return "Player " .. peer end
	return Network.IsActive() and "Host" or "You"
end

-- Whether this peer decides the match: the host, or a game with no session.
local function Decides()
	return not Network.IsClient()
end

function Match:OnCreate()
	if self.HostPlays ~= "Auto" and self.HostPlays ~= "Yes" and self.HostPlays ~= "No" then
		error("Match: HostPlays must be \"Auto\", \"Yes\" or \"No\", not " .. tostring(self.HostPlays))
	end
	self.StatusText = RequireEntity("StatusText", "UIText")
	self.Scoreboard = RequireEntity("Scoreboard", "Networked")
	-- Read by the gameplay test: peer ID -> score, and peer ID -> player entity (on the deciding peer).
	self.Scores = {}
	self.Players = {}
	self.Winner = nil
	self.BreakLeft = 0
	self.Role = Network.GetRole()
	Network.On("PeerConnected", function(peer) Log.Info("Arena: " .. Match.PeerName(peer) .. " joined") end)
	Network.On("PeerDisconnected", function(peer, reason) Log.Info("Arena: " .. Match.PeerName(peer) .. " left (" .. reason .. ")") end)
	Network.On("Connected", function(peer) Log.Info("Arena: joined as " .. Match.PeerName(peer)) end)
	-- Autoplay: the host has frozen the round and everyone is at rest; report what this peer sees, then leave.
	Network.On("Result", function(sender)
		if sender == 0 and self.Autoplay then
			self:LogResult()
			Game.Quit()
		end
	end)
	self:UpdateStatus()
end

-- The peers that should have a player now.
function Match:WantedPeers()
	local wanted = {}
	if not Network.IsActive() then
		wanted[0] = true
		return wanted
	end
	if self.HostPlays == "Yes" or (self.HostPlays == "Auto" and not Game.IsHeadless()) then wanted[0] = true end
	for _, peer in ipairs(Network.GetPeers()) do wanted[peer] = true end
	return wanted
end

function Match:SpawnPlayer(peer)
	local player = Scene.Instantiate(PlayerPrefab, self:FreeSpawn(nil))
	player.Name = "Player" .. peer
	-- Before the next tick, which spawns it on the clients with these values: the owner predicts it from then on.
	player:SetComponent("Networked", { Owner = peer })
	-- The body is a child that is not networked: the spawn carries its color once, which is all it needs.
	player:FindChild("PlayerBody"):SetComponent("Material", { AlbedoColor = Colors[peer % #Colors + 1] })
	self.Players[peer] = player
	self.Scores[peer] = 0
end

-- The spawn point farthest from every other player (the first of equals, so every run agrees): where a new player
-- starts and a tagged one comes back. `except` is the player being placed.
function Match:FreeSpawn(except)
	local best, bestDistance = Spawns[1], -1
	for _, spawn in ipairs(Spawns) do
		local nearest = math.huge
		for _, player in pairs(self.Players) do
			if player ~= except and player:IsValid() then
				local position = player.WorldPosition
				nearest = math.min(nearest, (spawn - Vec3(position.x, 0, position.z)):Length())
			end
		end
		if nearest > bestDistance + 0.001 then
			best, bestDistance = spawn, nearest
		end
	end
	return best
end

-- Spawns and removes players so that every wanted peer has one: peers come and go, and a session can start or end
-- under a game already playing (the host's own player goes when it turns into a dedicated server).
function Match:SyncPlayers()
	local wanted = self:WantedPeers()
	local changed = false
	for peer, player in pairs(self.Players) do
		if not wanted[peer] or not player:IsValid() then
			if player:IsValid() then player:Destroy() end
			self.Players[peer] = nil
			self.Scores[peer] = nil
			changed = true
		end
	end
	local peers = {}
	for peer in pairs(wanted) do peers[#peers + 1] = peer end
	table.sort(peers)
	for _, peer in ipairs(peers) do
		if not self.Players[peer] then
			self:SpawnPlayer(peer)
			changed = true
		end
	end
	return changed
end

function Match:OnUpdate(dt)
	local role = Network.GetRole()
	if role ~= self.Role then
		-- A session started or ended: a new match.
		self.Role = role
		for peer in pairs(self.Scores) do self.Scores[peer] = 0 end
		self.Winner = nil
		self.Reported = false
		self:Publish()
	end
	self:UpdateStatus()
	if not Decides() then return end
	local changed = self:SyncPlayers()
	if self.Winner ~= nil then
		self.BreakLeft = self.BreakLeft - dt
		if self.Autoplay then
			self:FinishAutoplay()
		elseif self.BreakLeft <= 0 then
			self:StartRound()
			changed = true
		end
	end
	if changed then self:Publish() end
end

-- Called on the deciding peer by a player whose shot hit `target` (a player or a bot entity).
function Match:OnTag(shooter, target)
	if self.Winner ~= nil then return end
	local shooterPeer = shooter:GetComponent("Networked").Owner
	if self.Scores[shooterPeer] == nil then return end
	self.Scores[shooterPeer] = self.Scores[shooterPeer] + 1
	local script = target:GetScript()
	if script and script.IsBot then
		script:Respawn()
	else
		target.Translation = self:FreeSpawn(target)
	end
	Log.Info("Arena: " .. Match.PeerName(shooterPeer) .. " tagged " .. target.Name .. " (" .. self.Scores[shooterPeer] .. ")")
	if self.Scores[shooterPeer] >= self.WinScore then
		self.Winner = shooterPeer
		self.BreakLeft = self.RoundBreak
	end
	self:Publish()
end

function Match:StartRound()
	self.Winner = nil
	-- Everyone back to a spawn point, in peer order so every run agrees; a player not placed yet still counts where
	-- it stands, which at worst leaves two players a little closer than they could be.
	local peers = {}
	for peer in pairs(self.Players) do peers[#peers + 1] = peer end
	table.sort(peers)
	for _, peer in ipairs(peers) do
		self.Scores[peer] = 0
		self.Players[peer].Translation = self:FreeSpawn(self.Players[peer])
	end
	for _, bot in ipairs(Scene.GetEntitiesWith("Script")) do
		local script = bot:GetScript()
		if script and script.IsBot then script:Respawn() end
	end
end

-- The scoreboard rows, by peer, and the round's message; sent to every client through the Scoreboard's RPC.
function Match:Publish()
	if not Decides() then return end
	local peers = {}
	for peer in pairs(self.Scores) do peers[#peers + 1] = peer end
	table.sort(peers)
	local rows = {}
	for _, peer in ipairs(peers) do rows[#rows + 1] = { Peer = peer, Score = self.Scores[peer] } end
	local message = self.Winner ~= nil and (Match.PeerName(self.Winner) .. " wins!") or ""
	local over = self.Winner ~= nil
	self.Scoreboard:GetScript():ShowScores(0, rows, message, over)
	if Network.IsHost() then Network.Rpc(self.Scoreboard, "ShowScores", "Clients", rows, message, over) end
end

function Match:UpdateStatus()
	local text
	local state = Network.GetState()
	if not Network.IsActive() then
		text = "Practice: start with --host PORT or --connect ADDRESS to play together"
	elseif Network.IsHost() then
		text = "Hosting on port " .. tostring(Network.GetPort()) .. ", " .. #Network.GetPeers() .. " client(s)"
	elseif state == "Connected" then
		text = "Playing as " .. Match.PeerName(Network.GetPeerId())
	elseif state == "Connecting" then
		text = "Connecting..."
	else
		text = "Disconnected: " .. tostring(Network.GetDisconnectReason())
	end
	if self.StatusText:GetComponent("UIText").Text ~= text then self.StatusText:SetComponent("UIText", { Text = text }) end
end

-- Autoplay on the host: once the round is over, wait for everyone to come to rest (the clients show other entities
-- InterpolationDelay behind), log the result, tell the clients to log theirs, and quit when they have left.
function Match:FinishAutoplay()
	if not Network.IsHost() then return end
	if not self.Reported and self.BreakLeft <= self.RoundBreak - ResultDelay then
		self.Reported = true
		self:LogResult()
		Network.Send("Clients", "Result")
	end
	if self.Reported and #Network.GetPeers() == 0 then Game.Quit() end
end

-- One line the network test compares across peers: the scores and where this peer sees every player and bot.
function Match:LogResult()
	local rows = self.Scoreboard:GetScript().Rows or {}
	local scores = {}
	for _, row in ipairs(rows) do scores[#scores + 1] = row.Peer .. ":" .. row.Score end
	local places = {}
	local names = {}
	for _, entity in ipairs(Scene.GetEntitiesWith("CharacterController")) do names[#names + 1] = entity.Name end
	table.sort(names)
	for _, name in ipairs(names) do
		local position = Scene.FindEntityByName(name).WorldPosition
		places[#places + 1] = string.format("%s=%.3f,%.3f,%.3f", name, position.x, position.y, position.z)
	end
	Log.Info("Arena: result peer=" .. tostring(Network.GetPeerId()) .. " scores=" .. table.concat(scores, ",") .. " " .. table.concat(places, " "))
end

return Match
