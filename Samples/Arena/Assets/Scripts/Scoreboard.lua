-- Shows the scores on every peer. It is networked (Server authority) so that the host can call ShowScores on the
-- clients' copies: the host is the only peer that may (Rpcs). The host sends the whole table each time something
-- changes and to every client that joins, so one message brings a client up to date.
local Arena = require("Assets.Scripts.Lib.Arena")

local Scoreboard = {}

Scoreboard.Rpcs = { ShowScores = "Host" }

function Scoreboard:OnCreate()
	-- Read by players on clients (Arena.RoundOver), Autoplay and the gameplay test: { { Peer = id, Score = n }, ... } by
	-- peer, and the peer that won the round (false while it is played), with the message shown for it.
	self.Rows = {}
	self.Winner = false
	self.Message = ""
	self.Refused = false
	self.ScoreText = Scene.FindEntityByName("ScoreText")
	self.MessageText = Scene.FindEntityByName("MessageText")
	if not self.ScoreText or not self.MessageText then error("Scoreboard: the scene needs ScoreText and MessageText") end
end

-- A peer ID: 0 for the host, clients from 1.
local function IsPeer(value)
	return math.type(value) == "integer" and value >= 0
end

-- The rows the host sent, read by index (a table with other keys or holes is not a list), or nil.
local function ReadRows(rows)
	if type(rows) ~= "table" then return nil end
	local count = 0
	for _ in pairs(rows) do count = count + 1 end
	if count ~= #rows then return nil end
	local valid, seen = {}, {}
	for i = 1, #rows do
		local row = rows[i]
		if type(row) ~= "table" or not IsPeer(row.Peer) or math.type(row.Score) ~= "integer" or row.Score < 0 or seen[row.Peer] then return nil end
		seen[row.Peer] = true
		valid[i] = { Peer = row.Peer, Score = row.Score }
	end
	return valid
end

-- What the host sent: the rows by peer, and the peer that won the round or false while it is played. (The winner need
-- not have a row: it may have left during the break.)
function Scoreboard:ShowScores(sender, rawRows, winner)
	local rows = ReadRows(rawRows)
	if not rows or not (winner == false or IsPeer(winner)) then
		return self:Refuse("a score table that is not a list of distinct rows and a winner")
	end
	self.Rows, self.Winner = rows, winner
	self.Message = winner ~= false and (Arena.PeerName(winner) .. " wins!") or ""
	local lines = {}
	for _, row in ipairs(rows) do lines[#lines + 1] = Arena.PeerName(row.Peer) .. "  " .. row.Score end
	self.ScoreText:SetComponent("UIText", { Text = table.concat(lines, "\n") })
	self.MessageText:SetComponent("UIText", { Text = self.Message })
end

-- A table the host should never send: kept out, and logged once.
function Scoreboard:Refuse(what)
	if not self.Refused then Log.Warn("Arena: the scoreboard received " .. what .. "; it keeps the last good one (not logged again)") end
	self.Refused = true
end

return Scoreboard
