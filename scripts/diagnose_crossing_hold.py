#!/usr/bin/env python3
"""Regression loop: capture Crossing vs C4004 2.5s UI hold.

Mirrors the fixed path:
- UART ENTER/EXIT sets pending flags (c4004_sensor_take_boundary_crossing)
- take() consumes the window; leave wins if both seen
- UI hold bools are NOT used

Symptom that must stay green: two ENTER wire bytes 1000 ms apart
both emit crossing=0.

Usage: python scripts/diagnose_crossing_hold.py
Exit 1 = red, 0 = green.
"""

from __future__ import annotations

CROSSING_ENTER = 0
CROSSING_LEAVE = 1
CROSSING_NONE = 2
C4004_BOUNDARY_ENTER = 0x00
C4004_BOUNDARY_EXIT = 0x01
C4004_BOUNDARY_NONE = 0x02


def apply_boundary_value(pending: dict, value: int) -> None:
    if value == C4004_BOUNDARY_ENTER:
        pending["enter"] = True
    elif value == C4004_BOUNDARY_EXIT:
        pending["exit"] = True


def take_boundary_crossing(pending: dict) -> int:
    saw_enter = pending["enter"]
    saw_exit = pending["exit"]
    pending["enter"] = False
    pending["exit"] = False
    if saw_exit:
        return CROSSING_LEAVE
    if saw_enter:
        return CROSSING_ENTER
    return CROSSING_NONE


def run_sequence(wire_at_ms: list[tuple[int, int]], sample_ms: list[int]) -> list[int]:
    pending = {"enter": False, "exit": False}
    out = []
    events = list(wire_at_ms)
    for t in sample_ms:
        while events and events[0][0] <= t:
            apply_boundary_value(pending, events[0][1])
            events.pop(0)
        out.append(take_boundary_crossing(pending))
    return out


def main() -> int:
    cases = [
        (
            "ENTER@0ms, ENTER@1000ms; samples @0,1000,2000",
            [(0, C4004_BOUNDARY_ENTER), (1000, C4004_BOUNDARY_ENTER)],
            [0, 1000, 2000],
            [CROSSING_ENTER, CROSSING_ENTER, CROSSING_NONE],
        ),
        (
            "first window ENTER",
            [(0, C4004_BOUNDARY_ENTER)],
            [0],
            [CROSSING_ENTER],
        ),
        (
            "same window enter then leave → leave",
            [(0, C4004_BOUNDARY_ENTER), (200, C4004_BOUNDARY_EXIT)],
            [1000],
            [CROSSING_LEAVE],
        ),
    ]
    failed = 0
    for title, wires, samples, want in cases:
        got = run_sequence(wires, samples)
        ok = got == want
        print(f"{'GREEN' if ok else 'RED'} {title}")
        print(f"  got={got} want={want}")
        if not ok:
            failed += 1
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
