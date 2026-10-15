-- Contact-driven spawn fixture invoked by FeatureTest.BeginRuntimeSpawnCheck.
local Contact = {}
function Contact:OnCreate()
	FeatureTest.RuntimeCreates = FeatureTest.RuntimeCreates + 1
end
function Contact:OnTriggerEnter(other)
	if other.Name ~= "RuntimeSpawnZone" then return end
	FeatureTest.RuntimeContacts = FeatureTest.RuntimeContacts + 1
	local child = Scene.CreateEntity(self.Entity.Name .. "Child")
	child.Translation = self.Entity.Translation + Vec3(0, 2, 0)
	child:AddComponent("RigidBody", { Type = "Dynamic" })
	child:AddComponent("BoxCollider")
	child:AddComponent("Joint", { Type = "Fixed", ConnectedEntity = self.Entity.ID })
end
return Contact
