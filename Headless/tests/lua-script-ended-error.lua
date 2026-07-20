emu.addEventCallback(function()
	error("intentional scriptEnded failure")
end, emu.eventType.scriptEnded)
