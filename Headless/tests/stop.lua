local function stopFromLua()
	emu.log("stopping from Lua with code 7")
	emu.stop(7)
end

emu.addEventCallback(stopFromLua, emu.eventType.endFrame)
