emu.addEventCallback(function()
	error({kind = "non-string-error"})
end, emu.eventType.endFrame)
