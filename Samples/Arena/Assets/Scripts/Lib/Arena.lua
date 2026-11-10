-- What the arena's scripts share: how peers are named, how the host reads what a client sent, and how scripts find each
-- other. `require("Assets.Scripts.Lib.Arena")` returns this table.
local Arena = {}

-- The name a peer goes by on the scoreboard (the game alone, with no session, is "You").
function Arena.PeerName(peer)
	if peer ~= 0 then return "Player " .. peer end
	return Network.IsActive() and "Host" or "You"
end

-- A number a peer sent, or nil unless it is finite and within a million of zero. What a client sends is untrusted, and
-- a value the engine refuses (an infinite time, a NaN) must be turned away here rather than raise an error in the
-- host's script: an error in an RPC stops that entity's script on the host.
function Arena.ReadNumber(value)
	if type(value) ~= "number" or value ~= value or math.abs(value) > 1e6 then return nil end
	return value
end

-- A vector a peer sent (vectors arrive as arrays of three numbers), or nil when it is anything else. Each component is
-- read by index: a loop over them with ipairs would stop at the first missing one and let {} or {1} through.
function Arena.ReadVec3(value)
	if type(value) ~= "table" then return nil end
	local x, y, z = Arena.ReadNumber(value[1]), Arena.ReadNumber(value[2]), Arena.ReadNumber(value[3])
	if not x or not y or not z then return nil end
	return Vec3(x, y, z)
end

-- A vector as an array, the form it arrives in from another peer.
function Arena.ToArray(v)
	return { v.x, v.y, v.z }
end

-- From one position to another across the floor (height left out).
function Arena.FlatOffset(from, to)
	return Vec3(to.x - from.x, 0, to.z - from.z)
end

-- The keys of a table, sorted, so every run visits peers in the same order.
function Arena.SortedKeys(t)
	local keys = {}
	for key in pairs(t) do keys[#keys + 1] = key end
	table.sort(keys)
	return keys
end

-- The script of the entity named `name`, or nil while there is none. A client replaces its own networked entities (the
-- scoreboard, the bots) with the host's when it is accepted, and spawns arrive in any order, so a script must not keep
-- another networked entity's script from its OnCreate: it looks it up when it needs it.
function Arena.FindScript(name)
	local entity = Scene.FindEntityByName(name)
	return entity and entity:GetScript()
end

-- Like FindScript, for an entity the scene must have (Match is not networked, so every peer has it from the start).
function Arena.RequireScript(name)
	local script = Arena.FindScript(name)
	if not script then error("Arena: the scene has no entity named '" .. name .. "' with a running script", 2) end
	return script
end

-- Whether the round is over (someone won and the break runs): players and bots stand still. The peer that decides the
-- match knows it from Match; a client from the scoreboard the host sends it.
function Arena.RoundOver()
	if Network.IsClient() then
		local board = Arena.FindScript("Scoreboard")
		return board ~= nil and board.Over
	end
	return Arena.RequireScript("Match").Winner ~= nil
end

return Arena
