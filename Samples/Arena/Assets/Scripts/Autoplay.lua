-- The computer plays one round, then every peer reports what it sees and quits: what the SampleArenaNetwork test runs
-- (see README.md). It is not in the scene: the test adds an entity named Autoplay with this script to its copy of the
-- sample, on every peer (an entity that is not networked, so each peer keeps its own), and sets Match's WinScore and a
-- RoundBreak long enough for everyone to report. Players read their input from it while it is there
-- (Player:ReadInput).
--
-- Player 1 walks to a spot on the north side (-6, -8) and tags east along it every cooldown. Player 2 walks to the east
-- end of that side (8, -8) and stands there; each tag sends it to the south-east corner (8, 8), the spawn point farthest
-- from player 1, and it walks back north along the east wall. So player 1 wins the round however the peers' timing goes.
-- Other players stand still.
local Arena = require("Assets.Scripts.Lib.Arena")

local Autoplay = {}

Autoplay.Properties = {
	-- Seconds the host waits after the win before everyone reports: clients show the others Network.InterpolationDelay
	-- (0.1 s in this project) behind the host, plus the latency, and the last of the players' moves must arrive.
	ResultDelay = 1.5
}

-- How close to its spot a player stops (a step at full speed covers 8 cm).
local Arrived = 0.05

function Autoplay:OnCreate()
	self.OverFor = 0
	self.Reported = false
	self.SendFailing = false
	-- From the host only: on the host a client's "Result" would otherwise end the game.
	Network.On("Result", function(sender)
		if sender ~= 0 then return end
		self:Report()
		self:Quit()
	end)
	-- The test waits for this line (with the WinScore it set), so a missing Autoplay, or a WinScore that did not reach
	-- Match, fails as such rather than as a missing result.
	Log.Info("Arena: autoplay to " .. tostring(Arena.RequireScript("Match").WinScore))
end

-- The move, whether to tag, and where to face for `player` this frame (Player:ReadInput's contract).
function Autoplay:InputFor(player)
	local peer = player:GetComponent("Networked").Owner
	local spot, aim
	if peer == 1 then
		spot, aim = Vec3(-6, 0, -8), Vec3(1, 0, 0)
	elseif peer == 2 then
		spot = Vec3(8, 0, -8)
	else
		return Vec3(0, 0, 0), false, nil
	end
	local offset = Arena.FlatOffset(player.WorldPosition, spot)
	local distance = offset:Length()
	if distance > Arrived then
		-- Slow down on arrival rather than overshoot.
		return offset:Normalized() * math.min(1, distance * 2), false, nil
	end
	return Vec3(0, 0, 0), aim ~= nil, aim
end

-- On the host: once someone has won and everyone had time to come to rest, report and tell the clients to; quit when
-- they have all left (they leave once they have reported).
function Autoplay:OnUpdate(dt)
	if not Network.IsHost() or not Arena.RoundOver() then return end
	self.OverFor = self.OverFor + dt
	if not self.Reported and self.OverFor >= self.ResultDelay then
		local sent, reason = Network.Send("Clients", "Result")
		if sent then
			self.Reported = true
			self:Report()
			if self.SendFailing then Log.Info("Arena: the clients were asked for their results") end
		elseif not self.SendFailing then
			Log.Warn("Arena: could not ask the clients for their results (trying again each frame): " .. tostring(reason))
		end
		self.SendFailing = not sent
	end
	if self.Reported and #Network.GetPeers() == 0 then self:Quit() end
end

-- Ends the game, saying so first: the test tells a peer that finished from one that ran out of frames by this line.
function Autoplay:Quit()
	Log.Info("Arena: quitting")
	Game.Quit()
end

-- One line the network test compares across peers: the scores and where this peer sees every character.
function Autoplay:Report()
	local scores = {}
	for _, row in ipairs(Arena.RequireScript("Scoreboard").Rows) do scores[#scores + 1] = row.Peer .. ":" .. row.Score end
	local characters = Scene.GetEntitiesWith("CharacterController")
	table.sort(characters, function(a, b) return a.Name < b.Name end)
	local places = {}
	for _, entity in ipairs(characters) do
		local position = entity.WorldPosition
		places[#places + 1] = string.format("%s=%.3f,%.3f,%.3f", entity.Name, position.x, position.y, position.z)
	end
	Log.Info("Arena: result peer=" .. tostring(Network.GetPeerId()) .. " scores=" .. table.concat(scores, ",") .. " " .. table.concat(places, " "))
end

return Autoplay
