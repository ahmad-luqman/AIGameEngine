-- Records contact and trigger callbacks into FeatureTestEvents for the feature test driver.
local Probe = {}

local function Count(name)
	FeatureTestEvents = FeatureTestEvents or {}
	FeatureTestEvents[name] = (FeatureTestEvents[name] or 0) + 1
end

function Probe:OnCollisionBegin(other) Count("CollisionBegin") end
function Probe:OnCollisionEnd(other) Count("CollisionEnd") end
function Probe:OnTriggerEnter(other) Count("TriggerEnter") end
function Probe:OnTriggerExit(other) Count("TriggerExit") end

return Probe
