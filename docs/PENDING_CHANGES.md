# Firmware changes waiting for the analysis run

The owner's decision, 2026-10-04: analysis first, then every firmware change in one go, as separate
commits, followed by a confirming run. Nothing here is built yet. Tools and harness fixes are not on this
list; they went in first so the analysis could be trusted.

## Sure, from evidence already in hand

1. **An access point advertises room it then refuses.** Its own console says "No heap to serve another
   handheld; refusing connection" while its beacon says "1 free" (Everest, 16 times in three minutes;
   Denali stranded two handhelds for 1 h 46 min on 2026-10-03).
   - **Built (D92), not yet run on the boards:** the beacon takes the cost of arriving off the heap
     before it counts, and is rewritten when the room goes to or from zero.
   - A full access point says so in a reply instead of closing the socket, so the handheld shows "AP full"
     rather than "session receive error".
   - **Built (D92), not yet run on the boards:** a handheld refused by the only access point in range
     waits 7 s, 15 s, 30 s, then 60 s before asking again.
   - **In the build since 2026-10-04 (D93):** NimBLE pools
     halved (9 KB back), one Bluetooth controller connection (2 KB back). AMPDU off and smaller Wi-Fi
     buffers gained nothing; TCP windows are already at their floor.
   - **Measured, behind a switch that is off:** `LG_NODE_GPS_POLLED` gives Everest 4 KB back.
   - Open after the gains: the unchanged room formula admits six handhelds at a 4 KB low-water mark.
   - Recorded as D92.

2. **Screen saver: show the grid time. Built, not yet seen on a screen.** A third line under the handheld's name and its access point,
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
