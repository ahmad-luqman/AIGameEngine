-- Shows the scores on every peer. It is networked (Server authority) so that the host can call ShowScores on the
-- clients' copies: the host is the only peer that may (Rpcs), and it sends the whole table each time something
-- changes, so a client that joins late, or missed nothing but an old table, still shows the current one.
local Scoreboard = {}

Scoreboard.Rpcs = { ShowScores = "Host" }

function Scoreboard:OnCreate()
	-- Read by players, bots and the gameplay test: { { Peer = id, Score = n }, ... } by peer, the round's message,
	-- and whether the round is over.
	self.Rows = {}
	self.Message = ""
	self.Over = false
	self.ScoreText = Scene.FindEntityByName("ScoreText")
	self.MessageText = Scene.FindEntityByName("MessageText")
	if not self.ScoreText or not self.MessageText then error("Scoreboard: the scene needs ScoreText and MessageText") end
end

function Scoreboard:ShowScores(sender, rows, message, over)
	if type(rows) ~= "table" or type(message) ~= "string" or type(over) ~= "boolean" then
		Log.Warn("Arena: the scoreboard received something that is not a score table")
		return
	end
	local valid = {}
	for _, row in ipairs(rows) do
		if type(row) == "table" and math.type(row.Peer) == "integer" and math.type(row.Score) == "integer" then
			valid[#valid + 1] = { Peer = row.Peer, Score = row.Score }
		end
	end
	self.Rows, self.Message, self.Over = valid, message, over
	local match = Scene.FindEntityByName("Match")
	local lines = {}
	for _, row in ipairs(valid) do lines[#lines + 1] = match:GetScript().PeerName(row.Peer) .. "  " .. row.Score end
	self.ScoreText:SetComponent("UIText", { Text = table.concat(lines, "\n") })
	self.MessageText:SetComponent("UIText", { Text = message })
end

return Scoreboard
