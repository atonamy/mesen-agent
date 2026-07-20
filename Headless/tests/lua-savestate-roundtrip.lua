local frameComplete = false
local handled = false
local stateSavedEvents = 0
local stateLoadedEvents = 0

emu.addEventCallback(function()
	frameComplete = true
end, emu.eventType.endFrame)

emu.addEventCallback(function()
	stateSavedEvents = stateSavedEvents + 1
end, emu.eventType.stateSaved)

emu.addEventCallback(function()
	stateLoadedEvents = stateLoadedEvents + 1
end, emu.eventType.stateLoaded)

emu.addMemoryCallback(function(address, value)
	if frameComplete and not handled then
		handled = true
		local before = emu.read(0x16, emu.memType.nesMemory)
		local state = emu.createSavestate()
		assert(stateSavedEvents == 0, "in-memory createSavestate unexpectedly emitted stateSaved")
		emu.write(0x16, (before + 1) % 256, emu.memType.nesMemory)
		assert(emu.read(0x16, emu.memType.nesMemory) ~= before, "test mutation failed")
		emu.loadSavestate(state)
		assert(stateLoadedEvents == 0, "in-memory loadSavestate unexpectedly emitted stateLoaded")
		assert(emu.read(0x16, emu.memType.nesMemory) == before, "Lua save-state round trip did not restore RAM")
		emu.log("Lua save-state round trip succeeded without reentrant state events")
		emu.breakExecution()
	end
end, emu.callbackType.exec, 0x8013)
