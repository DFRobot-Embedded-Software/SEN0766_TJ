# ADR 0006: Capture duration is 10–300 s, frozen at start

The data page configures how long a Capture Session runs. The value is stored on the device like Room. The range stays 10–300 seconds (default 300) so browser-held WAV does not grow past the existing five-minute ceiling. A running session keeps the duration from start; edits apply next time. Empty or out-of-range values refuse start rather than clamp, so a mistype cannot silently become 300 s.
