# Radar log Crossing uses C4004 wire bytes

Capture-session radar CSV writes Crossing as `0` enter, `1` leave, `2` none — the same values as the C4004 boundary UART field, not the more intuitive “0 = none” encoding. Dataset consumers can match firmware logs and the DFRobot protocol without a translation table.
