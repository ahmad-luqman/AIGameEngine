-- Records contact, trigger and joint-break callbacks into FeatureTestEvents for the feature test driver.
local Probe = {}

local function Count(name)
	FeatureTestEvents = FeatureTestEvents or {}
	FeatureTestEvents[name] = (FeatureTestEvents[name] or 0) + 1
end

function Probe:OnCollisionBegin(other, contact)
	Count("CollisionBegin")
	-- The first contact per entity.
	local key = "Contact:" .. self.Entity.Name
	FeatureTestEvents[key] = FeatureTestEvents[key] or contact
end
-- The first contact of each other kind of event, whichever entity gets it (contacts of destroyed or
-- rebuilt bodies end without one).
local function Remember(name, contact)
	Count(name)
	FeatureTestEvents[name .. "Contact"] = FeatureTestEvents[name .. "Contact"] or contact
end
function Probe:OnCollisionEnd(other, contact) Remember("CollisionEnd", contact) end
function Probe:OnTriggerEnter(other, contact) Remember("TriggerEnter", contact) end
function Probe:OnTriggerExit(other, contact) Remember("TriggerExit", contact) end
-- Per entity, since several probes hold joints that break.
function Probe:OnJointBreak(other)
	Count("JointBreak:" .. self.Entity.Name)
	FeatureTestEvents["JointBrokeWith:" .. self.Entity.Name] = other and other.Name or "world"
end

return Probe
