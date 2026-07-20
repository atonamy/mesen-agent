local frame = 0

emu.addEventCallback(function()
	frame = frame + 1
	if frame == 2 then
		emu.write(0x15, 0xA5, emu.memType.nesMemory)
		emu.log("first script wrote marker")
	end
end, emu.eventType.endFrame)
