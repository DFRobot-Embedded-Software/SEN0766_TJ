# MCP Sensors

A room-installed node that senses environment and occupancy, and can run a capture session for offline audio plus CSV logs.

## Language

**Room**:
The named physical space where this device is installed.
_Avoid_: Zone, Area

**Zone**:
A named radar detection region inside a Room.
_Avoid_: z0om, zoom, Room

**Capture Session**:
A web-started, web-stopped interval that yields one WAV plus an environment log and a radar log.
_Avoid_: Uplink, stream (live audio to a voice hub)

**Environment log**:
A CSV of timestamped temperature, humidity, CO₂, lux, and sound level for one Capture Session.

**Sound level**:
Ambient loudness in dB from the microphone RMS, without the webpage calibration offset.
_Avoid_: Webpage `sound` (that value includes the offset)

**Radar log**:
A CSV of timestamped occupancy for one Capture Session. One row per occupancy or boundary Zone per sample. Noise and disabled Zones are omitted.

**Crossing**:
A Zone boundary event in that 1-second sample: `0` enter, `1` leave, `2` none — same bytes as the C4004 boundary wire protocol. It is the edge in the window, not the 2.5s UI hold. Boundary Zones use the door signal; occupancy Zones use people appearing (0→>0) or disappearing (>0→0).
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
