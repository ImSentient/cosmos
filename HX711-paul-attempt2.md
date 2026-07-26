# HX711S read-on-DRDY rewrite — design, failures, fixes, validation (attempt 2)

Rewritten 2026-07-25/26 from james's `hx711s-new` branch (`413e9e38`).
This doc covers the read-on-DRDY rewrite era. The previous stack's history
(`paul/hx711-reliability`, iterations 1-5, original campaign) lives in
`HX711-paul-attempt1.md`. Audit/background material: `HX711.md`.

## Goals for this version

Make HX711 load-cell probing 100% reliable on the CC1 by building
reliability into the **MCU driver only**, following the datasheet and the
established reference implementations:

1. **Driver-only scope.** No changes to load_cell_probe, probe.py,
   load_cell.py, or any other working kalico code. Everything lives in
   `src/sensor_hx711s.c` (plus its own host decode in
   `klippy/extras/load_cell/hx711s.py`).
2. **Prevent, don't classify.** The previous stack polled free-running ADCs
   and classified the wreckage (quality bits, timing-miss skips, streak
   escalation, recovery FSM). This version makes the wreckage structurally
   rare: read on DRDY, verify, retry — so recoverable faults never reach
   the host at all.
3. **Follow the references.** Every reference driver waits for DRDY and
   re-reads rather than consume a raced frame: Prusa HX717
   (`src/common/hx717.cpp`), Linux IIO (`drivers/iio/adc/hx711.c`), upstream
   Klipper (`src/sensor_hx71x.c`), and Elegoo's stock firmware (bounded
   ready-poll + protected transfer). The HX711F datasheet supplies the
   recovery primitives (SCK >60us = power down; 4 settling conversions at
   80 SPS).
4. **Every commit justified with citations** (datasheet sections or named
   implementations), and every fix validated against captured hardware data
   before flashing.

## Failure taxonomy (what the wire actually does)

Each class was observed and captured on the CC1 test printer (carbon2u):

| Class | Signature | Frequency | Old handling |
|---|---|---|---|
| Torn read | Chip latches a conversion mid-read (~50us window / 11.6ms period) | ~0.4% of frames | quality bit, host skips |
| Big glitch | Protocol-valid frame, impossible value (267907 -> -1 on ch2) | ~1/800 frames | per-channel spike filter (950g) |
| Impulse | Single-sample blip, 87-220g, instantly reverts (move-start accel kicks, travel ring) | ~1/40 taps | **NOTHING** — fired phantom triggers |
| Desync | Gain-pulse count mismatch (serial protocol state corrupt) | rare | host force-restart (kills probe) |
| Stuck ADC | No DRDY for ~2.5 periods | never observed | — |

The killer was the **impulse class**. Two manifestations captured:

- **Phantom trigger** (recorded tap curve, 40+ taps): a ~220g single-sample
  blip at probing-move start fired the first-crossing trigger 1.2mm above
  real contact. The tap window then contained no contact ramp and the tap
  validator aborted with `TAP_CHRONOLOGY`.
- **Pre-movement trigger** (182k-sample raw stream capture): an isolated
  +87g single-sample blip, 0.5s after travel vibration settled, landed in
  the arming window -> `Probe triggered prior to movement`, macro abort.
  Same class, smaller amplitude — under the first fix's 190g threshold but
  over the 75g trigger force.

Real contact at 2mm/s rises by at most ~80g/sample **sustained**; impulses
are bigger than the trigger force for exactly **one sample**. That gap is
what the fix exploits.

## The fixes (3 commits, driver-only)

### `9efa8c34` — in-driver recovery (read-on-DRDY)
- Torn reads: emit nothing, re-read on the next DRDY, bounded 2 retries;
  only then degrade to the host's benign hold-last-value path.
- Desync: in-driver power cycle (datasheet power-down control), discard
  the 4 settling conversions, resume after ~115ms. Host gets one RECOVERED
  marker frame (torn-retry + recovery tallies) instead of a fatal error.
  Previously the host force-restarted the whole sensor, killing any active
  probe/tare session.
- Stuck ADC: same power-cycle path.

### `b9a658cb` — impulse hold-confirm on the trigger feed
- Any single-sample jump on the channel sum over threshold is held for one
  sample and only forwarded to the probe trigger if the next sample
  confirms it as a sustained step (same hold-confirm pattern as james's
  per-channel glitch filter, applied to the sum). Raw stream untouched —
  impulses still reach the host for diagnostics.

### `dd612373` — threshold under trigger_force + sign-aware confirm
- The 190g threshold missed the 87g end of the impulse class. Threshold now
  67g (just under trigger_force 75g): any blip big enough to trigger is
  held.
- Confirm check made sign-aware: an up-jump confirms when the next sample
  stays above (pending - threshold). The original symmetric
  `|current - pending| <= threshold` check could never confirm a monotonic
  ramp (each sample is more than threshold above the previous) — a real
  logic bug caught during the fix.

## How we tested

1. **Tap recorder** (`[tap_recorder]` + `TAP_RECORDER_START`): JSONL of
   every tap's force curve + move queue, valid and invalid. Caught the
   phantom trigger's flat-window signature.
2. **Raw force capture** (`load_cell/dump_force` mux over klippy's unix
   socket -> JSONL): full 80 SPS stream during real macro runs. Caught the
   +87g pre-movement blip with exact timing context. (`/tmp/force_dump.py`
   in this repo's sibling notes; sensor name for the mux is
   `load_cell_probe`. **Gotcha:** /tmp on carbon2u is tmpfs — redeploy the
   script after every flash/reboot.)
3. **Offline replay validation**: before flashing `dd612373`, the full
   182k-sample capture was replayed through a Python model of the new
   filter: all 72 phantom crossings (1-sample blips, 60-220g) removed;
   all 433 real contact ramps still trigger within latency bounds.
4. **Campaign loop** (`hx711-mesh-loop.sh`): full
   `BED_MESH_CALIBRATE BED_TEMP=60` runs back-to-back, stop on first
   failure, heaters killed on failure.

### Results (carbon2u, `dd612373`)

| Campaign | Result |
|---|---|
| Round 1 @60C (b9a658cb) | FAIL attempt 1 — pre-movement trigger |
| Round 2 @60C (b9a658cb + capture) | FAIL attempt 5 — same, captured |
| Round 3 @60C (dd612373) | **10/10 PASS** (~417s each) |
| Hot soak (dd612373) | **20/20 PASS @100C** (~2.5h, chamber ~60C) |

Across the 30 passing runs: 0 in-driver recoveries (armed, never needed),
0 bad taps in the hot soak, glitch filter drops logged and benign. This
matches the previous stack's validated record (10x@60C + 20x@100C) with a
driver-only diff: 3 commits in `sensor_hx711s.c` + decode in `hx711s.py`,
replacing a 10-commit stack that spanned the driver, the load-cell
probing framework, and the generic probe retry path.

## Branch / build notes

- Branch `paul/hx711s-new` (tracks `origin/hx711s-new`), head `dd612373`.
- Built via cosmos-nightly `rc.build.cc1.emmc` (inc: branch + SRCREV),
  flashed with services stopped (klipper/moonraker/grumpyscreen/ustreamer),
  verified by `Loaded MCU ... Kalico dd612373` x3.
- The previous validated stack remains at `paul/hx711-reliability`
  (`c368f84e`) if a fallback is needed.

## 2026-07-26 (evening) — tunable constants, gilfoyle audit, scheduler fix

**Branch:** `paul/hx711s-new3` (local worktree `~/carbon/kalico-hx711s-new3`,
base = james's `hx711s-new2` @ `581e692f`, unpushed). Same 4 reliability
layers, now with the constants tunable from printer.cfg via one
`hx711s_set_tuning` MCU command:

| Option | Default | Commit |
|---|---|---|
| `torn_retries` | 2 | torn-frame verify |
| `stuck_timeout_ms` | 0 = derive from sample_rate (~2.5 periods) | watchdog |
| `impulse_threshold_counts` | 5775 (~55g @ 105 c/g) | impulse hold |

Gilfoyle audit findings, all fixed via rebase-edit amends:
- **Scheduler corruption (critical):** `hx711s_recover()` moved timer
  waketimes with an in-place write while the timers were live in the
  sorted schedule list. Fixed with `sched_del_timer`/`sched_add_timer`.
  Never fired in validation (0 recoveries) — would have detonated on the
  first real desync.
- **PR #6 carried the same class of bug** in `start_recovery()`, but
  only at the *desync* (task-context) call site; the stuck
  (event-context) site was safe because the timer is popped during the
  callback and reinserted by `SF_RESCHEDULE`. Naively adding del/add
  inside `start_recovery` would have double-inserted. Correct fix: wrap
  the task-context call site. Amended into the recovery commit
  (`9a4543ce`), branch force-pushed (`dd612373..bfc8cc68`), PR #6
  updated.
- **Watchdog, G1:** 35ms timeout was 80-SPS-only → rate-aware
  (`2500//sps + 5`ms, 10 SPS boards safe).
- **Watchdog, G2:** primary-only liveness → per-chip; a wedged secondary
  silently fed stale counts into the sum before.
- **Recovery, G3:** held-sum keeps the probe watchdog fed across the
  post-settle requalify window.
- **Docs:** Config_Reference documents all 3 options + the
  `impulse_threshold_counts < trigger_force x counts_per_gram` rule.

Docs updated; no push (new3 stays local until Paul says otherwise).
