-- A trigger zone on the course (checkpoint, goal, kill floor). When a marble enters it emits Event, bursts the
-- zone's own particle emitter, rings a chime at the zone (a spatial one-shot) and, with Consume, removes the marble.
local Sensor = {}

Sensor.Properties = {
	Event = "CheckpointPassed",
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
	self.Entered = self.Entered + 1
	if self.Burst > 0 then self.Emitted = self.Emitted + self.Entity:EmitParticles(self.Burst) end
	if self.ChimeVolume > 0 then Audio.PlayOneShot(Chime, self.Entity.WorldPosition, self.ChimeVolume) end
	if self.Consume then other:Destroy() end
	Events.Emit(self.Event, other)
end

return Sensor
