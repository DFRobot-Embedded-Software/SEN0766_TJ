# ADR 0007: Sample zip layout, Event, and monotonic sample id

A finished Capture Session downloads `sample_NNNNNN.zip`. The zip contains one folder of the same name with `audio.wav`, `environment.csv`, `radar.csv`, and `label.json`. `label.json` has only `sample_id`, `start_time` (CST-8 `YYYY-MM-DDTHH:MM:SS.000`, no timezone suffix), and `event`.

Event is a data-page field stored on the device like Room. Empty Event refuses start. Duration and Event are frozen at start.

The six-digit sample counter lives in NVS and increments only on a normal stop. Abort does not consume a number. After 999999 the counter wraps to 000001.
