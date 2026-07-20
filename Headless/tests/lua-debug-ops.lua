local frameComplete = false
local handled = false

emu.addEventCallback(function()
	frameComplete = true
end, emu.eventType.endFrame)

emu.addMemoryCallback(function(address, value)
	if frameComplete and not handled then
		handled = true
		local png = emu.takeScreenshot()
		assert(#png > 8 and png:sub(1, 8) == "\137PNG\r\n\26\n", "Lua screenshot is not a PNG")
		local state = emu.createSavestate()
		assert(type(state) == "string" and #state > 100, "Lua save state is empty")
		emu.log("exec callback captured PNG and save state")
		emu.breakExecution()
	end
end, emu.callbackType.exec, 0x8013)
