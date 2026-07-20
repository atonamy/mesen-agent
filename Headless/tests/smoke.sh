#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
platform="$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"
if [[ "$platform" == "darwin-arm64" ]]; then platform="osx-arm64"; fi
if [[ "$platform" == "darwin-x86_64" ]]; then platform="osx-x64"; fi
if [[ "$platform" == "linux-x86_64" ]]; then platform="linux-x64"; fi
if [[ "$platform" == "linux-aarch64" ]]; then platform="linux-arm64"; fi
cli="${MESEN_AGENT:-$root/bin/$platform/Release/mesen-agent}"
python_bin="${PYTHON:-python3}"
if ! command -v "$python_bin" >/dev/null 2>&1; then python_bin=python; fi
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$python_bin" "$root/Headless/tests/create_test_rom.py" "$work/test.nes"
"$cli" "$work/test.nes" \
	--home "$work/home" \
	--frames 20 \
	--lua "$root/Headless/tests/smoke.lua" \
	--read-memory NesMemory:0x10:1 \
	--dump-memory "NesInternalRam=$work/ram.bin" \
	--state-json "$work/state.json" \
	--save-state "$work/state.mss" \
	--screenshot "$work/frame.png" \
	--video "$work/gameplay.avi" \
	--audio "$work/audio.wav" \
	--pc \
	--eval '[$10]' > "$work/events.jsonl"

"$python_bin" - "$work" <<'PY'
import json
import pathlib
import sys

work = pathlib.Path(sys.argv[1])
events = [json.loads(line) for line in (work / "events.jsonl").read_text().splitlines()]
assert events[-1]["event"] == "summary" and events[-1]["ok"]
assert any(e.get("event") == "lua_log" and "frame 5" in e.get("text", "") for e in events)
assert any(e.get("event") == "memory_read" and e.get("hex") == "42" for e in events)
for name in ["ram.bin", "state.json", "state.mss", "frame.png", "gameplay.avi", "audio.wav"]:
    assert (work / name).stat().st_size > 0, name
assert (work / "frame.png").read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
assert (work / "audio.wav").read_bytes().startswith(b"RIFF")
print("headless smoke test passed")
PY

set +e
"$cli" "$work/test.nes" --home "$work/stop-home" --frames 100 \
	--lua "$root/Headless/tests/stop.lua" > "$work/stop-events.jsonl"
stop_code=$?
set -e
[[ "$stop_code" -eq 7 ]]
"$python_bin" - "$work/stop-events.jsonl" <<'PY'
import json
import pathlib
import sys

events = [json.loads(line) for line in pathlib.Path(sys.argv[1]).read_text().splitlines()]
assert events[-1]["event"] == "summary"
assert events[-1]["reason"] == "emulation_stopped"
assert events[-1]["stop_code"] == 7
assert any(e.get("event") == "lua_log" and "code 7" in e.get("text", "") for e in events)
print("Lua stop-code smoke test passed")
PY

"$cli" "$work/test.nes" --home "$work/reload-home" --frames 0 \
	--load-state "$work/state.mss" --read-memory NesMemory:0x10:1 \
	> "$work/reload-events.jsonl"
"$cli" "$work/test.nes" --home "$work/break-home" --frames 100 \
	--breakpoint 'NesMemory:0x8013:x' --step 1 --pc \
	> "$work/break-events.jsonl"
"$cli" "$work/test.nes" --home "$work/trace-home" --frames 1 \
	--trace "$work/trace.log" --disassemble 0x8000:2 > "$work/trace-events.jsonl"

"$python_bin" - "$work" <<'PY'
import json
import pathlib
import sys

work = pathlib.Path(sys.argv[1])
reload_events = [json.loads(line) for line in (work / "reload-events.jsonl").read_text().splitlines()]
assert any(e.get("event") == "memory_read" and e.get("hex") == "42" for e in reload_events)

break_events = [json.loads(line) for line in (work / "break-events.jsonl").read_text().splitlines()]
assert break_events[-1]["reason"] == "debugger_break"
assert any(e.get("event") == "program_counter" and e.get("value") == 0x8015 for e in break_events)

trace_events = [json.loads(line) for line in (work / "trace-events.jsonl").read_text().splitlines()]
assert any(e.get("event") == "trace_saved" for e in trace_events)
assert any(e.get("event") == "disassembly" and e.get("bytes") == "78" and e.get("text", "").startswith("SEI") for e in trace_events)
assert (work / "trace.log").stat().st_size > 1000
print("state reload, breakpoint, step, and trace smoke tests passed")
PY

"$cli" "$work/test.nes" --home "$work/cli-input-home" --frames 20 \
	--input '0:start@5' --read-memory NesMemory:0x12:3 > "$work/cli-input-events.jsonl"
"$cli" "$work/test.nes" --home "$work/lua-input-home" --frames 20 \
	--lua "$root/Headless/tests/input.lua" --read-memory NesMemory:0x12:3 \
	> "$work/lua-input-events.jsonl"
"$cli" "$work/test.nes" --home "$work/combined-input-home" --frames 20 \
	--lua "$root/Headless/tests/input-a.lua" --input '0:start@5' \
	--read-memory NesMemory:0x12:3 > "$work/combined-input-events.jsonl"
"$cli" "$work/test.nes" --home "$work/end-frame-input-home" --frames 20 \
	--lua "$root/Headless/tests/lua-input-end-frame.lua" \
	--read-memory NesMemory:0x12:3 > "$work/end-frame-input-events.jsonl"

"$python_bin" - "$work" <<'PY'
import json
import pathlib
import sys

work = pathlib.Path(sys.argv[1])
cli_events = [json.loads(line) for line in (work / "cli-input-events.jsonl").read_text().splitlines()]
assert cli_events[-1]["event"] == "summary" and cli_events[-1]["ok"]
assert any(e.get("event") == "memory_read" and e.get("hex") == "010000" for e in cli_events)

lua_events = [json.loads(line) for line in (work / "lua-input-events.jsonl").read_text().splitlines()]
assert lua_events[-1]["event"] == "summary" and lua_events[-1]["ok"]
assert any(e.get("event") == "memory_read" and e.get("hex") == "010100" for e in lua_events)
assert any(e.get("event") == "lua_log" and "controller buttons available" in e.get("text", "") for e in lua_events)

combined_events = [json.loads(line) for line in (work / "combined-input-events.jsonl").read_text().splitlines()]
assert combined_events[-1]["event"] == "summary" and combined_events[-1]["ok"]
assert any(e.get("event") == "memory_read" and e.get("hex") == "010100" for e in combined_events)
end_frame_events = [json.loads(line) for line in (work / "end-frame-input-events.jsonl").read_text().splitlines()]
assert end_frame_events[-1]["ok"]
assert any(e.get("event") == "memory_read" and e.get("hex") == "010000" for e in end_frame_events)
print("deterministic CLI and Lua input smoke tests passed")
PY

"$cli" "$work/test.nes" --home "$work/lua-nes-ram-home" --frames 1 \
	--lua "$root/Headless/tests/lua-nes-ram.lua" \
	--read-memory NesInternalRam:0xF0:3 > "$work/lua-nes-ram-events.jsonl"
set +e
"$cli" "$work/test.nes" --home "$work/lua-invalid-memory-home" --frames 1 \
	--lua "$root/Headless/tests/lua-invalid-memory-type.lua" \
	> "$work/lua-invalid-memory-events.jsonl"
invalid_memory_code=$?
set -e
[[ "$invalid_memory_code" -eq 5 ]]

"$python_bin" - "$work" <<'PY'
import json
import pathlib
import sys

work = pathlib.Path(sys.argv[1])

ram = [json.loads(line) for line in (work / "lua-nes-ram-events.jsonl").read_text().splitlines()]
assert ram[-1]["ok"]
assert any(e.get("event") == "memory_read" and e.get("hex") == "abefcd" for e in ram)
assert "round trips passed" in next(e["text"] for e in ram if e.get("event") == "lua_log")

invalid = [json.loads(line) for line in (work / "lua-invalid-memory-events.jsonl").read_text().splitlines()]
assert invalid[-1]["reason"] == "lua_error" and not invalid[-1]["ok"]
invalid_log = next(e["text"] for e in invalid if e.get("event") == "lua_log")
assert "memoryType must be a number" in invalid_log
print("Lua NES RAM alias, read/write, and invalid-memory-enum smoke tests passed")
PY

set +e
"$cli" "$work/test.nes" --home "$work/lua-load-error-home" --frames 10 \
	--lua "$root/Headless/tests/lua-load-error.lua" > "$work/lua-load-error-events.jsonl"
load_error_code=$?
"$cli" "$work/test.nes" --home "$work/lua-runtime-error-home" --frames 100 \
	--lua "$root/Headless/tests/lua-runtime-error.lua" \
	--state-json "$work/lua-runtime-error-state.json" \
	--screenshot "$work/lua-runtime-error.png" > "$work/lua-runtime-error-events.jsonl"
runtime_error_code=$?
"$cli" "$work/test.nes" --home "$work/lua-timeout-home" --frames 100 --timeout 5 \
	--lua-timeout 1 --lua "$root/Headless/tests/lua-timeout.lua" \
	> "$work/lua-timeout-events.jsonl"
timeout_error_code=$?
set -e

[[ "$load_error_code" -eq 5 ]]
[[ "$runtime_error_code" -eq 5 ]]
[[ "$timeout_error_code" -eq 5 ]]

"$cli" "$work/test.nes" --home "$work/lua-multi-home" --frames 4 \
	--lua "$root/Headless/tests/lua-multi-first.lua" \
	--lua "$root/Headless/tests/lua-multi-second.lua" \
	--read-memory NesMemory:0x15:1 > "$work/lua-multi-events.jsonl"
"$cli" "$work/test.nes" --home "$work/lua-event-order-home" --frames 4 \
	--lua "$root/Headless/tests/lua-event-order.lua" \
	> "$work/lua-event-order-events.jsonl"
"$cli" "$work/test.nes" --home "$work/lua-script-ended-home" --frames 1 \
	--lua "$root/Headless/tests/lua-script-ended.lua" \
	> "$work/lua-script-ended-events.jsonl"
set +e
"$cli" "$work/test.nes" --home "$work/lua-script-ended-error-home" --frames 1 \
	--lua "$root/Headless/tests/lua-script-ended-error.lua" \
	> "$work/lua-script-ended-error-events.jsonl"
script_ended_error_code=$?
"$cli" "$work/test.nes" --home "$work/lua-script-ended-stop-home" --frames 1 \
	--lua "$root/Headless/tests/lua-script-ended-stop.lua" \
	> "$work/lua-script-ended-stop-events.jsonl"
script_ended_stop_code=$?
set -e
[[ "$script_ended_error_code" -eq 5 ]]
[[ "$script_ended_stop_code" -eq 9 ]]

"$python_bin" - "$work" <<'PY'
import json
import pathlib
import sys

work = pathlib.Path(sys.argv[1])

def events(name):
    return [json.loads(line) for line in (work / name).read_text().splitlines()]

load_error = events("lua-load-error-events.jsonl")
assert load_error[-1]["reason"] == "lua_error" and not load_error[-1]["ok"]
assert load_error[-1]["frames_executed"] == 0
assert any(e.get("event") == "lua_error" and e.get("phase") == "load" and e.get("error_count") == 1 for e in load_error)

runtime_error = events("lua-runtime-error-events.jsonl")
assert runtime_error[-1]["reason"] == "lua_error" and not runtime_error[-1]["ok"]
assert any(e.get("event") == "lua_error" and e.get("phase") == "callback" and e.get("error_count") == 1 for e in runtime_error)
runtime_log = next(e["text"] for e in runtime_error if e.get("event") == "lua_log")
assert "intentional runtime callback failure 1" in runtime_log
assert "intentional runtime callback failure 2" not in runtime_log
assert (work / "lua-runtime-error-state.json").stat().st_size > 0
assert (work / "lua-runtime-error.png").read_bytes().startswith(b"\x89PNG\r\n\x1a\n")

timeout_error = events("lua-timeout-events.jsonl")
assert timeout_error[-1]["reason"] == "lua_error" and not timeout_error[-1]["ok"]
timeout_log = next(e["text"] for e in timeout_error if e.get("event") == "lua_log")
assert timeout_log.count("Maximum execution time (1 seconds) exceeded.") == 1

multi = events("lua-multi-events.jsonl")
assert multi[-1]["ok"] and multi[-1]["frames_executed"] == 4
assert any(e.get("event") == "memory_read" and e.get("hex") == "a5" for e in multi)
assert [e.get("error_count") for e in multi if e.get("event") == "lua_log"] == [0, 0]
assert "first script wrote marker" in next(e["text"] for e in multi if e.get("id") == 1 and e.get("event") == "lua_log")
assert "second script observed marker" in next(e["text"] for e in multi if e.get("id") == 2 and e.get("event") == "lua_log")

order = events("lua-event-order-events.jsonl")
assert order[-1]["ok"] and order[-1]["frames_executed"] == 4
order_log = next(e["text"] for e in order if e.get("event") == "lua_log")
assert "event-order=end,input,start,end,input,start,end" in order_log

script_ended = events("lua-script-ended-events.jsonl")
assert script_ended[-1]["ok"]
assert "scriptEnded callback ran" in next(e["text"] for e in script_ended if e.get("event") == "lua_log")

script_ended_error = events("lua-script-ended-error-events.jsonl")
assert script_ended_error[-1]["reason"] == "lua_error" and not script_ended_error[-1]["ok"]
assert "intentional scriptEnded failure" in next(e["text"] for e in script_ended_error if e.get("event") == "lua_log")

script_ended_stop = events("lua-script-ended-stop-events.jsonl")
assert script_ended_stop[-1]["reason"] == "emulation_stopped"
assert script_ended_stop[-1]["stop_code"] == 9
assert "scriptEnded requested stop" in next(e["text"] for e in script_ended_stop if e.get("event") == "lua_log")
print("Lua error, timeout, multi-script, and event-order smoke tests passed")
PY

set +e
"$cli" "$work/test.nes" --home "$work/lua-nonstring-error-home" --frames 10 \
	--lua "$root/Headless/tests/lua-nonstring-error.lua" \
	> "$work/lua-nonstring-error-events.jsonl"
nonstring_error_code=$?
"$cli" "$work/test.nes" --home "$work/lua-load-nonstring-error-home" --frames 10 \
	--lua "$root/Headless/tests/lua-load-nonstring-error.lua" \
	> "$work/lua-load-nonstring-error-events.jsonl"
load_nonstring_error_code=$?
"$cli" "$work/test.nes" --home "$work/lua-sandbox-home" --frames 0 \
	--lua "$root/Headless/tests/lua-require.lua" \
	> "$work/lua-sandbox-events.jsonl"
sandbox_error_code=$?
"$cli" "$work/test.nes" --home "$work/lua-invalid-timeout-home" --frames 0 \
	--timeout 1 --lua-timeout 2 > "$work/lua-invalid-timeout-events.jsonl" 2>&1
invalid_timeout_code=$?
set -e

[[ "$nonstring_error_code" -eq 5 ]]
[[ "$load_nonstring_error_code" -eq 5 ]]
[[ "$sandbox_error_code" -eq 5 ]]
[[ "$invalid_timeout_code" -eq 2 ]]

"$cli" "$work/test.nes" --home "$work/lua-require-home" --frames 0 \
	--lua-allow-io --lua "$root/Headless/tests/lua-require.lua" \
	> "$work/lua-require-events.jsonl"
"$cli" "$work/test.nes" --home "$work/lua-network-home" --frames 0 \
	--lua-allow-network --lua "$root/Headless/tests/lua-network-library.lua" \
	> "$work/lua-network-events.jsonl"

quoted_dir="$work/quoted'path"
mkdir -p "$quoted_dir"
cp "$root/Headless/tests/lua-require.lua" "$root/Headless/tests/lua-helper.lua" "$quoted_dir/"
quoted_script="$quoted_dir/lua-require.lua"
case "$(uname -s)" in
	MINGW*|MSYS*) quoted_script="$(cygpath -w "$quoted_script")" ;;
esac
"$cli" "$work/test.nes" --home "$work/lua-quoted-path-home" --frames 0 \
	--lua-allow-io --lua "$quoted_script" \
	> "$work/lua-quoted-path-events.jsonl"

"$cli" "$work/test.nes" --home "$work/lua-debug-ops-home" --frames 100 \
	--lua "$root/Headless/tests/lua-debug-ops.lua" --step 1 --pc \
	--screenshot "$work/lua-debug-ops.png" > "$work/lua-debug-ops-events.jsonl"
"$cli" "$work/test.nes" --home "$work/lua-savestate-roundtrip-home" --frames 100 \
	--lua "$root/Headless/tests/lua-savestate-roundtrip.lua" --pc \
	> "$work/lua-savestate-roundtrip-events.jsonl"

"$python_bin" - "$work" <<'PY'
import json
import pathlib
import sys

work = pathlib.Path(sys.argv[1])

def events(name):
    return [json.loads(line) for line in (work / name).read_text().splitlines()]

for name in ["lua-nonstring-error-events.jsonl", "lua-load-nonstring-error-events.jsonl"]:
    result = events(name)
    assert result[-1]["reason"] == "lua_error" and not result[-1]["ok"]
    log = next(e for e in result if e.get("event") == "lua_log")
    assert log["error_count"] == 1 and "Lua error object of type 'table'" in log["text"]

sandbox = events("lua-sandbox-events.jsonl")
assert sandbox[-1]["reason"] == "lua_error"
assert "--lua-allow-io" in next(e["text"] for e in sandbox if e.get("event") == "lua_log")

for name, marker in [
    ("lua-require-events.jsonl", "relative require loaded sibling module"),
    ("lua-quoted-path-events.jsonl", "relative require loaded sibling module"),
    ("lua-network-events.jsonl", "network library loaded without IO/OS"),
]:
    result = events(name)
    assert result[-1]["ok"] and result[-1]["frames_executed"] == 0
    log = next(e for e in result if e.get("event") == "lua_log")
    assert log["error_count"] == 0 and marker in log["text"]

debug_ops = events("lua-debug-ops-events.jsonl")
assert debug_ops[-1]["ok"] and debug_ops[-1]["reason"] == "debugger_break"
debug_log = next(e for e in debug_ops if e.get("event") == "lua_log")
assert debug_log["error_count"] == 0
assert "exec callback captured PNG and save state" in debug_log["text"]
assert (work / "lua-debug-ops.png").read_bytes().startswith(b"\x89PNG\r\n\x1a\n")

state_roundtrip = events("lua-savestate-roundtrip-events.jsonl")
assert state_roundtrip[-1]["ok"] and state_roundtrip[-1]["reason"] == "debugger_break"
state_log = next(e for e in state_roundtrip if e.get("event") == "lua_log")
assert state_log["error_count"] == 0
assert "save-state round trip succeeded without reentrant state events" in state_log["text"]
print("Lua sandbox, module path, non-string error, and debugger-operation smoke tests passed")
PY
