/*
 * chaos_self.h - an access point that attacks itself, for chaos testing with no wires (D82).
 *
 * Why this exists. `tools/chaos.py` injects every fault from the PC side of a USB serial port, and
 * discovers boards by opening every port. Once an AP runs on battery to stop the brownouts (D81) it
 * has no serial cable, so the framework can still *observe* the grid - the BLE status beacon (D68)
 * already carries backbone links, handhelds attached, uptime, lowest heap, reset reason, restarts
 * and brownouts per AP, which is most of what a chaos run measures - but it can no longer *cause*
 * anything. This is the missing half: the AP inflicts the faults on itself, on a schedule the
 * laptop can predict and check.
 *
 * **This is never in a normal build.** NODE_CHAOS_ENABLED is 0 unless a build asks for it, and a
 * build that has it says so loudly at boot. It must not be flashed to an access point anyone
 * depends on: its whole purpose is to break the grid on purpose.
 *
 * What it can do to itself, using the same self-restoring hooks the console offers:
 *   - the ESP-NOW backbone off for a while (lgbb_disable), so anything that still crosses went by LoRa
 *   - LoRa off for a while (lora_disable), to prove nothing stalls without it
 *   - a restart, which is what the reset line used to do from the PC
 *
 * It cannot cut power - nothing on the board can - so a battery run loses the "power out" fault that
 * the USB harness had. That is a real reduction in coverage and is written into docs/chaos-engineering.md.
 *
 * **Bounded so it always stops.** The number of faults injected is counted in NVS, not in RAM, so a
 * restart does not reset the budget: an AP that restarts itself cannot restart its own chaos run.
 * After CHAOS_MAX_FAULTS the AP says so once and never injects again until it is reflashed or the
 * counter is cleared from the console. Every radio outage is self-restoring on top of that.
 *
 * **Deterministic.** Each fault is drawn from (seed, AP index, boot counter), so the schedule is
 * reproducible from the report and two APs never take themselves out at the same moment.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* How many faults one AP will ever inject before it stops for good. Counted in NVS. */
#define CHAOS_MAX_FAULTS   60u

/* The gap between faults, in seconds: drawn uniformly from this range. */
#define CHAOS_GAP_MIN_S    180u
#define CHAOS_GAP_MAX_S    900u

/* How long a radio outage lasts, in seconds. Well under the console hooks' own hour cap. */
#define CHAOS_OUTAGE_MIN_S 30u
#define CHAOS_OUTAGE_MAX_S 300u

/*
 * Starts the self-chaos schedule. Does nothing at all in a build without NODE_CHAOS_ENABLED, so the
 * call site needs no #if around it. `ap_index` and `boot` seed the schedule with the compiled-in
 * seed, so the run is reproducible and the three APs never coincide.
 */
void chaos_self_start(uint16_t ap_index, uint32_t boot);

/*
 * Core task, every pass: injects a fault when one is due. Returns at once, and does nothing in a
 * build without chaos or once the budget is spent.
 */
void chaos_self_poll(uint32_t now_ms);

/* ---- the dead-man window (D83) ----
 *
 * Chaos injects faults only while an armed window is open, and the window is an absolute deadline in
 * grid time. The laptop re-arms it periodically over the admin page; if it stops - because the run
 * ended, the laptop died, or nobody can reach the grid any more - the window closes and the AP stops
 * attacking itself by itself. Silence ends a run rather than sustaining it.
 *
 * The deadline is in NVS and in grid time because a restart is one of the faults: a deadline in RAM
 * would be wiped by the fault most likely to occur, and the AP would come back armed with no way to
 * stop it short of a walk.
 */
#define CHAOS_WINDOW_MAX_MIN   120u   /* one arming can never open the window longer than this */

/*
 * Opens or extends the window, `minutes` from now, capped at CHAOS_WINDOW_MAX_MIN. Returns false when
 * the grid has no time, because without a clock the deadline cannot be enforced and an armed AP could
 * never be trusted to stop itself - refusing is the safe answer.
 */
bool chaos_self_arm(uint32_t minutes);

/* Closes the window now. Chaos stops; the budget is untouched, so a later arming resumes the run. */
void chaos_self_disarm(void);

/* Whether the window is open, and how many seconds of it are left (0 when closed). */
bool     chaos_self_armed_now(void);
uint32_t chaos_self_window_left_s(void);

/* Console `chaos`: what this build will do, what it has done, and what is left. */
void chaos_self_print(void);

/* Console `chaos stop`: no more faults this boot or ever, until the counter is cleared. */
void chaos_self_stop(void);

/* Console `chaos clear`: forget the budget, so a fresh run can start. */
void chaos_self_clear(void);

/* Whether this build can attack itself at all, for the log and the console. */
bool chaos_self_available(void);

/* Faults injected so far, for the report and the console. */
uint32_t chaos_self_count(void);
