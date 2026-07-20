local frame = 0

emu.addEventCallback(function()
	frame = frame + 1
	if frame == 2 then
		assert(emu.read(0x15, emu.memType.nesMemory) == 0xA5, "script callback order changed")
		emu.log("second script observed marker")
	end
end, emu.eventType.endFrame)
