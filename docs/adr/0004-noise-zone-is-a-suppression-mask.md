# Noise Zone is a suppression mask

A person whose point lies in a Noise Zone is omitted from every occupancy and boundary row in the radar log, even if that point is also inside another Zone. We chose a shield over “overlaps count independently” so fan/plant noise cannot inflate people counts. Noise and disabled tags still do not get their own CSV rows.
