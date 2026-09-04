#!/usr/bin/env python3
"""Regression: capture time_ok vs ESP-IDF SNTP status consume-on-read.

esp_sntp_get_sync_status() returns COMPLETED once, then RESET.
The dashboard polls /api/capture; using get_sync_status() as time_ok
makes the UI stick on "等待 NTP 对时" after the first poll.

Sticky callback (firmware) must stay true after the first sync.

Usage: python scripts/diagnose_sntp_status_consume.py
Exit 1 = red, 0 = green.
"""

from __future__ import annotations

SNTP_RESET = 0
SNTP_COMPLETED = 1


class IdfSntpStatus:
    def __init__(self) -> None:
        self._status = SNTP_RESET

    def mark_completed(self) -> None:
        self._status = SNTP_COMPLETED

    def get_sync_status(self) -> int:
        ret = self._status
        if self._status == SNTP_COMPLETED:
            self._status = SNTP_RESET
        return ret


def naive_time_ok(sntp: IdfSntpStatus) -> bool:
    return sntp.get_sync_status() == SNTP_COMPLETED


def sticky_time_ok(flag: list[bool]) -> bool:
    return flag[0]


def poll_status(time_ok_fn, n: int) -> list[bool]:
    return [time_ok_fn() for _ in range(n)]


def main() -> int:
    sntp = IdfSntpStatus()
    sntp.mark_completed()
    naive = poll_status(lambda: naive_time_ok(sntp), 3)

    sticky = [False]
    sticky[0] = True
    sticky_polls = poll_status(lambda: sticky_time_ok(sticky), 3)

    failed = 0
    if naive != [True, False, False]:
        print("RED naive consume-on-read model drifted:", naive)
        failed += 1
    else:
        print("OK  IDF consume-on-read: first poll COMPLETED, rest RESET")

    if sticky_polls != [True, True, True]:
        print("RED sticky time_ok:", sticky_polls)
        failed += 1
    else:
        print("OK  sticky time_ok stays true across dashboard polls")

    if naive[1]:
        print("RED: naive time_ok still true on second poll (would hide the bug)")
        failed += 1

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
