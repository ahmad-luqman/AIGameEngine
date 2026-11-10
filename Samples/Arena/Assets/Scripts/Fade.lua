-- A tag's beam: shown for a moment on the peer that drew it, then removed. It is local to that peer (not networked).
local Fade = {}

Fade.Properties = { Seconds = 0.15 }

function Fade:OnCreate()
	self.Left = self.Seconds
end

function Fade:OnUpdate(dt)
	self.Left = self.Left - dt
	if self.Left <= 0 then self.Entity:Destroy() end
end

return Fade
