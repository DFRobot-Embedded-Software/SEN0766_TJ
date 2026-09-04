# MCP Sensors

A room-installed node that senses environment and occupancy, and can run a capture session for offline audio plus CSV logs.

## Language

**Room**:
The named physical space where this device is installed.
_Avoid_: Zone, Area

**Zone**:
A named radar detection region inside a Room. Occupancy and boundary Zones appear in the radar log; Noise and disabled tags do not.
_Avoid_: z0om, zoom, Room

**Noise Zone**:
A suppression mask, not an occupancy region. A tracked person whose point falls inside a Noise Zone is omitted from every occupancy and boundary count.
_Avoid_: Treating Noise as an overlapping Zone that still contributes people

**Capture Session**:
A web-started interval of a configured duration that yields one downloadable zip named `sample_NNNNNN.zip`. Inside is a folder `sample_NNNNNN/` containing `audio.wav`, `environment.csv`, `radar.csv`, and `label.json`. It may start only after SNTP has completed. Duration is 10–300 seconds (default 300). Event and duration are set on the data page and stored on the device like Room. A session uses the values in force at start. Reaching the duration or an early stop are both a normal stop; abort (tab close) discards the zip and does not consume a sample number. The sample counter persists across reboot.
_Avoid_: Uplink, stream; treating only a button stop as a normal stop

**Sample id**:
The folder and `label.json` id for one normally finished Capture Session, `sample_` plus a six-digit counter (`sample_000001`). The device increments it only after a normal stop.
_Avoid_: Using the Unix time of the zip as the sample id

**Event**:
The operator-chosen label for what this Capture Session is recording (example: `sleep`). It is stored on the device and written to `label.json`. Empty Event refuses start.
_Avoid_: Radar or environment bus events; treating Event as a Zone

**label.json**:
Metadata for one sample: `sample_id`, `start_time` (CST-8 `YYYY-MM-DDTHH:MM:SS.000`, no timezone suffix), and `event`.
_Avoid_: Extra keys not in that trio

**Environment log**:
A CSV of wall-clock samples (CST-8 `YYYY-MM-DD HH:MM:SS`) with temperature, humidity, CO₂, lux, and sound level for one Capture Session.
_Avoid_: Unix epoch seconds as the timestamp column

**Sound level**:
Ambient loudness in dB from the microphone RMS, without the webpage calibration offset.
_Avoid_: Webpage `sound` (that value includes the offset)

**Radar log**:
A CSV of wall-clock occupancy samples (same timestamp format as the environment log) for one Capture Session. One row per occupancy or boundary Zone per sample. Noise and disabled Zones are omitted.

**Crossing**:
A Zone boundary event in that 1-second sample: `0` enter, `1` leave, `2` none — same bytes as the C4004 boundary wire protocol. It is the edge in the window, not the 2.5s UI hold. The first window of a session still records an edge. If enter and leave both occur in one second, leave wins. Boundary Zones use the door wire byte; occupancy Zones use people appearing (0→>0) or disappearing (>0→0).
_Avoid_: 无状态=0 (that encoding was rejected)

**Stationary count**:
How many tracked people in a Zone are breath-only still (kinesia 1). Targets with kinesia 0 are not people and are not counted.
_Avoid_: Treating kinesia 0 as a person

**Micro-movement count**:
How many tracked people in a Zone have small motion (kinesia 2–30).
_Avoid_: Active count (that is the firmware's two-bucket moving total)

**Slow-activity count**:
How many tracked people in a Zone are moving slowly (kinesia 31–60).

**Fast-activity count**:
How many tracked people in a Zone are moving quickly (kinesia 61–100).
