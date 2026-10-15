-- The marker folds callback order into serialized state, so scene.hash verifies more than final poses.
local Spawn = {}

local function record(value)
	ReplayTrace = (ReplayTrace * 17 + value) % 1000003
	Scene.FindEntityByName("ReplayTrace").Translation = Vec3(ReplayTrace, ReplayCreates, ReplayContacts)
end

function Spawn:OnCreate()
	ReplayCreates = ReplayCreates + 1
	self.Index = tonumber(self.Entity.Name:match("%d+$"))
	record(self.Index)
end

function Spawn:OnTriggerEnter(other)
	if other.Name ~= "ReplayZone" then return end
	ReplayContacts = ReplayContacts + 1
	record(10 + self.Index)
	if self.Spawned then return end
	self.Spawned = true
	local child = Scene.CreateEntity("ReplayChild" .. self.Index)
	child.Translation = Vec3(60 + self.Index, 4, 0)
	child:AddComponent("RigidBody", { Type = "Dynamic" })
	child:AddComponent("BoxCollider")
	child:AddComponent("Joint", { Type = "Point", ConnectedAnchor = child.Translation })
	child:AddComponent("Script", { Script = "Assets/Scripts/RuntimeSpawns.lua" })
end

function Spawn:OnTriggerExit(other)
	if other.Name ~= "ReplayZone" then return end
	ReplayExits = ReplayExits + 1
	record(20 + self.Index)
end

return Spawn
