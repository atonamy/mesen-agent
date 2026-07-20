local helper = require("lua-helper")
assert(helper.marker == 0x5A, "relative require returned the wrong module")
emu.log("relative require loaded sibling module")
