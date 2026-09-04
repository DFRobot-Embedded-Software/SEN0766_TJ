#!/usr/bin/env python3
"""Capture duration: integer 10–300, else refuse (no clamp)."""

from __future__ import annotations

MIN_S = 10
MAX_S = 300


def accept(raw: str) -> bool:
    if not raw.isdigit() or raw.startswith("0"):
        return False
    n = int(raw)
    return MIN_S <= n <= MAX_S


def main() -> int:
    cases = [
        ("300", True),
        ("10", True),
        ("60", True),
        ("", False),
        ("0", False),
        ("9", False),
        ("301", False),
        ("10.5", False),
        ("0300", False),
    ]
    failed = 0
    for raw, want in cases:
        got = accept(raw)
        if got != want:
            print(f"RED {raw!r}: got {got} want {want}")
            failed += 1
        else:
            print(f"OK  {raw!r} -> {got}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
