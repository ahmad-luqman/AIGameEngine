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
	local marble = other:GetScript()
	if not marble then
		-- Its script failed (the error is reported); it can no longer remember being removed.
		if not self.WarnedUnscripted then
			self.WarnedUnscripted = true
			Log.Warn("MarbleRun: a marble without a running script entered '" .. self.Entity.Name .. "'; it may count twice")
		end
		marble = {}
	end
	if marble.Consumed then return end
	self.Entered = self.Entered + 1
	if self.Burst > 0 then self.Emitted = self.Emitted + self.Entity:EmitParticles(self.Burst) end
	if self.ChimeVolume > 0 then
		local played = Audio.PlayOneShot(Chime, self.Entity.WorldPosition, self.ChimeVolume)
		if not played and not self.ChimeFailed then
			self.ChimeFailed = true
			Log.Warn("MarbleRun: '" .. self.Entity.Name .. "' cannot play " .. Chime)
		end
	end
	if self.Event ~= "" then
		local handled = Events.Emit(self.Event, other)
		if handled == 0 then
			Log.Warn("MarbleRun: nothing handles '" .. self.Event .. "' from '" .. self.Entity.Name .. "'")
		end
	end
	if self.Consume then
		marble.Consumed = true
		other:Destroy()
	end
end

return Sensor
