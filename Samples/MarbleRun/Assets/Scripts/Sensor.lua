-- A trigger zone on the course (checkpoint, goal, kill floor). When a marble enters it emits Event (unless empty) and,
-- as its properties ask, bursts the zone's own particle emitter (Burst), rings a chime at the zone (a spatial one-shot,
-- ChimeVolume) and removes the marble (Consume).
local Sensor = {}

Sensor.Properties = {
	Event = "",
	Burst = 0,
	ChimeVolume = 0,
	Consume = false
}

local Chime = "Assets/Audio/Chime.wav"

function Sensor:OnCreate()
	-- Read by the gameplay test: how many marbles entered and how many particles the bursts emitted.
	self.Entered = 0
	self.Emitted = 0
end

function Sensor:OnTriggerEnter(other, contact)
	if other.Name ~= "Marble" then return end
	-- A removed marble lingers until the frame ends and may touch another zone (or this one again) before then:
	-- it counts once.
	local marble = other:GetScript() or {}
	if marble.Consumed then return end
	self.Entered = self.Entered + 1
	if self.Burst > 0 then self.Emitted = self.Emitted + self.Entity:EmitParticles(self.Burst) end
	if self.ChimeVolume > 0 and not Audio.PlayOneShot(Chime, self.Entity.WorldPosition, self.ChimeVolume) and not self.ChimeFailed then
		self.ChimeFailed = true
		Log.Warn("MarbleRun: '" .. self.Entity.Name .. "' cannot play " .. Chime)
	end
	if self.Event ~= "" and Events.Emit(self.Event, other) == 0 then
		Log.Warn("MarbleRun: nothing handles '" .. self.Event .. "' from '" .. self.Entity.Name .. "'")
	end
	if self.Consume then
		marble.Consumed = true
		other:Destroy()
	end
end

return Sensor
