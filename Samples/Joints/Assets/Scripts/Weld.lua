-- Counts broken welds in the tower for the HUD. Both blocks of a broken weld get OnJointBreak, so each
-- pair is counted once.
local Weld = {}

function Weld:OnJointBreak(other)
	JointsDemoBroken = JointsDemoBroken or {}
	local a, b = self.Entity.ID, other and other.ID or 0
	local key = math.min(a, b) .. ":" .. math.max(a, b)
	if not JointsDemoBroken[key] then
		JointsDemoBroken[key] = true
		JointsDemoBreaks = (JointsDemoBreaks or 0) + 1
	end
end

return Weld
