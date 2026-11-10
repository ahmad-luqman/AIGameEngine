-- Shows the scores on every peer. It is networked (Server authority) so that the host can call ShowScores on the
-- clients' copies: the host is the only peer that may (Rpcs). The host sends the whole table each time something
-- changes and to every client that joins, so one message brings a client up to date.
local Arena = require("Assets.Scripts.Lib.Arena")

local Scoreboard = {}

Scoreboard.Rpcs = { ShowScores = "Host" }

function Scoreboard:OnCreate()
	-- Read by players, bots and the gameplay test: { { Peer = id, Score = n }, ... } by peer, the round's message, and
	-- whether the round is over.
	self.Rows = {}
	self.Message = ""
	self.Over = false
	self.ScoreText = Scene.FindEntityByName("ScoreText")
	self.MessageText = Scene.FindEntityByName("MessageText")
	if not self.ScoreText or not self.MessageText then error("Scoreboard: the scene needs ScoreText and MessageText") end
end

-- What the host sent: the rows by peer, and the peer that won the round (false while it is played).
function Scoreboard:ShowScores(sender, rows, winner)
	if type(rows) ~= "table" or not (winner == false or math.type(winner) == "integer") then
		return self:Refuse("something that is not a score table")
	end
	local valid, seen = {}, {}
	for _, row in ipairs(rows) do
		if type(row) ~= "table" or math.type(row.Peer) ~= "integer" or math.type(row.Score) ~= "integer" or seen[row.Peer] then
			return self:Refuse("a score table with a malformed or repeated row")
		end
		seen[row.Peer] = true
		valid[#valid + 1] = { Peer = row.Peer, Score = row.Score }
	end
	self.Rows = valid
	self.Over = winner ~= false
	self.Message = self.Over and (Arena.PeerName(winner) .. " wins!") or ""
	local lines = {}
	for _, row in ipairs(valid) do lines[#lines + 1] = Arena.PeerName(row.Peer) .. "  " .. row.Score end
	self.ScoreText:SetComponent("UIText", { Text = table.concat(lines, "\n") })
	self.MessageText:SetComponent("UIText", { Text = self.Message })
end

-- A table the host should never send: kept out, and logged once.
function Scoreboard:Refuse(what)
	if not self.Refused then Log.Warn("Arena: the scoreboard received " .. what .. "; it keeps the last good one (not logged again)") end
	self.Refused = true
end

return Scoreboard
