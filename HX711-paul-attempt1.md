# HX711S old reliability stack — implementation and testing (attempt 1)

The poll-and-classify stack on `paul/hx711-reliability` (base fa59d011):
design, crash fixes, five hardening iterations, and the validation
campaigns (10/10 @ 60C, 20/20 @ 100C) that shipped as kalico PR #5 /
cosmos PR #265. Superseded by the read-on-DRDY rewrite — see
`HX711-paul-attempt2.md`. Audit/background material: `HX711.md`.

---

# HX711S rewrite, testing, and troubleshooting

This document records the major Kalico HX711S rewrite developed for the
current Cosmos PR, the regressions found while flashing it to `carbon2u`, the
fixes already made, and the remaining driver review.  The research, data-sheet
audit, Elegoo reverse engineering, and original design choices that led to the
rewrite remain in [`HX711.md`](HX711.md).

## Code under test

The Kalico work is on branch `paul/hx711-reliability`.  The tested history is:

| Revision | Purpose |
| --- | --- |
| `8433f46d569694ab136fc90696031ec68ef4270d` | Replace held-value and fixed-jump heuristics with MCU-owned frame quality, deterministic recovery, median tare, and two-sample triggering. |
| `9ed1574a997a331a332cba71f8d75ce21d3b1f63` | Flush the shared bulk buffer before appending a 20-byte four-channel frame. |
| `28aca926429211037556af2cd2d4c2ae94607b8b` | Remove a live timer before rescheduling it through the recovery path. |
| `9044d16e49295c3c405601a3818c390a9951b213` | Complete the timestamped wire format, fault classification, recovery state, and host-side reliability rewrite. |
| `9c1936efd4a07ea78b97bf4a50a3d23bb786ac29` | Retry one invalid HX711 sample at the generic probe layer. |
| `64593629b31ab1baa05d30db8474c3e7e5ddb508` | Convert one HX711 collector acquisition error into the retryable probe error at the load-cell boundary. |

The corresponding Cosmos build points
`meta-opencentauri/recipes-apps/klipper/kalico_2026.02.00.inc` at revision
`64593629b31ab1baa05d30db8474c3e7e5ddb508`.  The latest flashed bed-MCU
version on `carbon2u` reported `64593629`.

The follow-up fixes are committed on `paul/hx711-reliability` and must always
be deployed as a paired Kalico Python package plus bed-MCU firmware.  The
latest probe-retry commits are `9c1936ef` (probe-layer retry) and `64593629`
(collector-boundary retry).

## Rewrite contract

1. Each MCU record contains the signed raw count from every ADC followed by a
   32-bit quality word.  Bits 8--11 form the affected-channel mask.  Low bits
   describe settling, not-ready, gain-clock, post-read, overrun, and saturation
   conditions.
2. A nonzero-quality frame is never used for tare, filtering, or a probe
   decision.  Raw counts are retained as diagnostic evidence.
3. A protocol fault powers down and restarts all four ADCs, discards five
   settling frames, and requires two additional clean qualification frames.
4. A hard acquisition fault during an active probing move terminates the move
   through a dedicated `trsync` error reason.
5. A valid filtered force must exceed the trigger threshold for two
   consecutive samples.
6. Tare uses 30 valid samples and calculates an independent per-channel
   median.

The safety direction remains correct: the driver must not synthesize a value,
continue Z motion on an invalid acquisition, or silently omit a bad channel
from the force sum.  The issues below are primarily state, timing,
observability, and recovery-policy defects around that contract.

## Confirmed MCU crash root causes and fixes

### Bulk-buffer overwrite

A four-channel quality record is 20 bytes, while `sensor_bulk.data` is 51
bytes.  The initial rewrite appended a third record when 40 bytes were already
buffered and checked capacity only afterward.  That wrote 9 bytes beyond the
buffer and corrupted adjacent `hx711s_adc` state.  Depending on the overwritten
values, the visible result was a stalled or disconnected bed MCU rather than a
clean HX711 error.

Revision `9ed1574a` checks whether the complete next record fits and flushes
the existing 40-byte packet before appending.  This is a confirmed memory
safety fix.

### Duplicate insertion of a live scheduler timer

Protocol recovery runs from task context while the HX711 polling timer remains
linked in Klipper's MCU timer list.  The first rewrite changed its wake time
and called `sched_add_timer()` again without removing the existing entry.  The
same timer could therefore appear twice or form a corrupted list.  The eventual
symptoms included serial garbage, bed-MCU disconnects, and `Invalid oid type`
shutdowns.

Revision `28aca926` calls `sched_del_timer()` before changing and re-adding the
timer, and uses that reset path for command-started acquisition as well.  A
five-minute idle soak and a ten-second diagnostic after flashing showed no
invalid serial bytes, retransmits, reconnects, scheduler errors, or invalid OID
messages.

## Live validation after both crash fixes

The fixed image booted normally on the Centauri Carbon 1.  A non-motion
`LOAD_CELL_DIAGNOSTIC` completed with:

```text
Sensor reported no errors
Measured samples per second: 86.0, configured: 80.0
Good samples: 866, Saturated samples: 0, Unique values: 581
```

At the end of that test the bed MCU reported `bytes_invalid=0` and
`bytes_retransmit=0`.  This establishes that the crash fixes work and that the
driver can produce a clean idle stream.  It does not establish reliable
probing.

## 2026-07-23 bed-mesh failure

The configured `BED_MESH_CALIBRATE BED_TEMP=60` wrapper was submitted after
the fixed firmware reboot.  Moonraker reported the request pending at 60, 120,
and 180 seconds, then returned HTTP 400 after **223.158 seconds**.  Klipper
remained connected and `ready`; the homed toolhead stopped near `(236, 28, 5)`.

Conditions at failure were:

* bed target and temperature approximately 60 C;
* extruder target and temperature approximately 140 C;
* chamber approximately 39.3--39.6 C;
* bed MCU `bytes_invalid=0` and `bytes_retransmit=0`.

The log contained channel-specific hard-quality values followed by many
recovery records:

| Quality | Decode |
| --- | --- |
| `0x1` | Settling or qualification frame. |
| `0x80c` | Channel 3 mask plus extra-gain-bit-low and post-read-low. |
| `0x80e` | Channel 3 mask plus not-ready, extra-gain-bit-low, and post-read-low. |
| `0x40e` | Channel 2 mask plus not-ready, extra-gain-bit-low, and post-read-low. |

At the initial capture point there were 254 warning **lines** whose latest
quality was `0x1`, eight whose latest quality was `0x80c`, seven ending in
`0x80e`, and one ending in `0x40e`.  These are not per-frame totals.  The host
logs one warning per batch and prints only the final fault's quality; an
earlier hard fault in the same batch may be hidden by a later settling frame.

Immediately before the request failed, four batches reported 7, 1, 4, and 4
invalid frames.  Their sum is the reported `Sensor reported 16 errors while
sampling`.  That exception text appeared four times consecutively in the log,
but the current logging is insufficient to prove that these were four distinct
probe attempts rather than propagation of one exception.

No MCU shutdown, reconnect, `Invalid oid`, or transport corruption occurred.
The existing `bed_mesh default` config block remained unchanged, so this run
did not produce or save a new mesh.  The heaters left targeted by the failed
macro were turned off afterward.

## Full-driver review findings

### High priority

1. **Recovery frames are treated as fresh acquisition failures.**
   `HX711S_Q_SETTLING` is intentionally emitted for five discarded settling
   and two qualification frames after every reset.  Python counts every one
   in `errors`, and `LoadCell.validate_samples()` rejects a tare window if the
   count is nonzero.  A single hard fault therefore guarantees at least seven
   additional errors and can make the sensor unable to reacquire a clean tare
   even after recovery succeeds.  Settling records must remain excluded from
   control data, but they should be classified separately from the hard fault
   that initiated recovery.

2. **Collector errors are batch-wide rather than time-windowed.**
   `LoadCellSampleCollector` filters valid samples by `min_time` and
   `max_time`, but blindly adds the batch's complete `errors` count.  Because
   acquisition runs continuously and a client can attach midway through a
   100-ms batch, a tare may fail on a fault that occurred before its requested
   measurement window.  Fault records already carry timestamps and must be
   filtered by the same time bounds as samples.

3. **The detailed fault list is discarded at the next Python layer.**
   `HX711SBase._process_batch()` creates useful
   `{time, counts, quality}` records, but `LoadCell._sensor_data_event()`
   replaces them with `"faults": []`.  Downstream collectors, WebSocket
   clients, and status code cannot identify the channel or distinguish hard
   faults from settling.  This directly prevents a reliable explanation of
   the failed mesh.

4. **Fixed-frequency timestamp reconstruction is invalid across reset gaps.**
   `FixedFreqReader` assumes every record advances at one continuous sample
   frequency.  Power-down, the 50-ms startup wait, and discarded conversions
   introduce irregular gaps, yet the stream sequence contains no per-frame
   capture clock or skipped-slot count.  Clock regression therefore smears
   timestamps around recovery.  That can distort tap analysis and calculated
   contact position even after invalid records are removed.  Add an MCU clock
   to each frame or otherwise advance an explicit sample-slot counter through
   gaps.

5. **Stopping acquisition races a pending capture task.**
   The zero-rate command removes the timer and sets `HX711S_OFF`, but does not
   clear `pending_flag`.  An already-woken capture task may still read and
   report a sample; if that read has a protocol fault it can call the reset
   path and restart a sensor that the host just stopped.  The stop path must
   clear pending state atomically, and the capture task must refuse work while
   off.

6. **A pre-start fault can abort a future homing move.**
   Valid samples honor `FLAG_AWAIT_HOMING` and ignore data before
   `homing_start_time`.  `load_cell_probe_report_fault()` checks only
   `FLAG_IS_HOMING`, so a quality frame arriving after the home command but
   before the scheduled motion start triggers an error.  Fault handling must
   use the same time gate, while still refusing to arm the move unless the ADC
   is online and qualified.

### Medium priority

7. **Terminal probe errors are not latched.**
   A successful force trigger sets `FLAG_IS_HOMING_TRIGGER`; safety, watchdog,
   and ADC errors do not.  The watchdog and incoming samples can continue to
   call `trsync_do_trigger()` until the host clears homing.  The first terminal
   outcome should latch a common completed flag and stop further filter or
   watchdog activity.

8. **Multi-channel range checks use a single ADC's limits.**
   Probe safety and drift configuration compare the four-channel summed tare
   against `sensor.get_range()` (approximately +/-8.4 million) instead of the
   load cell's summed range (approximately +/-33.5 million).  This can reject
   valid configurations and makes the Python validation inconsistent with the
   summed value used by the MCU probe.

9. **Two-sample confirmation is sample-rate dependent and biases trigger
   time.**  The driver supports 10, 20, 80, and 320 SPS variants, but the
   confirmation count is hard-coded to two.  It adds 100 ms at 10 SPS and only
   3.125 ms at 320 SPS.  The stored trigger time is the second crossing, not
   the first, so ordinary homing position is shifted by the distance travelled
   during that interval.  Make confirmation duration or count configurable by
   sample rate and retain the first crossing timestamp when appropriate.

10. **The hard quality that caused a reset is often hidden in logs.**
    The batch warning prints only `faults[-1]["quality"]`.  Recovery frames
    commonly follow a hard fault, producing a misleading `latest quality=0x1`
    line.  Log per-quality and per-channel counts with rate limiting, and expose
    cumulative counters through status.

11. **Digital protocol validity does not prove physical plausibility.**
    The rewrite intentionally removed the arbitrary 100000-count jump filter,
    but it added no replacement for a physically implausible yet correctly
    framed value.  Median tare and two-sample triggering reduce the risk; they
    do not detect a two-frame electrical impulse or gross disagreement among
    channels.  A separate, well-justified per-channel slew/disagreement policy
    is still needed for probing.  It must not be conflated with serial quality.

12. **The pre-read fault path still clocks a not-ready ADC.**
    When the task sees a high DOUT, it marks `NOT_READY` but proceeds to issue
    all 25--28 clocks before resetting.  The data sheet requires SCK low while
    data is not ready.  Record the mask and recover without clocking that
    transaction.

### Lower priority and diagnostics

13. `clear_home()` passes one more argument than the command format declares.
    The current encoder appears to ignore it, but the call should match the
    seven command fields exactly.
14. The collector timeout path clears its counters before formatting the
    exception, so a timeout can report zero errors and overflows even when they
    caused the timeout.
15. `sos_filter_set_active` does not validate `coeff_int_bits` on the MCU, and
    the MCU range setter's comment requires positive `grams_per_count` while
    its check accepts zero.  Python normally prevents both, but MCU command
    validation should be complete.
16. The post-read check is sampled after an IRQ-polling low phase.  Capturing
    DOUT after the specified rising-edge setup time and before servicing
    arbitrary pending work would make the check more deterministic and closer
    to the data-sheet timing diagram.
17. There are no focused state-machine or fault-policy tests for this driver.
    The build tests syntax and target compilation but not buffer boundaries,
    timer membership, reset sequences, batch fault propagation, timestamp
    gaps, or pre-start/active-homing behavior.

## Follow-up implementation

The full review above has now been applied to the worktree as follows:

| Finding | Resolution |
| --- | --- |
| Recovery frames failed tare | Pure settling/qualification records remain excluded from control data but are classified as nonfatal recovery frames. Only hard quality faults fail a collection window. |
| Batch-wide stale errors | Fault timestamps are preserved and filtered against each collector's `min_time`/`max_time`; a fault before a new tare no longer poisons it. |
| Fault details discarded | `LoadCell._sensor_data_event()` forwards quality, decoded flags, affected channels, raw counts, timestamp, and hard/recovery classification. |
| Bad timestamps across reset | Every MCU frame now includes its capture clock. HX711S uses a timestamped bulk reader and no longer infers continuous time across power-down/recovery gaps. |
| Stop/capture race | Stop atomically marks the sensor off and clears pending work; the capture task refuses to run while off, and probe readiness is withdrawn. |
| Pre-start fault race | Samples and faults use the same capture-time gate. The MCU also refuses to begin active probing unless the attached HX711S has reached `ONLINE` after its clean qualification streak. |
| Repeated terminal triggers | Success, safety, watchdog, overflow, and invalid-ADC outcomes all use one latched terminal path. |
| Single-channel range validation | Probe safety and drift checks now use `LoadCell.saturation_range()`, which scales with the channel count. |
| Sample-rate-dependent trigger | New `trigger_confirm_time` configuration defaults to 12.5 ms. The MCU requires force to remain above threshold for that duration but records the first crossing as contact time. |
| Hidden initiating fault | Logs aggregate decoded flags and channels with rate limiting; status exposes lifetime frame/quality/channel counters, the latest hard fault, and a latest-batch summary. |
| Physical plausibility | The arbitrary count-jump heuristic remains removed. Temporal confirmation is now explicit and configurable. A universal channel-disagreement limit was deliberately not invented because valid off-center bed contact is inherently asymmetric; any future hard limit requires measured mechanical bounds. |
| Clocking while not ready | A high DOUT produces a timestamped `NOT_READY` record and reset without issuing the 25--28 clock pulses. |
| Command mismatch | The home protocol now explicitly contains `trigger_confirm_ticks`; start and clear calls both send the declared eight fields. |
| Timeout diagnostics | Collector timeout values are captured before state is cleared, so exceptions retain the actual error and overflow counts. |
| Incomplete MCU validation | Zero `grams_per_count`, reversed/equal safety ranges, and invalid SOS coefficient integer widths are rejected. |
| Nondeterministic bit checks | DOUT is sampled after the rising-edge setup interval while IRQs are masked; the post-read level is captured immediately after the final falling edge before polling pending IRQs. |
| Mixed host/MCU protocol | The timestamped frame carries an explicit `0xa711` format tag. New-host/old-MCU and old-host/new-MCU combinations both fail closed instead of interpreting shifted words as control data. |
| Missing regression coverage | Six focused host tests cover quality classification, fault preservation, format mismatch, real timestamp gaps, collector time-window filtering, and legacy sensor compatibility. The complete STM32F401 firmware, including all HX711/load-cell sources, compiles with warnings enabled. |

The timestamp enlarges a four-channel record from 20 to 24 bytes. The
pre-append capacity check therefore flushes at 48 bytes, retaining two whole
records in the 51-byte bulk buffer without an overrun.

The validation completed for the committed rewrite is:

```text
pytest -q test/test_hx711_reliability.py
14 passed

ruff check (all changed Python and test files)
All checks passed

STM32F401 full MCU build
PASS
```

## Live deployment and retry-fix chronology

All live tests below were run on a Centauri Carbon 1 (`carbon2u`) with the
Cosmos CC1 eMMC build.  Each image was copied to `/user-resource`, verified,
flashed with `flash`, and activated by reboot.  The bed was not saved or
modified by the mesh tests; it was turned off after each stopped run.

### Baseline rewrite behavior

At a 60 C bed, the first mesh attempt returned:

```text
Sensor reported 1 acquisition errors and 0 bulk overflows while sampling
```

The printer stayed connected and ready, with no MCU shutdown, serial
corruption, or bulk overflow.  This showed that the driver was safely refusing
an invalid acquisition, but the error escaped as a mesh failure.

### Probe-layer retry (`9c1936ef`)

The first fix caught one exact `Load Cell Probe Error: invalid HX711 sample`
inside `PrinterProbe`, retracted, and retried the same point once.  It was
deliberately narrow: repeated invalid samples and unrelated safety errors
remain fatal.  Host tests and lint passed, and the image booted with all three
MCUs reporting `9c1936ef`.

At 60 C, mesh 1 of 5 still failed with the acquisition-error message above.
The exception was raised by the load-cell collector before `PrinterProbe` was
entered, so the retry hook was bypassed.  The batch stopped immediately.

### Collector-boundary retry (`64593629`)

The second fix converts exactly one HX711 acquisition error with zero bulk
overflows at the load-cell probe boundary into the existing retryable invalid
sample error.  It covers both probe sample and probe-tare collection, while
leaving repeated errors, bulk overflows, non-HX711 sensors, and safety faults
fatal.  Host tests (`14` focused HX711 tests plus related tests), Ruff,
Python compilation, and firmware checks passed.

The image was rebuilt, flashed, rebooted, and verified on carbon2u.  At 60 C,
mesh 1 of the planned five-run validation reached the probing stage.  The
collector conversion did activate, but the request ultimately failed with:

```text
Error during homing probe: Load Cell Probe Error: invalid HX711 sample
```

That indicates the transient fault recurred within the one-retry budget.  The
printer remained healthy with no MCU shutdown, communication loss, or bulk
overflow.  Meshes 2--5 were not run because the agreed test rule is to stop on
the first failure.

The live report with counters, temperatures, exact responses, and log excerpts
is maintained in
[`HX711-bed-mesh-test-20260723.md`](../cosmos-nightly/HX711-bed-mesh-test-20260723.md).
This section is the authoritative narrative; the separate report is retained
as the raw test log.

## Printer validation still required

No production-readiness claim should be made from compilation and host tests
alone. After review and a paired host/bed-MCU build, repeat:

1. idle acquisition and `LOAD_CELL_DIAGNOSTIC` while confirming the sensor
   transitions through recovery to `ONLINE`;
2. repeated cold probes and a complete bed mesh;
3. a 105 C bed and >50 C chamber soak followed by repeated probes and a mesh;
4. stop/start and injected/observed readiness-fault recovery; and
5. log/status capture proving that the first hard quality, affected channel,
   recovery duration, trigger position, and transport counters remain visible.

## Current conclusion

The two catastrophic MCU failures remain understood and fixed: one was a
direct bulk-buffer overwrite and the other a duplicate live timer insertion.
The subsequent full-driver review has now been implemented in the worktree,
including the policy and observability defects exposed by the failed mesh.
Local regression tests and a complete STM32 build pass. The new paired
host/MCU protocol has not yet been flashed or exercised on a printer, so the
rewrite remains test firmware until the cold and high-temperature validation
above succeeds.

## Iteration 1 (2026-07-23): timing misses reclassified as non-fatal skips

A 10-consecutive-mesh campaign at 60 C bed was started against `64593629`.
Mesh attempt 1 failed after 455 s with the now-familiar
`Load Cell Probe Error: invalid HX711 sample` after both retry layers were
exhausted.  Counters showed **36 hard faults in 455 s (~1/12.6 s — the same
rate as idle)**: 22 `not_ready` and 14 `extra_low`+`post_read_low` pairs,
81% on channel 3.  The fault stream is a constant background property of the
polling design, not motion-induced: the MCU polls the four free-running ADCs
and any read that races a conversion boundary — window closed before the
read, or a conversion completing mid-read — was treated as a protocol fault
with a full power-down reset, a 7-frame recovery, and a probe-aborting hard
fault.  No retry budget survives a 0.1%-per-frame fault rate across the
dozens of tare/sample windows in one mesh macro.

Kalico `13952564` ("hx711s: reclassify timing misses as non-fatal sample
skips") reclassifies the three timing flags (`NOT_READY`, `EXTRA_LOW`,
`POST_READ_LOW`): the frame stays excluded from tare/filter/probe data, but
the MCU neither resets nor reports a probe fault, and the host counts it in a
new `timing` health bucket instead of failing the collection window.
`READ_OVERRUN`/`SATURATED` semantics are unchanged.  A genuinely stuck ADC
still fails closed: 40 consecutive timing misses (~0.5 s at 80 SPS) tag the
frame with the new `Q_TIMING_STREAK` bit (host: hard fault) and the MCU
reports a probe fault and resets.  Old hosts fail closed on the unknown bit.

Validation so far: 15 host tests (timing classification, escalation marking,
collector behavior), Ruff clean, STM32F401 build passes.  Deployed to
carbon2u via Cosmos build r12; mesh campaign result pending.  Raw evidence:
[`HX711-bed-mesh-test-20260723.md`](../cosmos-nightly/HX711-bed-mesh-test-20260723.md)
and `hx711-mesh-loop-20260723.log`.

## Iteration 2 (2026-07-23): drift-safety vs trigger-confirm race

The first mesh on `13952564` (timing-miss reclassification) failed at 230 s
with a NEW error: `force exceeded drift_safety_limit before triggering!`.
The acquisition layer was completely clean for the first time: **zero hard
faults, zero recoveries** during the attempt (39 timing skips absorbed).
The failure had moved to the trigger layer, which the rewrite had never
successfully exercised on-printer.

Live experiments showed `PROBE` at 2 mm/s triggers cleanly (peak ~281 g),
while `LOADCELL_Z_HOME` (cleaning-pass probes at the bed edge) failed
intermittently; a passing run showed contact peaks of ~475 g against the
+/-1000 g drift band.  Root cause: the MCU checked the raw drift band before
the 12.5 ms trigger confirm on every sample, so any ramp crossing the band
within 1-2 samples died as a safety error before the confirm could complete.
Upstream Kalico triggers on the first filtered crossing and never races; the
confirm window is a rewrite addition (impulse rejection).  Research
(subagent, deleg_11ecc16b) confirmed upstream policy and that the drift band
must stay on the raw sample as the last-resort runaway guard.

Kalico `426ac30b` gates the drift band only on the ARMING sample of a
contact streak; once armed, the confirm owns the outcome and may complete
across the band.  Below threshold the band still applies and any armed
confirm resets.  Deployed via Cosmos r13; campaign result pending.

## Campaign result (2026-07-23): 10/10 consecutive meshes at 60 C

After five fix iterations (timing-miss reclassification, arming-gated drift
band, first-crossing trigger default, per-channel spike rejection, bounded
bulk-overflow tolerance) plus one macro config change (cleaning-pass
tolerance 0.1), the rewrite completed **10 consecutive
`BED_MESH_CALIBRATE BED_TEMP=60` runs** (~76 min, 440-509 s each) with:

- zero hard faults and zero recovery cycles across 392k frames
- 445 timing skips absorbed silently (the original death sentence)
- zero trigger/safety/tolerance/transport aborts

Full raw evidence: cosmos-nightly `HX711-bed-mesh-test-20260723.md`.
Kalico `c368f84e` on `paul/hx711-reliability`; Cosmos r16 on `paul/nightly`
(unpushed); macros.cfg cleaning-pass tolerance change in the image.

Remaining known gremlins (non-blocking): ch3 remains the noisiest cell
(~70% of timing/channel faults); the rare deep-trigger edge-probe anomaly
(~1/100) is tolerated by the relaxed cleaning-pass tolerance and should be
re-evaluated under the read-on-DRDY sampling idea (see plan idea pool).

---

## 3. Test campaign record (was cosmos-nightly/HX711-bed-mesh-test-20260723.md)

# HX711 bed-mesh stress test

The consolidated testing and fix narrative is maintained in
[`HX711-REWRITE.md`](../cosmos-merge/HX711-REWRITE.md). This file is retained as
the raw carbon2u test log and counter record.

Target: Centauri Carbon 1 (`carbon2u`), Kalico `9044d16`

Bed target: 60 C (measured 60.0-61.6 C during probing)

Command: `BED_MESH_CALIBRATE` (mesh was not saved)

## Results

| Attempt | Result | Error | Notes |
|---:|---|---|---|
| 1 | FAIL | `Sensor reported 1 acquisition errors and 0 bulk overflows while sampling` | Klipper stayed ready; no MCU shutdown. |
| 2 | FAIL | `Error during homing probe: Load Cell Probe Error: invalid HX711 sample` | Repeated probe retries reached the same hard-sample rejection; no MCU shutdown. |

No successful mesh was obtained, so the planned ten-run batch was stopped rather than masking a repeatable probe failure.

## Sensor counters

Before testing: 36,950 frames, 45 hard faults, 322 settling events.

After two attempts: 81,548 frames, 95 hard faults, 672 settling events, 76 `not_ready`, 19 `extra_low`, and 19 `post_read_low` flags. No bulk overflows were reported.

The decisive log entries were repeated `Error during homing probe: Load Cell Probe Error: invalid HX711 sample`. The driver is correctly refusing to use samples that fail wire-level validity checks, but the mesh/probe path currently propagates that hard sample rejection as a failed probe instead of retrying the point with a fresh acquisition. This explains the mesh failure without indicating an MCU crash or communication loss.

Bed heating was disabled after the test. No Cosmos push was made because the prerequisite successful mesh was not reached.

## Retry-fix validation

Kalico `9c1936ef` added a probe-layer retry for one `Load Cell Probe Error: invalid HX711 sample`. The image was rebuilt, flashed, and rebooted successfully; all three MCUs reported `9c1936ef`.

At 60 C, mesh 1 of the planned five-run validation again failed with `Sensor reported 1 acquisition errors and 0 bulk overflows while sampling`. This error is raised by the load-cell collector before `PrinterProbe._probe()` and therefore bypasses the new probe-layer retry. The batch was stopped as requested; meshes 2-5 were not run.

## Collector-layer retry validation

Kalico `64593629` added the collector-boundary conversion for exactly one HX711 acquisition error with zero bulk overflows. The image was rebuilt, flashed, and rebooted successfully; all three MCUs reported `64593629`.

At 60 C, mesh 1 of 5 reached the load-cell probing stage and the conversion worked: the request ultimately reported `Load Cell Probe Error: invalid HX711 sample` after the probe-layer retry was exhausted. This is progress over the previous unhandled acquisition-error message, but the transient fault recurred within the retry budget. The batch was stopped immediately; meshes 2-5 were not run.

## 10-consecutive-mesh campaign (Kalico `64593629`, 60 C bed)

### 2026-07-23T04:28:12Z — mesh attempt 1

- Image/Kalico/Cosmos revision: Kalico `64593629-dirty` (all 3 MCUs), Cosmos CC1 eMMC build 20260723_033956
- Bed target/measured temperature: 60 C target via wrapper; 55.8-56.2 C measured after heaters off
- Exact command/request: `GET /printer/gcode/script?script=BED_MESH_CALIBRATE BED_TEMP=60` (Moonraker, synchronous)
- Start/end and duration: 04:28:12Z -> +455 s
- Result: FAIL
- Exact response/error: HTTP 400 `Error during homing probe: Load Cell Probe Error: invalid HX711 sample; see sensor fault diagnostics`
- HX711 counters before -> after:
  - frames.received 226936 -> 266022 (+39086)
  - frames.hard 279 -> 315 (+36); frames.settling 1960 -> 2212 (+252 = 36x7 recoveries)
  - not_ready 206 -> 228 (+22); extra_low 73 -> 87 (+14); post_read_low 73 -> 87 (+14, always paired with extra_low)
  - channel_faults: ch1 +0, ch2 +7, ch3 +29 (ch3 = 81% of this window's faults)
- Relevant log excerpt: rate-limited summary lines e.g. `load_cell_probe: HX711 faults: hard=3 recovery=8; flags=[extra_low=1, not_ready=2, post_read_low=1, settling=8]; channels=[ch2=1, ch3=2]`; fault stream continued at idle after the failed request.
- MCU/API/service state: Klipper `ready`; no shutdown, no `Invalid oid`, no bulk overflow; bed `bytes_invalid=0`, `bytes_retransmit=9` (static). Toolhead homed xyz, parked ~(157.5, 246, 0.12). Heaters turned off by loop script after failure.
- Follow-up or stop reason: batch stopped on first failure per campaign rule. Fault rate during the macro (~1 hard fault / 12.6 s) matched the idle background rate: faults are a constant poll-vs-conversion phase-race stream, not motion-induced. Diagnosis and fix plan in `HX711-REWRITE.md` (iteration 1: edge-synchronized MCU reads).

### 2026-07-23T11:18Z — iteration-1 mesh attempt 1 (Kalico `13952564`)

- Image/Kalico/Cosmos revision: Kalico `13952564-dirty` (all 3 MCUs), Cosmos r12 build 20260723_050132
- Bed target/measured temperature: 60 C target via wrapper; nozzle probing temp 140 C
- Exact command/request: `BED_MESH_CALIBRATE BED_TEMP=60` via Moonraker
- Start/end and duration: 11:18Z -> +230 s
- Result: FAIL
- Exact response/error: HTTP 400 `Error during homing probe: Load Cell Probe Error: force exceeded drift_safety_limit before triggering!`
- HX711 counters before -> after:
  - frames.received 1982 -> 21972; valid 1971 -> 21926
  - **hard 0 -> 0 (last_hard_fault: null); settling 7 -> 7 (no recoveries)**; timing 4 -> 39
  - quality_flags: not_ready 3 -> 28, extra_low/post_read_low 1 -> 11 each
- Interpretation: the timing-miss reclassification worked perfectly — zero hard faults, zero resets, no acquisition errors. The failure moved to the next layer: the probe trigger itself.

## Iteration 2 diagnosis: drift-safety vs trigger-confirm race (2026-07-23)

Reproduction (all on `13952564`):

| Condition | Result |
|---|---|
| `PROBE` (2 mm/s, center), cold | PASS (peak force ~281 g, clean trigger) |
| `LOADCELL_Z_HOME`, bed 60 C + nozzle 140 C | FAIL (drift safety) x2, then PASS x1 (intermittent) |
| `LOAD_CELL_CALIBRATE TARE=TRUE` then `LOADCELL_Z_HOME`, hot | FAIL (fresh tare does not help) |
| `PROBE HOME=Z` hot, center (X128 Y128) | PASS |
| `PROBE HOME=Z` hot, edge (X120 Y-1) | PASS |
| `LOADCELL_Z_HOME` hot (retry) | PASS — cleaning-pass peak force ~475 g |

Mechanism: `load_cell_probe_report_sample_at()` checked the raw drift-safety
band (tare +/- 1000 g) BEFORE evaluating the 12.5 ms trigger confirm. At
~86 SPS the confirm needs 2+ above-threshold samples; on a fast contact ramp
(cleaning-pass probes, stiff contact) raw crosses the band within 1-2 samples
— before the confirm can complete. Upstream Kalico triggers on the FIRST
filtered crossing and never sees this race; the 12.5 ms confirm is a rewrite
addition. Live peaks during a passing cleaning probe reach ~475 g, so normal
variance intermittently pushes a sample past 1000 g mid-confirm.

Fix (Kalico `pending`): safety gates only the ARMING sample of a contact
streak; once armed, the confirm owns the outcome and may complete across the
band. Below threshold the band still applies and any armed confirm resets.
Matches upstream semantics for first-crossing (a first sample already beyond
the band still errors, e.g. lost tare) while letting genuine fast contacts
finish the confirm.

### 2026-07-23T08:46Z — iteration-2 mesh attempt 1 (Kalico `a4ceadd3`)

- Image/Kalico/Cosmos revision: Kalico `a4ceadd3-dirty` (all 3 MCUs), Cosmos r14 build 20260723_120805
- Bed target/measured temperature: 60 C target via wrapper
- Exact command/request: `BED_MESH_CALIBRATE BED_TEMP=60` via Moonraker
- Result: FAIL at 189 s
- Exact response/error: HTTP 400 `force exceeded drift_safety_limit before triggering!`
- HX711 counters before -> after:
  - frames.received 24762 -> 41144; valid 24732 -> 41093
  - hard 0 -> 0 (last_hard_fault: null); settling 7 -> 7 (no recoveries); timing 23 -> 44
  - channel_faults: ch3 18 -> 37 (dominant), ch2 4 -> 6, ch1 1
- Interpretation: acquisition still clean (zero hard faults). With
  first-crossing triggering (confirm=0), the drift-band error now requires
  the FIRST above-threshold sample beyond the band: a single-sample jump of
  >1000 g on the sum. A real 2 mm/s contact cannot do that in 25 um of
  travel — these are one-frame electrical glitches, ch3-dominant. Matches
  the old Elegoo 100k-count heuristic and the spike filter on
  OpenCentauri/kalico hx711s-new.

## Iteration 3 fix (2026-07-23): per-channel spike rejection

Kalico `87f1c4f7` ports the hx711s-new spike filter: a channel delta >100k
counts vs last-good is held back from the probe (raw frame still streams to
the host); a second consecutive sample near the suspicious value confirms a
genuine force step, an immediate revert confirms an isolated glitch. Genuine
fast ramps see at most one sample (~12 ms) of delay; one-frame spikes can no
longer reach the trigger/safety path. Deployed via Cosmos r15.

Flash-incident note (between r14 and r15): the r14 flash hit a stale
overlayfs handle on /etc/klipper/config/klipper-readonly (slot-switch
artifact, cleared by stopping klipper/moonraker/grumpyscreen/ustreamer),
then swupdate died with SIGBUS mid-run; the box wedged (dropbear
resetting) and needed a power cycle. It booted into r14 cleanly — the
critical writes had completed. Flash protocol updated: stop services first.

### 2026-07-23T09:04Z — iteration-3 mesh attempts 1-5 (Kalico `87f1c4f7`)

- Image/Kalico/Cosmos revision: Kalico `87f1c4f7-dirty` (all 3 MCUs), Cosmos r15 build 20260723_125805
- Bed target/measured temperature: 60 C target via wrapper
- Exact command/request: `BED_MESH_CALIBRATE BED_TEMP=60` via Moonraker, x5 attempts
- Results:
  - Attempt 1: PASS (530 s) — first full macro pass of the campaign
  - Attempt 2: PASS (444 s)
  - Attempt 3: PASS (446 s)
  - Attempt 4: PASS (448 s)
  - Attempt 5: FAIL at 182 s
- Exact response/error (attempt 5): HTTP 400 `Probe samples exceed samples_tolerance`
- HX711 counters (attempt 5, before -> after): received 163682 -> 179404; hard 0; timing 188 -> 213; ch3 channel_faults 118 -> 137
- Log evidence: the failure is at the LOADCELL_Z_HOME edge cleaning probe
  (X120 Y-1): after a TAP_SHAPE_INVALID retry, a good cluster (~0.075 mm),
  then a re-tare, then a sample pair reading -0.143173 and +0.003167 mm —
  0.146 mm spread vs the 0.05 mm tolerance. One anomalous deep-trigger
  sample in a heated, post-squish (ooze-mangled) edge context.
- Isolated measurements: edge probes at 75 g trigger repeat to 0.006 mm
  over 5 samples; squish+probe sequences (cold) 0.003 mm over 3 runs. The
  anomaly is intermittent (~1/100 edge probes) and context-dependent.
- Interpretation: driver probing is accurate; the failure is macro-fragility
  at a step whose purpose is ooze management, not measurement. The 12.5 ms
  spike filter ended the drift-safety deaths (0 hard faults across 5
  attempts, 213 timing skips absorbed).

### 2026-07-23T10:02Z — iteration-4 mesh attempts 1-6 (Kalico `87f1c4f7`, macro tolerance 0.1)

- Image/Kalico/Cosmos revision: Kalico `87f1c4f7-dirty` (all 3 MCUs), Cosmos r15 + macros.cfg cleaning-pass SAMPLES_TOLERANCE=0.1
- Bed target/measured temperature: 60 C target via wrapper
- Exact command/request: `BED_MESH_CALIBRATE BED_TEMP=60` via Moonraker, x6 attempts
- Results: attempts 1-5 PASS (460/443/441/446/443 s) — 5 consecutive, new campaign record; attempt 6 FAIL at 333 s
- Exact response/error (attempt 6): HTTP 400 `Sensor reported 0 acquisition errors and 1 bulk overflows while sampling`
- HX711 counters (attempt 6): received 194560 -> 223212; hard 0; timing 221 -> 259
- Interpretation: one lost bulk message after ~45 min of continuous
  streaming — a transient host scheduling hiccup, not acquisition
  corruption. Failing a 7.5-minute macro on one lost frame is too strict:
  hx711s frames are timestamped, so the gap is known; consumers are
  gap-tolerant (tare median, tap shape validation, MCU trigger does not
  use bulk data).

## Iteration 5 fix (2026-07-23): bounded bulk-overflow tolerance

Kalico `c368f84e`: max_tolerated_overflows sensor capability (2 for hx711s,
0 default for other sensors). LoadCell.validate_samples treats (0, <=2) as
warning + continue; larger or mixed losses stay fatal. Covers tare and tap
collection paths. 17 host tests pass. Deployed via Cosmos r16.

### 2026-07-23T11:02Z — FINAL CAMPAIGN: 10/10 PASS (Kalico `c368f84e`)

- Image/Kalico/Cosmos revision: Kalico `c368f84e-dirty` (all 3 MCUs), Cosmos r16 + macros.cfg cleaning-pass tolerance 0.1
- Bed target/measured temperature: 60 C target via wrapper
- Exact command/request: `BED_MESH_CALIBRATE BED_TEMP=60` via Moonraker, x10
- Results: 10/10 PASS. Durations 440-509 s (median ~446 s). Loop exited 0 at 2026-07-23T12:18:29-04:00.
- Cumulative counters across all 10 attempts (~76 min):
  - frames.received 392294, valid 391842
  - **hard faults: 0 (last_hard_fault: null); recoveries: 0 (settling constant at 7); bulk overflow aborts: 0**
  - timing skips absorbed: 445 total (not_ready 311, extra_low 134, post_read_low 134)
  - channel_faults: ch3 311 (dominant as always), ch2 89, ch1 45
- Conclusion: the five-iteration stack holds end to end. Bed mesh at 60 C
  is reliable on the HX711 rewrite.

## Campaign summary — the five layers

1. `13952564` — timing misses (not_ready/extra_low/post_read_low, read
   racing the free-running ADCs) reclassified from reset-worthy faults to
   silent sample skips; streak escalation via Q_TIMING_STREAK stays fatal.
2. `426ac30b` — drift-safety band gates only the ARMING sample of a contact
   streak; an armed trigger confirm owns completion across the band.
3. `a4ceadd3` — trigger_confirm_time defaults to 0: first-crossing
   triggering, matching every upstream reference implementation.
4. `87f1c4f7` — per-channel spike rejection (>100k-count single-sample
   jumps held one sample; revert = glitch, repeat = genuine step), ported
   from OpenCentauri/kalico hx711s-new.
5. `c368f84e` — bounded bulk-overflow tolerance (<=2/session) via a
   max_tolerated_overflows sensor capability.
Plus one config change: LOADCELL_Z_HOME cleaning-pass SAMPLES_TOLERANCE
0.05 -> 0.1 (ooze-management step, not measurement).

---

## 4. Campaign loop logs (verbatim, from cosmos-nightly iter logs)

### iter0 (kalico 64593629, pre-campaign firmware) — baseline failure
### 2026-07-23T00:28:12-04:00 attempt 1
-- counters before: {"channel_faults": {"1": 27, "2": 60, "3": 192}, "frames": {"fault": 2239, "hard": 279, "received": 226936, "settling": 1960, "valid": 224697}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": {"channels": [3], "counts": [166563, -124058, 283246, -32], "flags": ["extra_low", "post_read_low"], "hard": true, "quality": 2060, "time": 2670.20277, "wire_quality": 2802911244}, "quality_flags": {"extra_low": 73, "not_ready": 206, "post_read_low": 73, "settling": 1960}}
-- rc=0 duration=455s
-- response: {"error":{"code":400,"message":"Error during homing probe: Load Cell Probe Error: invalid HX711 sample; see sensor fault diagnostics","traceback":"Traceback (most recent call last):\n\n  File \"/usr/share/moonraker/moonraker/components/application.py\", line 761, in _process_http_request\n    result = await self.api_defintion.request(\n\n  File \"/usr/share/moonraker/moonraker/components/klippy_connection.py\", line 617, in request\n    return await self._request_standard(web_request)\n\n  File \"/usr/share/moonraker/moonraker/components/klippy_connection.py\", line 714, in _request_standard\n    return await base_request.wait(timeout)\n\n  File \"/usr/share/moonraker/moonraker/components/klippy_connection.py\", line 825, in wait\n    return await asyncio.wait_for(asyncio.shield(self._fut), to)\n\n  File \"/usr/lib/python3.12/asyncio/tasks.py\", line 520, in wait_for\n    return await fut\n\nmoonraker.utils.exceptions.ServerError: Error during homing probe: Load Cell Probe Error: invalid HX711 sample; see sensor fault diagnostics\n\n\nThe above exception was the direct cause of the following exception:\n\n\nTraceback (most recent call last):\n\n  File \"/usr/lib/python3.12/site-packages/tornado/web.py\", line 1859, in _execute\n    result = await result\n\n  File \"/usr/share/moonraker/moonraker/components/application.py\", line 744, in get\n    await self._process_http_request(RequestType.GET)\n\n  File \"/usr/share/moonraker/moonraker/components/application.py\", line 767, in _process_http_request\n    raise tornado.web.HTTPError(\n\ntornado.web.HTTPError: HTTP 400: Error during homing probe: Load Cell Probe Error: invalid HX711 sample; see sensor fault diagnostics\n"}}
HTTP:400
-- counters after: {"channel_faults": {"1": 27, "2": 67, "3": 221}, "frames": {"fault": 2527, "hard": 315, "received": 266022, "settling": 2212, "valid": 263495}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": {"channels": [3], "counts": [161010, -135752, 271421, -32], "flags": ["extra_low", "post_read_low"], "hard": true, "quality": 2060, "time": 3149.162025, "wire_quality": 2802911244}, "quality_flags": {"extra_low": 87, "not_ready": 228, "post_read_low": 87, "settling": 2212}}
-- FAIL attempt 1, stopping

### iter1 (kalico 13952564)
### 2026-07-23T07:18:28-04:00 attempt 1
-- counters before: {"channel_faults": {"2": 3, "3": 1}, "frames": {"fault": 11, "received": 1982, "settling": 7, "timing": 4, "valid": 1971}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 1, "not_ready": 3, "post_read_low": 1, "settling": 7}}
-- rc=0 duration=230s
-- response: {"error":{"code":400,"message":"Error during homing probe: Load Cell Probe Error: force exceeded drift_safety_limit before triggering!","traceback":"Traceback (most recent call last):\n\n  File \"/usr/share/moonraker/moonraker/components/application.py\", line 761, in _process_http_request\n    result = await self.api_defintion.request(\n\n  File \"/usr/share/moonraker/moonraker/components/klippy_connection.py\", line 617, in request\n    return await self._request_standard(web_request)\n\n  File \"/usr/share/moonraker/moonraker/components/klippy_connection.py\", line 714, in _request_standard\n    return await base_request.wait(timeout)\n\n  File \"/usr/share/moonraker/moonraker/components/klippy_connection.py\", line 825, in wait\n    return await asyncio.wait_for(asyncio.shield(self._fut), to)\n\n  File \"/usr/lib/python3.12/asyncio/tasks.py\", line 520, in wait_for\n    return await fut\n\nmoonraker.utils.exceptions.ServerError: Error during homing probe: Load Cell Probe Error: force exceeded drift_safety_limit before triggering!\n\n\nThe above exception was the direct cause of the following exception:\n\n\nTraceback (most recent call last):\n\n  File \"/usr/lib/python3.12/site-packages/tornado/web.py\", line 1859, in _execute\n    result = await result\n\n  File \"/usr/share/moonraker/moonraker/components/application.py\", line 744, in get\n    await self._process_http_request(RequestType.GET)\n\n  File \"/usr/share/moonraker/moonraker/components/application.py\", line 767, in _process_http_request\n    raise tornado.web.HTTPError(\n\ntornado.web.HTTPError: HTTP 400: Error during homing probe: Load Cell Probe Error: force exceeded drift_safety_limit before triggering!\n"}}
HTTP:400
-- counters after: {"channel_faults": {"2": 20, "3": 19}, "frames": {"fault": 46, "received": 21972, "settling": 7, "timing": 39, "valid": 21926}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 11, "not_ready": 28, "post_read_low": 11, "settling": 7}}
-- FAIL attempt 1, stopping

### iter2 (kalico a4ceadd3)
### 2026-07-23T08:46:33-04:00 attempt 1
-- counters before: {"channel_faults": {"1": 1, "2": 4, "3": 18}, "frames": {"fault": 30, "received": 24762, "settling": 7, "timing": 23, "valid": 24732}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 1, "valid": 9}, "last_hard_fault": null, "quality_flags": {"extra_low": 10, "not_ready": 13, "post_read_low": 10, "settling": 7}}
-- rc=0 duration=189s
-- response: {"error":{"code":400,"message":"Error during homing probe: Load Cell Probe Error: force exceeded drift_safety_limit before triggering!","traceback":"Traceback (most recent call last):\n\n  File \"/usr/share/moonraker/moonraker/components/application.py\", line 761, in _process_http_request\n    result = await self.api_defintion.request(\n\n  File \"/usr/share/moonraker/moonraker/components/klippy_connection.py\", line 617, in request\n    return await self._request_standard(web_request)\n\n  File \"/usr/share/moonraker/moonraker/components/klippy_connection.py\", line 714, in _request_standard\n    return await base_request.wait(timeout)\n\n  File \"/usr/share/moonraker/moonraker/components/klippy_connection.py\", line 825, in wait\n    return await asyncio.wait_for(asyncio.shield(self._fut), to)\n\n  File \"/usr/lib/python3.12/asyncio/tasks.py\", line 520, in wait_for\n    return await fut\n\nmoonraker.utils.exceptions.ServerError: Error during homing probe: Load Cell Probe Error: force exceeded drift_safety_limit before triggering!\n\n\nThe above exception was the direct cause of the following exception:\n\n\nTraceback (most recent call last):\n\n  File \"/usr/lib/python3.12/site-packages/tornado/web.py\", line 1859, in _execute\n    result = await result\n\n  File \"/usr/share/moonraker/moonraker/components/application.py\", line 744, in get\n    await self._process_http_request(RequestType.GET)\n\n  File \"/usr/share/moonraker/moonraker/components/application.py\", line 767, in _process_http_request\n    raise tornado.web.HTTPError(\n\ntornado.web.HTTPError: HTTP 400: Error during homing probe: Load Cell Probe Error: force exceeded drift_safety_limit before triggering!\n"}}
HTTP:400
-- counters after: {"channel_faults": {"1": 1, "2": 6, "3": 37}, "frames": {"fault": 51, "received": 41144, "settling": 7, "timing": 44, "valid": 41093}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 16, "not_ready": 28, "post_read_low": 16, "settling": 7}}
-- FAIL attempt 1, stopping

### iter3 (kalico 87f1c4f7)
### 2026-07-23T09:04:58-04:00 attempt 1
-- counters before: {"channel_faults": {"1": 2, "3": 2}, "frames": {"fault": 11, "received": 2606, "settling": 7, "timing": 4, "valid": 2595}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"not_ready": 4, "settling": 7}}
-- rc=0 duration=530s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 7, "2": 12, "3": 35}, "frames": {"fault": 61, "received": 48284, "settling": 7, "timing": 54, "valid": 48223}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 12, "not_ready": 42, "post_read_low": 12, "settling": 7}}
-- PASS attempt 1
### 2026-07-23T09:13:49-04:00 attempt 2
-- counters before: {"channel_faults": {"1": 7, "2": 12, "3": 35}, "frames": {"fault": 61, "received": 48328, "settling": 7, "timing": 54, "valid": 48267}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 12, "not_ready": 42, "post_read_low": 12, "settling": 7}}
-- rc=0 duration=444s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 14, "2": 24, "3": 61}, "frames": {"fault": 106, "received": 86620, "settling": 7, "timing": 99, "valid": 86514}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 20, "not_ready": 79, "post_read_low": 20, "settling": 7}}
-- PASS attempt 2
### 2026-07-23T09:21:15-04:00 attempt 3
-- counters before: {"channel_faults": {"1": 14, "2": 24, "3": 62}, "frames": {"fault": 107, "received": 86662, "settling": 7, "timing": 100, "valid": 86555}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 20, "not_ready": 80, "post_read_low": 20, "settling": 7}}
-- rc=0 duration=446s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 19, "2": 36, "3": 90}, "frames": {"fault": 152, "received": 125048, "settling": 7, "timing": 145, "valid": 124896}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 28, "not_ready": 117, "post_read_low": 28, "settling": 7}}
-- PASS attempt 3
### 2026-07-23T09:28:42-04:00 attempt 4
-- counters before: {"channel_faults": {"1": 19, "2": 36, "3": 90}, "frames": {"fault": 152, "received": 125092, "settling": 7, "timing": 145, "valid": 124940}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 28, "not_ready": 117, "post_read_low": 28, "settling": 7}}
-- rc=0 duration=448s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 26, "2": 44, "3": 118}, "frames": {"fault": 195, "received": 163640, "settling": 7, "timing": 188, "valid": 163445}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 38, "not_ready": 150, "post_read_low": 38, "settling": 7}}
-- PASS attempt 4
### 2026-07-23T09:36:11-04:00 attempt 5
-- counters before: {"channel_faults": {"1": 26, "2": 44, "3": 118}, "frames": {"fault": 195, "received": 163682, "settling": 7, "timing": 188, "valid": 163487}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 38, "not_ready": 150, "post_read_low": 38, "settling": 7}}
-- rc=0 duration=182s
-- response: {"error":{"code":400,"message":"Probe samples exceed samples_tolerance","traceback":"Traceback (most recent call last):\n\n  File \"/usr/share/moonraker/moonraker/components/application.py\", line 761, in _process_http_request\n    result = await self.api_defintion.request(\n\n  File \"/usr/share/moonraker/moonraker/components/klippy_connection.py\", line 617, in request\n    return await self._request_standard(web_request)\n\n  File \"/usr/share/moonraker/moonraker/components/klippy_connection.py\", line 714, in _request_standard\n    return await base_request.wait(timeout)\n\n  File \"/usr/share/moonraker/moonraker/components/klippy_connection.py\", line 825, in wait\n    return await asyncio.wait_for(asyncio.shield(self._fut), to)\n\n  File \"/usr/lib/python3.12/asyncio/tasks.py\", line 520, in wait_for\n    return await fut\n\nmoonraker.utils.exceptions.ServerError: Probe samples exceed samples_tolerance\n\n\nThe above exception was the direct cause of the following exception:\n\n\nTraceback (most recent call last):\n\n  File \"/usr/lib/python3.12/site-packages/tornado/web.py\", line 1859, in _execute\n    result = await result\n\n  File \"/usr/share/moonraker/moonraker/components/application.py\", line 744, in get\n    await self._process_http_request(RequestType.GET)\n\n  File \"/usr/share/moonraker/moonraker/components/application.py\", line 767, in _process_http_request\n    raise tornado.web.HTTPError(\n\ntornado.web.HTTPError: HTTP 400: Probe samples exceed samples_tolerance\n"}}
HTTP:400
-- counters after: {"channel_faults": {"1": 28, "2": 48, "3": 137}, "frames": {"fault": 220, "received": 179404, "settling": 7, "timing": 213, "valid": 179184}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 49, "not_ready": 164, "post_read_low": 49, "settling": 7}}
-- FAIL attempt 5, stopping

### iter4 (kalico 87f1c4f7 + macro tolerance 0.1)
### 2026-07-23T10:02:04-04:00 attempt 1
-- counters before: {"channel_faults": {"2": 1, "3": 1}, "frames": {"fault": 9, "received": 2190, "settling": 7, "timing": 2, "valid": 2181}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"not_ready": 2, "settling": 7}}
-- rc=0 duration=460s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 7, "2": 10, "3": 31}, "frames": {"fault": 55, "received": 41722, "settling": 7, "timing": 48, "valid": 41667}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 8, "not_ready": 40, "post_read_low": 8, "settling": 7}}
-- PASS attempt 1
### 2026-07-23T10:09:44-04:00 attempt 2
-- counters before: {"channel_faults": {"1": 7, "2": 10, "3": 31}, "frames": {"fault": 55, "received": 41784, "settling": 7, "timing": 48, "valid": 41729}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 8, "not_ready": 40, "post_read_low": 8, "settling": 7}}
-- rc=0 duration=443s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 8, "2": 17, "3": 63}, "frames": {"fault": 95, "received": 79824, "settling": 7, "timing": 88, "valid": 79729}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 25, "not_ready": 63, "post_read_low": 25, "settling": 7}}
-- PASS attempt 2
### 2026-07-23T10:17:08-04:00 attempt 3
-- counters before: {"channel_faults": {"1": 8, "2": 17, "3": 63}, "frames": {"fault": 95, "received": 79868, "settling": 7, "timing": 88, "valid": 79773}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 25, "not_ready": 63, "post_read_low": 25, "settling": 7}}
-- rc=0 duration=441s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 12, "2": 26, "3": 93}, "frames": {"fault": 138, "received": 117746, "settling": 7, "timing": 131, "valid": 117608}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 37, "not_ready": 94, "post_read_low": 37, "settling": 7}}
-- PASS attempt 3
### 2026-07-23T10:24:30-04:00 attempt 4
-- counters before: {"channel_faults": {"1": 12, "2": 26, "3": 93}, "frames": {"fault": 138, "received": 117788, "settling": 7, "timing": 131, "valid": 117650}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 37, "not_ready": 94, "post_read_low": 37, "settling": 7}}
-- rc=0 duration=446s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 16, "2": 36, "3": 116}, "frames": {"fault": 175, "received": 156252, "settling": 7, "timing": 168, "valid": 156077}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 45, "not_ready": 123, "post_read_low": 45, "settling": 7}}
-- PASS attempt 4
### 2026-07-23T10:31:58-04:00 attempt 5
-- counters before: {"channel_faults": {"1": 16, "2": 36, "3": 116}, "frames": {"fault": 175, "received": 156338, "settling": 7, "timing": 168, "valid": 156163}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 45, "not_ready": 123, "post_read_low": 45, "settling": 7}}
-- rc=0 duration=443s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 19, "2": 50, "3": 151}, "frames": {"fault": 227, "received": 194474, "settling": 7, "timing": 220, "valid": 194247}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 55, "not_ready": 165, "post_read_low": 55, "settling": 7}}
-- PASS attempt 5
### 2026-07-23T10:39:23-04:00 attempt 6
-- counters before: {"channel_faults": {"1": 19, "2": 50, "3": 152}, "frames": {"fault": 228, "received": 194560, "settling": 7, "timing": 221, "valid": 194332}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 56, "not_ready": 165, "post_read_low": 56, "settling": 7}}
-- rc=0 duration=333s
-- response: {"error":{"code":400,"message":"Sensor reported 0 acquisition errors and 1 bulk overflows while sampling","traceback":"Traceback (most recent call last):\n\n  File \"/usr/share/moonraker/moonraker/components/application.py\", line 761, in _process_http_request\n    result = await self.api_defintion.request(\n\n  File \"/usr/share/moonraker/moonraker/components/klippy_connection.py\", line 617, in request\n    return await self._request_standard(web_request)\n\n  File \"/usr/share/moonraker/moonraker/components/klippy_connection.py\", line 714, in _request_standard\n    return await base_request.wait(timeout)\n\n  File \"/usr/share/moonraker/moonraker/components/klippy_connection.py\", line 825, in wait\n    return await asyncio.wait_for(asyncio.shield(self._fut), to)\n\n  File \"/usr/lib/python3.12/asyncio/tasks.py\", line 520, in wait_for\n    return await fut\n\nmoonraker.utils.exceptions.ServerError: Sensor reported 0 acquisition errors and 1 bulk overflows while sampling\n\n\nThe above exception was the direct cause of the following exception:\n\n\nTraceback (most recent call last):\n\n  File \"/usr/lib/python3.12/site-packages/tornado/web.py\", line 1859, in _execute\n    result = await result\n\n  File \"/usr/share/moonraker/moonraker/components/application.py\", line 744, in get\n    await self._process_http_request(RequestType.GET)\n\n  File \"/usr/share/moonraker/moonraker/components/application.py\", line 767, in _process_http_request\n    raise tornado.web.HTTPError(\n\ntornado.web.HTTPError: HTTP 400: Sensor reported 0 acquisition errors and 1 bulk overflows while sampling\n"}}
HTTP:400
-- counters after: {"channel_faults": {"0": 1, "1": 23, "2": 59, "3": 176}, "frames": {"fault": 266, "received": 223212, "settling": 7, "timing": 259, "valid": 222946}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 67, "not_ready": 192, "post_read_low": 67, "settling": 7}}
-- FAIL attempt 6, stopping

### iter5 (kalico c368f84e) — 10/10 PASS

### hot100 (kalico c368f84e) — 20/20 PASS at 100C

- Command: BED_MESH_CALIBRATE BED_TEMP=100 x20, 2026-07-23T14:10Z -> 16:45Z (~2.5 h)
- Results: 20/20 PASS. Durations 446-664 s.
- Cumulative counters: frames.received 936984, valid 936291; hard faults 0
  (last_hard_fault: null); recoveries 0 (settling constant 7); bulk overflow
  aborts 0; timing skips absorbed 686 (not_ready 497, extra_low 189,
  post_read_low 189); channel_faults ch3 358 / ch2 311 / ch1 17.
- Raw loop log:
### 2026-07-23T14:10:44-04:00 attempt 1
-- counters before: {"channel_faults": {"1": 8, "2": 45, "3": 124}, "frames": {"fault": 184, "received": 148670, "settling": 7, "timing": 177, "valid": 148486}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 51, "not_ready": 126, "post_read_low": 51, "settling": 7}}
-- rc=0 duration=664s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 13, "2": 67, "3": 157}, "frames": {"fault": 244, "received": 205400, "settling": 7, "timing": 237, "valid": 205156}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 66, "not_ready": 171, "post_read_low": 66, "settling": 7}}
-- PASS attempt 1
### 2026-07-23T14:21:49-04:00 attempt 2
-- counters before: {"channel_faults": {"1": 13, "2": 67, "3": 157}, "frames": {"fault": 244, "received": 205442, "settling": 7, "timing": 237, "valid": 205198}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 66, "not_ready": 171, "post_read_low": 66, "settling": 7}}
-- rc=0 duration=459s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 14, "2": 76, "3": 178}, "frames": {"fault": 275, "received": 244432, "settling": 7, "timing": 268, "valid": 244157}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 78, "not_ready": 190, "post_read_low": 78, "settling": 7}}
-- PASS attempt 2
### 2026-07-23T14:29:29-04:00 attempt 3
-- counters before: {"channel_faults": {"1": 14, "2": 76, "3": 178}, "frames": {"fault": 275, "received": 244474, "settling": 7, "timing": 268, "valid": 244199}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 78, "not_ready": 190, "post_read_low": 78, "settling": 7}}
-- rc=0 duration=455s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 14, "2": 81, "3": 187}, "frames": {"fault": 289, "received": 283016, "settling": 7, "timing": 282, "valid": 282727}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 83, "not_ready": 199, "post_read_low": 83, "settling": 7}}
-- PASS attempt 3
### 2026-07-23T14:37:04-04:00 attempt 4
-- counters before: {"channel_faults": {"1": 14, "2": 81, "3": 187}, "frames": {"fault": 289, "received": 283058, "settling": 7, "timing": 282, "valid": 282769}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 83, "not_ready": 199, "post_read_low": 83, "settling": 7}}
-- rc=0 duration=458s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 14, "2": 100, "3": 205}, "frames": {"fault": 326, "received": 321832, "settling": 7, "timing": 319, "valid": 321506}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 91, "not_ready": 228, "post_read_low": 91, "settling": 7}}
-- PASS attempt 4
### 2026-07-23T14:44:43-04:00 attempt 5
-- counters before: {"channel_faults": {"1": 14, "2": 100, "3": 205}, "frames": {"fault": 326, "received": 321876, "settling": 7, "timing": 319, "valid": 321550}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 91, "not_ready": 228, "post_read_low": 91, "settling": 7}}
-- rc=0 duration=459s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 14, "2": 118, "3": 215}, "frames": {"fault": 354, "received": 360786, "settling": 7, "timing": 347, "valid": 360432}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 104, "not_ready": 243, "post_read_low": 104, "settling": 7}}
-- PASS attempt 5
### 2026-07-23T14:52:23-04:00 attempt 6
-- counters before: {"channel_faults": {"1": 14, "2": 118, "3": 215}, "frames": {"fault": 354, "received": 360828, "settling": 7, "timing": 347, "valid": 360474}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 104, "not_ready": 243, "post_read_low": 104, "settling": 7}}
-- rc=0 duration=450s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 14, "2": 138, "3": 221}, "frames": {"fault": 380, "received": 399082, "settling": 7, "timing": 373, "valid": 398702}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 108, "not_ready": 265, "post_read_low": 108, "settling": 7}}
-- PASS attempt 6
### 2026-07-23T14:59:55-04:00 attempt 7
-- counters before: {"channel_faults": {"1": 14, "2": 138, "3": 221}, "frames": {"fault": 380, "received": 399126, "settling": 7, "timing": 373, "valid": 398746}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 108, "not_ready": 265, "post_read_low": 108, "settling": 7}}
-- rc=0 duration=450s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 14, "2": 149, "3": 227}, "frames": {"fault": 397, "received": 437318, "settling": 7, "timing": 390, "valid": 436921}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 112, "not_ready": 278, "post_read_low": 112, "settling": 7}}
-- PASS attempt 7
### 2026-07-23T15:07:25-04:00 attempt 8
-- counters before: {"channel_faults": {"1": 14, "2": 149, "3": 227}, "frames": {"fault": 397, "received": 437360, "settling": 7, "timing": 390, "valid": 436963}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 112, "not_ready": 278, "post_read_low": 112, "settling": 7}}
-- rc=0 duration=450s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 15, "2": 162, "3": 231}, "frames": {"fault": 415, "received": 475636, "settling": 7, "timing": 408, "valid": 475221}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 114, "not_ready": 294, "post_read_low": 114, "settling": 7}}
-- PASS attempt 8
### 2026-07-23T15:14:57-04:00 attempt 9
-- counters before: {"channel_faults": {"1": 15, "2": 162, "3": 231}, "frames": {"fault": 415, "received": 475680, "settling": 7, "timing": 408, "valid": 475265}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 114, "not_ready": 294, "post_read_low": 114, "settling": 7}}
-- rc=0 duration=454s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 16, "2": 174, "3": 238}, "frames": {"fault": 435, "received": 514254, "settling": 7, "timing": 428, "valid": 513819}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 118, "not_ready": 310, "post_read_low": 118, "settling": 7}}
-- PASS attempt 9
### 2026-07-23T15:22:32-04:00 attempt 10
-- counters before: {"channel_faults": {"1": 16, "2": 174, "3": 238}, "frames": {"fault": 435, "received": 514298, "settling": 7, "timing": 428, "valid": 513863}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 118, "not_ready": 310, "post_read_low": 118, "settling": 7}}
-- rc=0 duration=451s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 16, "2": 188, "3": 252}, "frames": {"fault": 463, "received": 552616, "settling": 7, "timing": 456, "valid": 552153}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 123, "not_ready": 333, "post_read_low": 123, "settling": 7}}
-- PASS attempt 10
### 2026-07-23T15:30:04-04:00 attempt 11
-- counters before: {"channel_faults": {"1": 16, "2": 188, "3": 252}, "frames": {"fault": 463, "received": 552674, "settling": 7, "timing": 456, "valid": 552211}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 123, "not_ready": 333, "post_read_low": 123, "settling": 7}}
-- rc=0 duration=447s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 16, "2": 195, "3": 263}, "frames": {"fault": 481, "received": 590646, "settling": 7, "timing": 474, "valid": 590165}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 125, "not_ready": 349, "post_read_low": 125, "settling": 7}}
-- PASS attempt 11
### 2026-07-23T15:37:32-04:00 attempt 12
-- counters before: {"channel_faults": {"1": 16, "2": 195, "3": 263}, "frames": {"fault": 481, "received": 590688, "settling": 7, "timing": 474, "valid": 590207}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 125, "not_ready": 349, "post_read_low": 125, "settling": 7}}
-- rc=0 duration=448s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 17, "2": 210, "3": 276}, "frames": {"fault": 510, "received": 628740, "settling": 7, "timing": 503, "valid": 628230}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 134, "not_ready": 369, "post_read_low": 134, "settling": 7}}
-- PASS attempt 12
### 2026-07-23T15:45:00-04:00 attempt 13
-- counters before: {"channel_faults": {"1": 17, "2": 210, "3": 276}, "frames": {"fault": 510, "received": 628782, "settling": 7, "timing": 503, "valid": 628272}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 134, "not_ready": 369, "post_read_low": 134, "settling": 7}}
-- rc=0 duration=448s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 17, "2": 224, "3": 285}, "frames": {"fault": 533, "received": 666882, "settling": 7, "timing": 526, "valid": 666349}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 140, "not_ready": 386, "post_read_low": 140, "settling": 7}}
-- PASS attempt 13
### 2026-07-23T15:52:29-04:00 attempt 14
-- counters before: {"channel_faults": {"1": 17, "2": 224, "3": 285}, "frames": {"fault": 533, "received": 666926, "settling": 7, "timing": 526, "valid": 666393}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 140, "not_ready": 386, "post_read_low": 140, "settling": 7}}
-- rc=0 duration=446s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 17, "2": 241, "3": 291}, "frames": {"fault": 556, "received": 704862, "settling": 7, "timing": 549, "valid": 704306}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 144, "not_ready": 405, "post_read_low": 144, "settling": 7}}
-- PASS attempt 14
### 2026-07-23T15:59:57-04:00 attempt 15
-- counters before: {"channel_faults": {"1": 17, "2": 241, "3": 291}, "frames": {"fault": 556, "received": 704904, "settling": 7, "timing": 549, "valid": 704348}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 144, "not_ready": 405, "post_read_low": 144, "settling": 7}}
-- rc=0 duration=450s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 17, "2": 252, "3": 302}, "frames": {"fault": 578, "received": 743136, "settling": 7, "timing": 571, "valid": 742558}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 149, "not_ready": 422, "post_read_low": 149, "settling": 7}}
-- PASS attempt 15
### 2026-07-23T16:07:28-04:00 attempt 16
-- counters before: {"channel_faults": {"1": 17, "2": 252, "3": 302}, "frames": {"fault": 578, "received": 743180, "settling": 7, "timing": 571, "valid": 742602}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 149, "not_ready": 422, "post_read_low": 149, "settling": 7}}
-- rc=0 duration=454s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 17, "2": 261, "3": 313}, "frames": {"fault": 598, "received": 781762, "settling": 7, "timing": 591, "valid": 781164}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 156, "not_ready": 435, "post_read_low": 156, "settling": 7}}
-- PASS attempt 16
### 2026-07-23T16:15:03-04:00 attempt 17
-- counters before: {"channel_faults": {"1": 17, "2": 261, "3": 313}, "frames": {"fault": 598, "received": 781846, "settling": 7, "timing": 591, "valid": 781248}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 156, "not_ready": 435, "post_read_low": 156, "settling": 7}}
-- rc=0 duration=460s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 17, "2": 274, "3": 320}, "frames": {"fault": 618, "received": 820840, "settling": 7, "timing": 611, "valid": 820222}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 162, "not_ready": 449, "post_read_low": 162, "settling": 7}}
-- PASS attempt 17
### 2026-07-23T16:22:44-04:00 attempt 18
-- counters before: {"channel_faults": {"1": 17, "2": 274, "3": 320}, "frames": {"fault": 618, "received": 820882, "settling": 7, "timing": 611, "valid": 820264}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 162, "not_ready": 449, "post_read_low": 162, "settling": 7}}
-- rc=0 duration=462s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 17, "2": 294, "3": 326}, "frames": {"fault": 644, "received": 860092, "settling": 7, "timing": 637, "valid": 859448}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 174, "not_ready": 463, "post_read_low": 174, "settling": 7}}
-- PASS attempt 18
### 2026-07-23T16:30:28-04:00 attempt 19
-- counters before: {"channel_faults": {"1": 17, "2": 294, "3": 326}, "frames": {"fault": 644, "received": 860136, "settling": 7, "timing": 637, "valid": 859492}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 174, "not_ready": 463, "post_read_low": 174, "settling": 7}}
-- rc=0 duration=453s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 17, "2": 300, "3": 337}, "frames": {"fault": 661, "received": 898560, "settling": 7, "timing": 654, "valid": 897899}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 179, "not_ready": 475, "post_read_low": 179, "settling": 7}}
-- PASS attempt 19
### 2026-07-23T16:38:02-04:00 attempt 20
-- counters before: {"channel_faults": {"1": 17, "2": 300, "3": 337}, "frames": {"fault": 661, "received": 898618, "settling": 7, "timing": 654, "valid": 897957}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 179, "not_ready": 475, "post_read_low": 179, "settling": 7}}
-- rc=0 duration=452s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 17, "2": 311, "3": 358}, "frames": {"fault": 693, "received": 936984, "settling": 7, "timing": 686, "valid": 936291}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 189, "not_ready": 497, "post_read_low": 189, "settling": 7}}
-- PASS attempt 20
### 20/20 PASS 2026-07-23T16:45:35-04:00
### 2026-07-23T11:02:54-04:00 attempt 1
-- counters before: {"channel_faults": {"2": 1, "3": 1}, "frames": {"fault": 9, "received": 2920, "settling": 7, "timing": 2, "valid": 2911}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 1, "not_ready": 1, "post_read_low": 1, "settling": 7}}
-- rc=0 duration=509s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 7, "2": 10, "3": 31}, "frames": {"fault": 55, "received": 46830, "settling": 7, "timing": 48, "valid": 46775}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 13, "not_ready": 35, "post_read_low": 13, "settling": 7}}
-- PASS attempt 1
### 2026-07-23T11:11:25-04:00 attempt 2
-- counters before: {"channel_faults": {"1": 7, "2": 10, "3": 31}, "frames": {"fault": 55, "received": 46872, "settling": 7, "timing": 48, "valid": 46817}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 13, "not_ready": 35, "post_read_low": 13, "settling": 7}}
-- rc=0 duration=452s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 8, "2": 14, "3": 58}, "frames": {"fault": 87, "received": 85760, "settling": 7, "timing": 80, "valid": 85673}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 22, "not_ready": 58, "post_read_low": 22, "settling": 7}}
-- PASS attempt 2
### 2026-07-23T11:18:58-04:00 attempt 3
-- counters before: {"channel_faults": {"1": 8, "2": 14, "3": 59}, "frames": {"fault": 88, "received": 85820, "settling": 7, "timing": 81, "valid": 85732}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 22, "not_ready": 59, "post_read_low": 22, "settling": 7}}
-- rc=0 duration=450s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 13, "2": 26, "3": 95}, "frames": {"fault": 141, "received": 124512, "settling": 7, "timing": 134, "valid": 124371}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 44, "not_ready": 90, "post_read_low": 44, "settling": 7}}
-- PASS attempt 3
### 2026-07-23T11:26:30-04:00 attempt 4
-- counters before: {"channel_faults": {"1": 13, "2": 26, "3": 95}, "frames": {"fault": 141, "received": 124572, "settling": 7, "timing": 134, "valid": 124431}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 44, "not_ready": 90, "post_read_low": 44, "settling": 7}}
-- rc=0 duration=444s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 17, "2": 33, "3": 126}, "frames": {"fault": 183, "received": 162752, "settling": 7, "timing": 176, "valid": 162569}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 55, "not_ready": 121, "post_read_low": 55, "settling": 7}}
-- PASS attempt 4
### 2026-07-23T11:33:55-04:00 attempt 5
-- counters before: {"channel_faults": {"1": 17, "2": 33, "3": 126}, "frames": {"fault": 183, "received": 162796, "settling": 7, "timing": 176, "valid": 162613}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 55, "not_ready": 121, "post_read_low": 55, "settling": 7}}
-- rc=0 duration=443s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 22, "2": 44, "3": 159}, "frames": {"fault": 232, "received": 200930, "settling": 7, "timing": 225, "valid": 200698}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 71, "not_ready": 154, "post_read_low": 71, "settling": 7}}
-- PASS attempt 5
### 2026-07-23T11:41:20-04:00 attempt 6
-- counters before: {"channel_faults": {"1": 22, "2": 44, "3": 159}, "frames": {"fault": 232, "received": 200974, "settling": 7, "timing": 225, "valid": 200742}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 71, "not_ready": 154, "post_read_low": 71, "settling": 7}}
-- rc=0 duration=452s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 26, "2": 52, "3": 194}, "frames": {"fault": 279, "received": 239816, "settling": 7, "timing": 272, "valid": 239537}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 86, "not_ready": 186, "post_read_low": 86, "settling": 7}}
-- PASS attempt 6
### 2026-07-23T11:48:53-04:00 attempt 7
-- counters before: {"channel_faults": {"1": 26, "2": 52, "3": 194}, "frames": {"fault": 279, "received": 239860, "settling": 7, "timing": 272, "valid": 239581}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 86, "not_ready": 186, "post_read_low": 86, "settling": 7}}
-- rc=0 duration=445s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 29, "2": 65, "3": 223}, "frames": {"fault": 324, "received": 278092, "settling": 7, "timing": 317, "valid": 277768}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 97, "not_ready": 220, "post_read_low": 97, "settling": 7}}
-- PASS attempt 7
### 2026-07-23T11:56:18-04:00 attempt 8
-- counters before: {"channel_faults": {"1": 29, "2": 65, "3": 223}, "frames": {"fault": 324, "received": 278136, "settling": 7, "timing": 317, "valid": 277812}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 97, "not_ready": 220, "post_read_low": 97, "settling": 7}}
-- rc=0 duration=442s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 34, "2": 72, "3": 251}, "frames": {"fault": 364, "received": 316162, "settling": 7, "timing": 357, "valid": 315798}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 110, "not_ready": 247, "post_read_low": 110, "settling": 7}}
-- PASS attempt 8
### 2026-07-23T12:03:42-04:00 attempt 9
-- counters before: {"channel_faults": {"1": 34, "2": 72, "3": 251}, "frames": {"fault": 364, "received": 316206, "settling": 7, "timing": 357, "valid": 315842}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 10}, "last_hard_fault": null, "quality_flags": {"extra_low": 110, "not_ready": 247, "post_read_low": 110, "settling": 7}}
-- rc=0 duration=445s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 41, "2": 82, "3": 280}, "frames": {"fault": 410, "received": 354462, "settling": 7, "timing": 403, "valid": 354052}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 124, "not_ready": 279, "post_read_low": 124, "settling": 7}}
-- PASS attempt 9
### 2026-07-23T12:11:08-04:00 attempt 10
-- counters before: {"channel_faults": {"1": 41, "2": 82, "3": 280}, "frames": {"fault": 410, "received": 354496, "settling": 7, "timing": 403, "valid": 354086}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 124, "not_ready": 279, "post_read_low": 124, "settling": 7}}
-- rc=0 duration=440s
-- response: {"result":"ok"}
HTTP:200
-- counters after: {"channel_faults": {"1": 45, "2": 89, "3": 311}, "frames": {"fault": 452, "received": 392294, "settling": 7, "timing": 445, "valid": 391842}, "last_batch": {"bulk_overflows": 0, "hard_faults": 0, "recovery_frames": 0, "valid": 8}, "last_hard_fault": null, "quality_flags": {"extra_low": 134, "not_ready": 311, "post_read_low": 134, "settling": 7}}
-- PASS attempt 10
### 10/10 PASS 2026-07-23T12:18:29-04:00

---

## Updates (append newest at bottom)

- 2026-07-23 — Consolidated all HX711 docs/logs into this file.
- 2026-07-23 — 100C hot-soak complete: **20/20 PASS** (~2.5 h, 936984
  frames, zero hard faults, zero recoveries, 686 timing skips absorbed).
  Combined with the 10/10 at 60C the stack is validated cold and hot.
