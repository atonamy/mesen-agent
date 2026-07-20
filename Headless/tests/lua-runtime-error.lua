local calls = 0

emu.addEventCallback(function()
	calls = calls + 1
	error("intentional runtime callback failure " .. calls)
end, emu.eventType.endFrame)
