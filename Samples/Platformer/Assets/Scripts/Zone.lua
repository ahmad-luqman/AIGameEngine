-- A trigger zone on the course that tells the game when the hero enters and leaves it: a checkpoint, the pit's kill
-- zone, the goal or a gusty stretch (Kind).
local Zone = {}

Zone.Properties = { Kind = "Checkpoint" }

local Kinds = { Checkpoint = true, Kill = true, Goal = true, Wind = true }

function Zone:OnCreate()
	if not Kinds[self.Kind] then
		error("Zone '" .. self.Entity.Name .. "': unknown Kind '" .. tostring(self.Kind) .. "' (Checkpoint, Kill, Goal or Wind)")
	end
end

function Zone:Tell(event)
	if Events.Emit(event, self.Kind, self.Entity) == 0 and not self.Unheard then
		self.Unheard = true
		Log.Warn("Platformer: nothing handles " .. event .. " from '" .. self.Entity.Name .. "'")
	end
end

function Zone:OnTriggerEnter(other)
	if other.Name == "Hero" then self:Tell("ZoneEntered") end
end

function Zone:OnTriggerExit(other)
	if other.Name == "Hero" then self:Tell("ZoneLeft") end
end

return Zone
