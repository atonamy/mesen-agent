# Mesen Agent CLI

`mesen-agent` is a native, terminal-only front-end for Mesen's existing
emulation core. It never creates a window and initializes no display, audio
output, keyboard, mouse, or controller backend. Screenshots and recordings are
generated inside the emulation pipeline, so they work on CI workers and other
machines without a desktop session.

The executable emits one JSON object per line on stdout. Diagnostics and usage
errors go to stderr, and requested binary artifacts go to explicit paths. This
makes it safe to drive from coding agents, shell scripts, and test harnesses.

## Build

Linux and macOS need a C++17 compiler. The agent build excludes the desktop SDL,
display, host-audio, and physical-input backends, so it needs neither SDL2 nor
the .NET SDK.

```sh
make agent -j4
```

The result is:

- Linux x64: `bin/linux-x64/Release/mesen-agent`
- Linux ARM64: `bin/linux-arm64/Release/mesen-agent`
- macOS Intel: `bin/osx-x64/Release/mesen-agent`
- macOS Apple Silicon: `bin/osx-arm64/Release/mesen-agent`
- Windows x64: build `Headless/Headless.vcxproj` or the `Headless` solution
  project; `mesen-agent.exe` is placed beside `MesenCore.dll`.

Run `mesen-agent --help` for the complete option list.

## Common workflows

Run exactly 600 frames, then capture all primary artifacts:

```sh
mesen-agent game.nes --frames 600 \
  --screenshot artifacts/final.png \
  --video artifacts/gameplay.avi \
  --audio artifacts/audio.wav \
  --save-state artifacts/final.mss \
  --state-json artifacts/final-state.json \
  --dump-memory NesInternalRam=artifacts/ram.bin
```

The `.avi` recorder supports Mesen's CSCD (default) and ZMBV codecs. A `.gif`
extension selects GIF automatically. WAV and AVI contain emulated audio even
though no host audio device is opened.

Inspect and modify a running NES entirely from arguments:

```sh
mesen-agent game.nes --frames 120 \
  --write-memory NesMemory:0x0010:01020304 \
  --read-memory NesMemory:0x0000:64 \
  --eval 'pc' --eval '[$10]' \
  --pc --disassemble 0x8000:32 --callstack \
  --trace artifacts/trace.log --save-cdl NesPrgRom=artifacts/game.cdl
```

Stop on an execute/read/write breakpoint, then execute one instruction:

```sh
mesen-agent game.nes --frames 10000 \
  --breakpoint 'NesMemory:0xC010:x' \
  --step 1 --pc --read-memory NesMemory:0x20:16
```

Breakpoint syntax is `TYPE:START[-END]:rwx[:CONDITION]`. Conditions use the
same expression language as Mesen's graphical debugger. `--cpu` selects the
CPU used for breakpoints, expressions, stepping, disassembly, program-counter
operations, and call stacks.

Use `--list-memory` with a ROM to discover every memory space available for
that game. Memory names are case-insensitive and match Mesen's `MemoryType`
enum. The alias `cpu` means the active CPU's relative address space.

## Lua during emulation

`--lua FILE` loads an existing Mesen debugger script before emulation resumes.
It is repeatable, and scripts keep the complete established `emu` API: memory
and event callbacks, reads/writes, input overrides, drawing, screenshots,
save-state blobs, state tables, rewind, stepping, and `emu.stop(code)`.

```sh
mesen-agent game.sfc --frames 3600 \
  --lua assertions.lua --lua input-sequence.lua \
  --lua-timeout 5
```

Each script's log is emitted as a final `lua_log` JSON record with an
`error_count`. If a script calls `emu.stop(code)`, emulation ends immediately
and the process returns that code. Syntax errors, callback errors/assertions,
and watchdog timeouts are fail-fast: they emit `lua_error`, preserve the paused
debugger state for inspection, set `summary.reason` to `lua_error`, and exit 5.
Lua `io`/`os` and sockets remain sandboxed unless the invocation opts in with
`--lua-allow-io` or `--lua-allow-network`.

`--lua-timeout` may not exceed the overall `--timeout`. Trusted scripts using
blocking file or socket calls should also set their own operation timeouts,
because Lua's instruction watchdog can only interrupt Lua execution.

Prefer `emu.setInput` from an `emu.eventType.inputPolled` callback when input
must line up exactly with controller sampling. Explicit button values written
from `startFrame`, `endFrame`, or memory callbacks are also supported: they are
queued until the next controller poll, so the core cannot erase them before the
game reads them.

For simple controller sequences, Lua is not required:

```sh
mesen-agent game.nes --frames 300 \
  --input '0:start@0-2' \
  --input '0:right,a@30-120'
```

Input frame numbers are relative to the start of this invocation. Overlapping
schedules for a port are combined.

## Determinism and isolation

Maximum-speed mode is enabled by default. `--frames`, rather than wall-clock
time, determines the run boundary. Use `--no-max-speed` only when real-time
pacing is specifically useful.

The default Mesen data directory is the platform temporary directory under
`mesen-agent`. Use `--home DIR` for an isolated or persistent save/firmware
environment. This is especially important when a system requires firmware or
when multiple jobs execute concurrently.

## JSON Lines contract

Records have an `event` field such as `loaded`, `lua_loaded`, `memory_read`,
`expression`, `disassembly`, `screenshot_saved`, or `summary`. The last normal
record is always `summary`; it includes the stop reason, frame range, and Lua
stop code. Example:

```json
{"event":"loaded","rom":"/work/game.nes","console":"Nes","cpu":"Nes"}
{"event":"memory_read","type":"NesMemory","start":16,"length":1,"hex":"42"}
{"event":"summary","ok":true,"reason":"frame_limit","start_frame":1,"final_frame":61,"frames_executed":60,"stop_code":0}
```

Exit codes are `0` for a successful CLI run, the code supplied to
`emu.stop(code)` for a script-directed stop, `2` for invalid arguments, `3` for
runtime/load/artifact failures, `4` for the wall-clock safety timeout, and `5`
for a Lua load, callback, assertion, or watchdog failure.

## Verification

The smoke test creates a redistributable synthetic NES ROM, runs a Lua callback,
and verifies memory, JSON, PNG, AVI, WAV, save-state, and state-dump outputs:

```sh
make agent
bash Headless/tests/smoke.sh
```
