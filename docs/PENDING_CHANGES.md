# Firmware changes waiting for the analysis run

The owner's decision, 2026-10-04: analysis first, then every firmware change in one go, as separate
commits, followed by a confirming run. Nothing here is built yet. Tools and harness fixes are not on this
list; they went in first so the analysis could be trusted.

## Sure, from evidence already in hand

1. **An access point advertises room it then refuses.** Its own console says "No heap to serve another
   handheld; refusing connection" while its beacon says "1 free" (Everest, 16 times in three minutes;
   Denali stranded two handhelds for 1 h 46 min on 2026-10-03).
   - The beacon and the door use one number, with headroom for the cost of the join itself.
   - A full access point says so in a reply instead of closing the socket, so the handheld shows "AP full"
     rather than "session receive error".
   - A handheld refused by the only access point in range backs off instead of retrying every 10 s.
   - Then cut what each session costs (TCP windows, AMPDU, socket count), each measured on a cabled access
     point before it is kept.
   - Touches D84: record a new decision before building.

2. **Screen saver: show the grid time.** A third line under the handheld's name and its access point,
   drawn the same way as those two: the UI font in the rain's colours, each character lit white when a
   stream's head crosses it and fading back down the ramp, never falling. (Owner, 2026-10-04.)
   - Assumed until told otherwise: hours and minutes as the status bar shows them, repainting only the
     characters that changed when the minute turns, and "time not set" when the grid has no time (D6).

## Waiting on what the analysis shows

- Why Fuji panicked at 1 KB of heap.
- Why Everest and Denali reported 0 KB lowest heap in the beacon while the console says about 4 KB.
- Why handhelds re-register about twenty times each in twelve hours; Marshall leaving Fuji at 62 s.
- Why receiving over LoRa loses about a fifth while sending loses nothing.
- Whether "refused (time)" at the end of a battery run is the access point losing its clock.
