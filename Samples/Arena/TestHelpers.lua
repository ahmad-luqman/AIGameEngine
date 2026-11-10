-- Helpers for test_game.json's lua.exec steps: `require('TestHelpers')` defines them as globals in the Lua state of the
-- peer that runs it (the playing scene, or an in-process client through net.command; each has its own state). It sits
-- outside Assets/, so exports leave it out.
function Match() return Scene.FindEntityByName('Match'):GetScript() end
function Board() return Scene.FindEntityByName('Scoreboard'):GetScript() end
function P(peer) return Scene.FindEntityByName('Player' .. peer) end
function E(name) return Scene.FindEntityByName(name) end
function Near(a, b, tolerance) return math.abs(a - b) <= tolerance end
function NearXZ(entity, x, z, tolerance)
	local p = entity.WorldPosition
	return Near(p.x, x, tolerance) and Near(p.z, z, tolerance)
end
function At(entity)
	local p = entity.WorldPosition
	return string.format('%.3f,%.3f,%.3f', p.x, p.y, p.z)
end
function Scores()
	local parts = {}
	for _, row in ipairs(Board().Rows) do parts[#parts + 1] = row.Peer .. ':' .. row.Score end
	return table.concat(parts, ',')
end
function Text(name) return E(name):GetComponent('UIText').Text end
-- Everything a failed step needs to say what it saw.
function State()
	local characters = {}
	for _, entity in ipairs(Scene.GetEntitiesWith('CharacterController')) do characters[#characters + 1] = entity.Name .. '@' .. At(entity) end
	table.sort(characters)
	return 'scores=' .. Scores() .. ' winner=' .. tostring(Board().Winner) .. ' message=' .. Board().Message .. ' status=' .. Text('StatusText') .. ' ' .. table.concat(characters, ' ')
end
-- Where a player that walked east from (-8, -8) to the middle of the north side and turned south stands.
function AtNorthMiddle(entity) return NearXZ(entity, 0, -7.917, 0.01) end
-- Where a new round puts the host and players 1 and 2, with each bot just starting its lane.
function NewRoundPlaces()
	return NearXZ(P(0), -8, 8, 0.05) and NearXZ(P(1), 0, -8.5, 0.05) and NearXZ(P(2), 8, 8, 0.05)
		and E('BotA').WorldPosition.x < -2 and E('BotB').WorldPosition.x > 2
end
-- Records where player 1's tag beams end on this peer, in Beams, until StopWatchingBeams.
function WatchBeams()
	Beams = {}
	local script = P(1):GetScript()
	local show = script.ShowTag
	script.ShowTag = function(self, sender, from, to)
		Beams[#Beams + 1] = to or false
		return show(self, sender, from, to)
	end
end
function StopWatchingBeams() P(1):GetScript().ShowTag = nil end
-- Counts this peer's warnings that contain `text`, from now on (Warned(text)).
function WatchWarnings()
	Warnings = {}
	RealWarn = RealWarn or Log.Warn
	Log.Warn = function(...)
		Warnings[#Warnings + 1] = table.concat({ ... }, ' ')
		return RealWarn(...)
	end
end
function Warned(text)
	local count = 0
	for _, warning in ipairs(Warnings) do
		if warning:find(text, 1, true) then count = count + 1 end
	end
	return count
end
