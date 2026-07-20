assert(emu.memType.nesRam ~= nil, "nesRam compatibility alias is missing")
assert(emu.memType.nesRam == emu.memType.nesInternalRam,
	"nesRam must alias nesInternalRam")

emu.write(0xF0, 0xAB, emu.memType.nesRam)
assert(emu.read(0xF0, emu.memType.nesRam) == 0xAB,
	"nesRam write did not persist")
assert(emu.read(0xF0, emu.memType.nesInternalRam) == 0xAB,
	"nesInternalRam did not observe nesRam write")
assert(emu.read(0xF0, emu.memType.nesMemory) == 0xAB,
	"NES CPU bus did not observe internal RAM write")

emu.write16(0xF1, 0xCDEF, emu.memType.nesRam)
assert(emu.read16(0xF1, emu.memType.nesRam) == 0xCDEF,
	"nesRam 16-bit round trip failed")

emu.log("nesRam alias and NES internal RAM read/write round trips passed")
