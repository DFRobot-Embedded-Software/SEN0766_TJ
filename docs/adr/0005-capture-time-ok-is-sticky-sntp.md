# ADR 0005: Capture time_ok is sticky after first SNTP sync

## Status

Accepted

## Context

A Capture Session must not start until the device clock is a real Unix time. The dashboard polls `GET /api/capture` and shows “等待 NTP 对时” while `time_ok` is false.

ESP-IDF `esp_sntp_get_sync_status()` returns `SNTP_SYNC_STATUS_COMPLETED` once, then resets the stored status to `RESET`. Using that API as `time_ok` makes the second poll look unsynced until the next hourly SNTP refresh.

## Decision

Register `esp_sntp_set_time_sync_notification_cb`. On the first successful sync, set a sticky flag. `capture_session_time_ok()` reads only that flag.

Keep requiring a real SNTP sync. Do not treat a 1970 `time()` value as ready.

Use more than one NTP hostname (Aliyun, Tencent, `pool.ntp.org`) so a blocked public pool is not the only path.

## Consequences

After one successful reply, the UI stays on “可以对时，可采集” across polls. If the LAN has no DNS/NTP path to the internet, the wait remains until that path exists.
