#!/usr/bin/env python3
"""Create a tiny deterministic NROM image for headless CLI smoke tests."""

import pathlib
import sys


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: create_test_rom.py OUTPUT.nes")

    prg = bytearray([0xEA] * 0x4000)
    # Initialize the machine, then poll controller 1 forever.  $12 latches
    # Start and $13 latches A so one-frame input schedules remain observable;
    # $14 stores the current Start state to verify that the button is released.
    program = bytes(
        [
            0x78,
            0xD8,
            0xA2,
            0xFF,
            0x9A,
            0xE8,
            0x8E,
            0x00,
            0x20,
            0x8E,
            0x01,
            0x20,
            0x8E,
            0x10,
            0x40,
            0x86,
            0x12,
            0x86,
            0x13,
            0xE6,
            0x00,
            0xA9,
            0x01,
            0x8D,
            0x16,
            0x40,
            0x4A,
            0x8D,
            0x16,
            0x40,
            0xAD,
            0x16,
            0x40,
            0x29,
            0x01,
            0x05,
            0x13,
            0x85,
            0x13,
            0xAD,
            0x16,
            0x40,
            0xAD,
            0x16,
            0x40,
            0xAD,
            0x16,
            0x40,
            0x29,
            0x01,
            0x85,
            0x14,
            0x05,
            0x12,
            0x85,
            0x12,
            0x4C,
            0x13,
            0x80,
        ]
    )
    prg[: len(program)] = program
    prg[0x3FFA:0x4000] = bytes([0x00, 0x80, 0x00, 0x80, 0x00, 0x80])

    header = b"NES\x1a" + bytes([1, 1, 0, 0]) + bytes(8)
    pathlib.Path(sys.argv[1]).write_bytes(header + prg + bytes(0x2000))


if __name__ == "__main__":
    main()
