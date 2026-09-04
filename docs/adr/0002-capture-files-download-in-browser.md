# Capture artifacts download in the browser

This hardware has no SD card and the SPIFFS web partition is 1 MB, too small for minutes of 16 kHz WAV. A Capture Session streams PCM and 1 Hz samples to the browser, which writes `capture.wav`, `env.csv`, and `radar.csv` on stop. Closing the tab or dropping Wi‑Fi discards the session; there is no onboard resume.
