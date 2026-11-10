-- The computer plays one round, then every peer reports what it sees and quits: what the SampleArenaNetwork test runs
-- (see README.md). It is not in the scene: the test adds an entity named Autoplay with this script to its copy of the
-- sample, on every peer (an entity that is not networked, so each peer keeps its own). Players read their input from it
-- while it is there (Player:ReadInput).
--
-- Player 1 walks to a spot on the north side (-6, -8) and tags east along it every cooldown. Player 2 walks to the east
-- end of that side (8, -8) and stands there; each tag sends it to the south-east corner (8, 8), the spawn point farthest
-- from player 1, and it walks back north along the east wall. So player 1 wins the round however the peers' timing goes.
-- Other players stand still.
local Arena = require("Assets.Scripts.Lib.Arena")

local Autoplay = {}

Autoplay.Properties = {
	-- Tags that win the round.
	WinScore = 3,
	-- Seconds the host waits after the win before everyone reports: clients show the others Network.InterpolationDelay
	-- (0.1 s in this project) behind the host, plus the latency, and the last of the players' moves must arrive.
	ResultDelay = 1.5
}

-- How close to its spot a player stops (a step at full speed covers 8 cm).
local Arrived = 0.05

function Autoplay:OnCreate()
	local match = Arena.RequireScript("Match")
	match.WinScore = self.WinScore
	-- One round: the result is taken during its break, which must last until everyone has reported.
	match.RoundBreak = math.huge
	self.OverFor = 0
	self.Reported = false
	Network.On("Result", function(sender)
		if sender == 0 then
			self:Report()
			Game.Quit()
		end
	end)
	-- The test waits for this line, so a renamed property or script fails at once rather than at the frame limit.
	Log.Info("Arena: autoplay to " .. self.WinScore)
end

-- The move, whether to tag, and where to face for `player` this frame.
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
	if not Network.IsHost() or Arena.RequireScript("Match").Winner == nil then return end
	self.OverFor = self.OverFor + dt
	if not self.Reported and self.OverFor >= self.ResultDelay then
		local sent, reason = Network.Send("Clients", "Result")
		if sent then
			self.Reported = true
			self:Report()
		elseif not self.SendFailed then
			self.SendFailed = true
			Log.Warn("Arena: could not ask the clients for their results (trying again each frame): " .. tostring(reason))
		end
	end
	if self.Reported and #Network.GetPeers() == 0 then Game.Quit() end
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
