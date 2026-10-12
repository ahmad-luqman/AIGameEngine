-- Check helpers shared by the feature-test drivers (FeatureTest.lua, SceneSwitchManager.lua).
-- Checks.New(results, label) returns functions that count into results.Checks, collect failed check names in
-- results.Failures, and log each failure with the label.
local Checks = {}

function Checks.New(results, label)
	local checks = {}

	function checks.Check(name, condition)
		results.Checks = results.Checks + 1
		if not condition then
			table.insert(results.Failures, name)
			Log.Error(label .. " FAILED: " .. name)
		end
	end

	-- The function must run without an error (which is appended to the check's name otherwise).
	function checks.Expect(name, fn)
		local ok, err = pcall(fn)
		checks.Check(name .. (ok and "" or (": " .. tostring(err))), ok)
	end

	-- The function must raise an error, containing fragment when one is given.
	function checks.ExpectError(name, fn, fragment)
		local ok, err = pcall(fn)
		checks.Check(name, (not ok) and (fragment == nil or string.find(tostring(err), fragment, 1, true) ~= nil))
	end

	return checks
end

return Checks
