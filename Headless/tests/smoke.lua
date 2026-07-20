local frameCount = 0

local function onEndFrame()
	frameCount = frameCount + 1
	if frameCount == 5 then
		emu.write(0x10, 0x42, emu.memType.nesMemory)
		emu.log("headless Lua callback reached frame " .. frameCount)
	end
end

emu.addEventCallback(onEndFrame, emu.eventType.endFrame)
