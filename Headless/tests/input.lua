local poll = 0

emu.addEventCallback(function()
	local input = emu.getInput(0, 0)
	if poll == 0 then
		assert(input.start ~= nil, "standard controller was not connected")
		emu.log("headless controller buttons available")
	end
	if poll == 5 then
		emu.setInput({start=true, a=true}, 0, 0)
	end
	poll = poll + 1
end, emu.eventType.inputPolled)
