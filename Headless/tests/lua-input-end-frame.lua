local frame = 0

emu.addEventCallback(function()
	frame = frame + 1
	if frame == 5 then
		emu.setInput({start=true}, 0, 0)
	elseif frame == 6 then
		emu.setInput({start=false}, 0, 0)
	end
end, emu.eventType.endFrame)
