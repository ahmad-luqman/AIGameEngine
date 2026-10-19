-- A trigger zone on the course that tells the game when the hero enters and leaves it: a checkpoint, the pit's kill
-- zone, the goal or a gusty stretch (Kind).
local Zone = {}

Zone.Properties = { Kind = "Checkpoint" }

function Zone:OnTriggerEnter(other)
	if other.Name == "Hero" then Events.Emit("ZoneEntered", self.Kind, self.Entity) end
end

function Zone:OnTriggerExit(other)
	if other.Name == "Hero" then Events.Emit("ZoneLeft", self.Kind, self.Entity) end
end

return Zone
