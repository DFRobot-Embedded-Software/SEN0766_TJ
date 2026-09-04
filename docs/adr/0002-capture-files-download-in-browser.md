# Capture artifacts download in the browser

This hardware has no SD card and the SPIFFS web partition is 1 MB, too small for minutes of 16 kHz WAV. A Capture Session streams PCM and 1 Hz samples to the browser. A normal stop packs `sample_NNNNNN.zip` (folder `sample_NNNNNN/` with wav, CSVs, and `label.json`; ADR-0007) and shows a Download control. Auto `a.click()` after a timer or after `/api/capture/stop` is not a user gesture, so Chrome blocks it; PCM teardown also resets httpd sockets (recv 104). The operator must click Download ZIP. Tab close still aborts with no files.

