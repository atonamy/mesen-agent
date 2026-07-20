emu.addEventCallback(function()
	emu.log("scriptEnded requested stop")
	emu.stop(9)
end, emu.eventType.scriptEnded)
