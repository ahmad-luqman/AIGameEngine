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
	return 'scores=' .. Scores() .. ' over=' .. tostring(Board().Over) .. ' message=' .. Board().Message .. ' status=' .. Text('StatusText') .. ' ' .. table.concat(characters, ' ')
end
