-- What the arena's scripts share: how peers are named, how the host reads what a client sent, and how scripts find each
-- other. `require("Assets.Scripts.Lib.Arena")` returns this table.
local Arena = {}

-- The child of a player that carries its mesh and color.
Arena.BodyName = "PlayerBody"

-- How far behind the host a client's view may be beyond its round trip: its InterpolationDelay (the project's
-- Network.InterpolationDelay, 0.1 s here: raise this with it), the interval between snapshots and some jitter. The host
-- clamps tag times to it, and gives players this long, plus their round trip, to hear that a round ended.
Arena.MaxViewLag = 0.25

-- The name a peer goes by on the scoreboard (the game alone, with no session, is "You").
function Arena.PeerName(peer)
	if peer ~= 0 then return "Player " .. peer end
	return Network.IsActive() and "Host" or "You"
end

-- A time a peer sent, or nil unless it is a finite number. What a client sends is untrusted, and a value the engine
-- refuses (an infinite time, a NaN) must be turned away here rather than raise an error in the host's script: an error
-- in an RPC stops that entity's script on the host.
function Arena.ReadTime(value)
	if type(value) ~= "number" or value ~= value or value == math.huge or value == -math.huge then return nil end
	return value
end

-- A coordinate a peer sent: a finite number within a million of zero (the arena is 20 m across), or nil.
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

-- A vector as an array, the form it arrives in from another peer (Network.Rpc converts by itself; a script calling a
-- handler directly passes this).
function Arena.ToArray(v)
	return { v.x, v.y, v.z }
end

-- From one position to another across the floor (height left out).
function Arena.FlatOffset(from, to)
	return Vec3(to.x - from.x, 0, to.z - from.z)
end

-- The script of the entity named `name`, or nil while there is none. A client replaces its own networked entities (the
-- scoreboard, the bots) with the host's when it is accepted, and spawns arrive in any order. Entity handles follow the
-- entity's ID, so a script may keep one; it must not keep another entity's script table, which the replacement ends:
-- it looks the script up when it needs it.
function Arena.FindScript(name)
	local entity = Scene.FindEntityByName(name)
	return entity and entity:GetScript()
end

-- Like FindScript, for an entity that must exist when called (Match always, as it is not networked).
function Arena.RequireScript(name)
	local script = Arena.FindScript(name)
	if not script then error("Arena: the scene has no entity named '" .. name .. "' with a running script", 2) end
	return script
end

-- Whether the round is over (someone won and the break runs): players and bots stand still. The peer that decides the
-- match knows it from Match; a client from the scoreboard the host sends it. Neither raises when the script is missing
-- or failed (its own error said why), so one broken script does not stop every player and bot.
function Arena.RoundOver()
	if Network.IsClient() then
		local board = Arena.FindScript("Scoreboard")
		return board ~= nil and board.Winner ~= false
	end
	local match = Arena.FindScript("Match")
	return match ~= nil and match.Break ~= nil
end

return Arena
