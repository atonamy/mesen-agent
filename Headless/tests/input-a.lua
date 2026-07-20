local poll = 0

emu.addEventCallback(function()
	if poll == 5 then
		emu.setInput({a=true}, 0, 0)
	end
	poll = poll + 1
end, emu.eventType.inputPolled)
