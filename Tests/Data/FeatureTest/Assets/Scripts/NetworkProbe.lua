-- Network API checks for the feature test: the net-* steps of FeatureTest.batch.json play a small scene whose NetProbe
-- (a character the first client predicts) runs this script on the host and on an in-process client. Each peer counts
-- its own checks; NetworkProbe.Report() raises an error listing the failed ones.

NetworkProbe = { Checks = 0, Failures = {}, Log = {} }

local Checks = require("Assets.Scripts.Lib.Checks").New(NetworkProbe, "NetworkProbe")
local Check, ExpectError = Checks.Check, Checks.ExpectError

local function Logged(entry)
	for _, value in ipairs(NetworkProbe.Log) do
		if value == entry then
			return true
		end
	end
	return false
end

local Probe = {}
Probe.Rpcs = { Wave = "Owner", Cheer = "Host" }

function Probe:OnCreate()
	NetworkProbe.Entity = self.Entity
	NetworkProbe.Log = {}
	Network.On("PeerConnected", function(peer) table.insert(NetworkProbe.Log, "Joined" .. peer) end)
	Network.On("PeerDisconnected", function(peer, reason) table.insert(NetworkProbe.Log, "Left" .. peer .. reason) end)
	Network.On("Connected", function(peer) table.insert(NetworkProbe.Log, "Connected" .. peer) end)
	Network.On("Ping", function(sender, value)
		table.insert(NetworkProbe.Log, "Ping" .. sender .. ":" .. value)
		Network.Send(sender, "Pong", value + 1)
	end)
	Network.On("Pong", function(sender, value) table.insert(NetworkProbe.Log, "Pong" .. sender .. ":" .. value) end)
end

function Probe:OnUpdate(dt)
	-- The client that owns the character walks it from its own input; the host moves it from that client's inputs.
	if Network.IsClient() and Network.IsOwner(self.Entity) then
		self.Entity:Move(Input.IsKeyDown("W") and Vec3(0, 0, 3) or Vec3(0, 0, 0))
	end
end

function Probe:Wave(sender, word)
	table.insert(NetworkProbe.Log, "Wave" .. sender .. ":" .. word)
end

function Probe:Cheer(sender, word)
	table.insert(NetworkProbe.Log, "Cheer" .. sender .. ":" .. word)
end

-- Before any session: every query answers for a game alone, every send fails, and the host starts one.
function NetworkProbe.CheckOffline()
	local probe = NetworkProbe.Entity
	Check("Network.IsActive offline", Network.IsActive() == false)
	Check("Network.GetRole offline", Network.GetRole() == nil)
	Check("Network.IsHost offline", Network.IsHost() == false)
	Check("Network.IsClient offline", Network.IsClient() == false)
	Check("Network.GetState offline", Network.GetState() == nil)
	Check("Network.GetPeerId offline", Network.GetPeerId() == nil)
	Check("Network.GetPeers offline", #Network.GetPeers() == 0)
	Check("Network.GetPort offline", Network.GetPort() == nil)
	Check("Network.GetDisconnectReason offline", Network.GetDisconnectReason() == nil)
	Check("Network.GetRoundTripTime offline", Network.GetRoundTripTime() == 0)
	Check("Network.GetTime offline", Network.GetTime() >= 0)
	Check("Network.GetViewTime offline", Network.GetViewTime() == Network.GetTime())
	Check("Network.HasAuthority offline", Network.HasAuthority(probe))
	Check("Network.IsOwner offline", Network.IsOwner(probe))
	Check("Network.IsPredicted offline", not Network.IsPredicted(probe))
	local ok, err = Network.Send("Server", "Ping", 1)
	Check("Network.Send offline", ok == false and err == "no network session")
	Check("Network.SendUnreliable offline", Network.SendUnreliable("Server", "Ping", 1) == false)
	Check("Network.Rpc offline", Network.Rpc(probe, "Wave", "Server", "hi") == false)
	Check("Network.RpcUnreliable offline", Network.RpcUnreliable(probe, "Wave", "Server", "hi") == false)
	local hit = Network.Raycast(Vec3(4, 10, 4), Vec3(0, -1, 0), 50, Network.GetViewTime())
	Check("Network.Raycast offline", hit ~= nil and hit.Entity.Name == "Ground")
	ExpectError("Network.Send built-in name", function() Network.Send("Server", "Connected") end, "built-in")
	ExpectError("Network.On handler", function() Network.On("Ping", 5) end, "expected a function")
	Check("Network.On handle", Network.On("Unused", function() end) ~= nil)
	Check("Network.Host", Network.Host(0) == true)
	return NetworkProbe.Report()
end

function NetworkProbe.CheckHost()
	local probe = NetworkProbe.Entity
	Check("Network.IsHost", Network.IsHost() and Network.GetRole() == "Host" and Network.GetState() == "Connected")
	Check("Network.GetPeerId host", Network.GetPeerId() == 0)
	Check("Network.GetPeers host", #Network.GetPeers() == 1 and Network.GetPeers()[1] == 1)
	Check("Network.GetPort host", Network.GetPort() ~= nil)
	Check("Network.GetRoundTripTime host", Network.GetRoundTripTime(1) > 0)
	Check("Network PeerConnected", Logged("Joined1"))
	Check("Network.HasAuthority host", Network.HasAuthority(probe))
	Check("Network.IsOwner host", not Network.IsOwner(probe))
	Check("Network.Rpc to the owner", Network.Rpc(probe, "Cheer", "Owner", "go") == true)
	Check("Network.RpcUnreliable to the clients", Network.RpcUnreliable(probe, "Cheer", "Clients", "all") == true)
	ExpectError("Network.Send to the server from the host", function() Network.Send("Server", "Ping") end, "the host is the server")
	-- Lag compensation: the probe as the client saw it a moment ago is still under the ray.
	local position = probe.Translation
	local hit = Network.Raycast(position + Vec3(0, 5, 0), Vec3(0, -1, 0), 10, Network.GetTime() - 0.05, { Layers = { "Default" } })
	Check("Network.Raycast rewound", hit ~= nil and hit.Entity == probe)
	return NetworkProbe.Report()
end

function NetworkProbe.CheckClient()
	local probe = NetworkProbe.Entity
	Check("Network.IsClient", Network.IsClient() and Network.GetRole() == "Client" and Network.GetState() == "Connected")
	-- No Connected event here: the client replaced its own copy of this networked entity (whose handler heard it) with
	-- the host's when it was accepted.
	Check("Network.GetPeerId client", Network.GetPeerId() == 1)
	Check("Network.GetPeers client", #Network.GetPeers() == 1 and Network.GetPeers()[1] == 0)
	Check("Network.GetTime client", Network.GetTime() > 0 and Network.GetViewTime() < Network.GetTime())
	Check("Network.IsOwner client", Network.IsOwner(probe))
	Check("Network.IsPredicted client", Network.IsPredicted(probe))
	Check("Network.HasAuthority client", not Network.HasAuthority(probe))
	Check("Network.Send to the server", Network.Send("Server", "Ping", 41) == true)
	Check("Network.SendUnreliable to the server", Network.SendUnreliable("Server", "Ping", 7) == true)
	Check("Network.Rpc to the server", Network.Rpc(probe, "Wave", "Server", "hello") == true)
	Check("Network.RpcUnreliable to the server", Network.RpcUnreliable(probe, "Wave", "Server", "again") == true)
	ExpectError("Network.Send to the clients from a client", function() Network.Send("Clients", "Ping") end, "only the host")
	return NetworkProbe.Report()
end

-- After the client walked: it moved at once, and heard the host's replies and RPCs.
function NetworkProbe.CheckClientAfter()
	Check("Network predicted walk", NetworkProbe.Entity.Translation.z > 1)
	Check("Network Pong", Logged("Pong0:42"))
	Check("Network Rpc from the host", Logged("Cheer0:go") and Logged("Cheer0:all"))
	return NetworkProbe.Report()
end

-- The host moved the character from the client's inputs, and handled its messages and RPCs.
function NetworkProbe.CheckHostAfter()
	Check("Network inputs move the host's character", NetworkProbe.Entity.Translation.z > 1)
	Check("Network Ping", Logged("Ping1:41"))
	Check("Network Wave", Logged("Wave1:hello"))
	return NetworkProbe.Report()
end

function NetworkProbe.CheckLeft()
	Check("Network PeerDisconnected", Logged("Left1ClientQuit"))
	Check("Network.GetPeers after leaving", #Network.GetPeers() == 0)
	Network.Disconnect()
	return NetworkProbe.Report()
end

function NetworkProbe.Report()
	if #NetworkProbe.Failures > 0 then
		error(string.format("%d of %d network checks failed: %s", #NetworkProbe.Failures, NetworkProbe.Checks, table.concat(NetworkProbe.Failures, "; ")))
	end
	return string.format("PASS: %d network checks", NetworkProbe.Checks)
end

return Probe
