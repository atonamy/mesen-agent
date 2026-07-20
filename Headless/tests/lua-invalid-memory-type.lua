-- A misspelled/unknown enum entry evaluates to nil.  The memory API must
-- reject it instead of silently treating it as MemoryType 0 (SnesMemory).
emu.read(0x00, emu.memType.thisMemoryTypeDoesNotExist)
