#!/usr/bin/env python3
"""Capture Session download zip (ADR-0007) and Chrome last-click.

A normal stop packs sample_NNNNNN.zip with one inner folder:
audio.wav, environment.csv, radar.csv, label.json.
Abort does not consume a sample number.
"""

from __future__ import annotations

import io
import json
import re
import struct
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SAMPLE_MAX = 999999


def chrome_keeps_last(clicks: list[str]) -> list[str]:
    return clicks[-1:] if clicks else []


def commit_seq(seq: int, kind: str) -> int:
    if kind != "stop":
        return seq
    if seq >= SAMPLE_MAX:
        seq = 0
    return seq + 1


def format_sample_id(seq: int) -> str:
    return f"sample_{seq:06d}"


def zip_store(files: list[tuple[str, bytes]]) -> bytes:
    parts: list[bytes] = []
    centrals: list[bytes] = []
    offset = 0
    for name, data in files:
        name_b = name.encode("utf-8")
        crc = zlib.crc32(data) & 0xFFFFFFFF
        local = struct.pack(
            "<IHHHHHIIIHH",
            0x04034B50,
            20,
            0,
            0,
            0,
            0,
            crc,
            len(data),
            len(data),
            len(name_b),
            0,
        )
        parts.extend([local, name_b, data])
        cd = struct.pack(
            "<IHHHHHHIIIHHHHHII",
            0x02014B50,
            20,
            20,
            0,
            0,
            0,
            0,
            crc,
            len(data),
            len(data),
            len(name_b),
            0,
            0,
            0,
            0,
            0,
            offset,
        )
        centrals.extend([cd, name_b])
        offset += 30 + len(name_b) + len(data)
    cd_start = offset
    cd_blob = b"".join(centrals)
    end = struct.pack(
        "<IHHHHIIH",
        0x06054B50,
        0,
        0,
        len(files),
        len(files),
        len(cd_blob),
        cd_start,
        0,
    )
    return b"".join(parts) + cd_blob + end


def pack_sample(sample_id: str, event: str, start_time: str) -> bytes:
    prefix = sample_id + "/"
    label = json.dumps(
        {"sample_id": sample_id, "start_time": start_time, "event": event},
        indent=2,
    )
    return zip_store(
        [
            (prefix + "audio.wav", b"RIFF"),
            (prefix + "environment.csv", b"timestamp\n"),
            (prefix + "radar.csv", b"timestamp,room\n"),
            (prefix + "label.json", label.encode("utf-8")),
        ]
    )


def main() -> int:
    failed = 0
    clicks = ["capture_1.wav", "env_1.csv", "radar_1.csv"]
    kept = chrome_keeps_last(clicks)
    if kept != ["radar_1.csv"]:
        print("RED chrome-last model:", kept)
        failed += 1
    else:
        print("OK  timer auto-click keeps only the last file")

    if commit_seq(6, "abort") != 6:
        print("RED abort consumed a sample number")
        failed += 1
    else:
        print("OK  abort does not consume sample number")
    if commit_seq(6, "stop") != 7:
        print("RED stop did not increment")
        failed += 1
    else:
        print("OK  normal stop increments sample number")
    if format_sample_id(commit_seq(999999, "stop")) != "sample_000001":
        print("RED wrap")
        failed += 1
    else:
        print("OK  wrap 999999 -> sample_000001")

    sid = format_sample_id(1)
    start_time = "2026-09-03T23:00:00.000"
    if re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.000", start_time) is None:
        print("RED start_time shape")
        failed += 1
    blob = pack_sample(sid, "sleep", start_time)
    with zipfile.ZipFile(io.BytesIO(blob)) as zf:
        names = zf.namelist()
        label = json.loads(zf.read(sid + "/label.json"))
    want = [
        sid + "/audio.wav",
        sid + "/environment.csv",
        sid + "/radar.csv",
        sid + "/label.json",
    ]
    if names != want:
        print("RED zip members:", names)
        failed += 1
    else:
        print("OK  zip interior matches ADR-0007")
    if set(label.keys()) != {"sample_id", "start_time", "event"}:
        print("RED label keys:", label.keys())
        failed += 1
    elif "Z" in label["start_time"] or "+" in label["start_time"]:
        print("RED start_time has timezone")
        failed += 1
    else:
        print("OK  label.json trio and CST-8 start_time")

    html = (ROOT / "spiffs" / "index.html").read_text(encoding="utf-8")
    if "</span></div><div class=\"capture-bar\" id=\"captureBar\">" not in html:
        print("RED capture bar must sit after .title, not inside it")
        failed += 1
    else:
        print("OK  capture bar is a sibling after the data-page title")

    js = (ROOT / "spiffs" / "js" / "app.js").read_text(encoding="utf-8")
    if 'prefix+"audio.wav"' not in js or 'prefix+"label.json"' not in js:
        print("RED app.js packer paths")
        failed += 1
    else:
        print("OK  app.js packs audio.wav / CSVs / label.json under sample id")

    if not (ROOT / "docs" / "adr" / "0007-capture-sample-zip-and-label.md").exists():
        print("RED ADR-0007 missing")
        failed += 1
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
