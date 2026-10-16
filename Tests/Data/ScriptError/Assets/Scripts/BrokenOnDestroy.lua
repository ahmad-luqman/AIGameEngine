-- Fails only while the game stops: --fail-on-script-errors must still count it (exit code 7).
local BrokenOnDestroy = {}

function BrokenOnDestroy:OnDestroy()
	error("deliberate OnDestroy error")
end

return BrokenOnDestroy
