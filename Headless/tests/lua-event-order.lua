local sequence = {}
local endFrames = 0

local function record(name)
	if #sequence < 12 then
		table.insert(sequence, name)
	end
end

emu.addEventCallback(function()
	record("start")
end, emu.eventType.startFrame)

emu.addEventCallback(function()
	record("input")
end, emu.eventType.inputPolled)

emu.addEventCallback(function()
	record("end")
	endFrames = endFrames + 1
	if endFrames == 3 then
		emu.log("event-order=" .. table.concat(sequence, ","))
	end
end, emu.eventType.endFrame)
