# Environment log sound_db is uncalibrated

`env.csv` records `sound_db` from microphone RMS (Kconfig reference only), without the webpage calibration offset. The live data page still shows the offset-adjusted value. Aligning the two would make field notes disagree with the CSV; keeping them separate lets you recalibrate later without rewriting captures.
