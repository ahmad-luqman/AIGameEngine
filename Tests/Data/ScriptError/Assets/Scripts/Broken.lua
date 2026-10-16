-- Fails at runtime (it compiles): runs with --fail-on-script-errors must exit with code 7.
local Broken = {}

function Broken:OnUpdate(dt)
	error("deliberate script error")
end

return Broken
