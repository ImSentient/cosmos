# HX711S combined-sample rewrite — james's hx711s-new2 + paul/hx711s-new3 (attempt 3)

Era: 2026-07-26. James rewrote his driver as `hx711s-new2` (`581e692f`,
"read adcs independently"): one timer per chip, each chip read on its own
DRDY, the primary chip paces a single emitted sample that carries the sum
of all 4 load cells (secondaries held at their latest reading). All-high
frames are held instead of errored. Earlier eras: `HX711-paul-attempt1.md`
(old stack), `HX711-paul-attempt2.md` (our DRDY rewrite on his v1, PR #6).
Audit/background: `HX711.md`.

## v2 evaluation: 5x mesh campaign on `581e692f` (2026-07-26)

Built via cosmos-nightly (branch=hx711s-new2, PR r20), flashed to carbon2u
(all 3 MCUs verified `Kalico 581e692f-dirty`). 5x back-to-back
`BED_MESH_CALIBRATE` @ 60 C with full raw force capture (197k samples):

- **5/5 PASS, 465/465 taps valid.** Zero bad taps, zero retries, zero
  pre-movement triggers, zero recoveries, zero glitch drops.
- **Repeatability:** median per-point spread 57 um, max 120 um. Worst
  points are corners and drift monotonically run-to-run (e.g. (246,246):
  0.199 -> 0.319 mm) = thermal baseline wander, not probe noise.
- **Impulse class still present and unguarded:** 3 single-sample blips
  >=75 g (76-78 g, one sample wide, instantly reverting) captured in
  travel/arm zones. Same class that killed campaigns on the old base
  (phantom trigger -> TAP_CHRONOLOGY; +87 g in the arming window ->
  "triggered prior to movement"). Per-chip DRDY does not change it
  (mechanical, not acquisition); these 3 missed arming instants by luck.
- v2 carries no torn-read verify/retry, no desync recovery (fatal
  last_error latch -> host force-restart), no stuck watchdog, no impulse
  filtering.

Artifacts: `/home/paul/carbon/cosmos-nightly/hx711s-new2-artifacts/`
(REPORT.md, campaign.log, force.jsonl.gz, klippy.log.gz).

## paul/hx711s-new3: hardening port (4 commits, local worktree)

`~/carbon/kalico-hx711s-new3`, branch `paul/hx711s-new3` on `581e692f`,
unpushed. Each commit builds klipper.bin clean; amended via rebase-edit.

| Commit | Layer |
|---|---|
| `3554f486` | torn-frame verify + bounded in-place re-read (`torn_retries` option) |
| `bde495c8` | in-driver desync/overflow recovery: power cycle, one-shot RECOVERED marker, held-sum keeps probe watchdog fed |
| `ce06a5c7` | stuck watchdog, per-chip + rate-aware (`stuck_timeout_ms`, derived from sample_rate) |
| `5118d01a` | impulse hold-confirm on the trigger feed (`impulse_threshold_counts`, default 5775) |

All three tuning values ride one `hx711s_set_tuning` MCU command sent at
config time; defaults preserve validated behavior; documented in
docs/Config_Reference.md.

## Offline replay -> threshold choice

Python model of the impulse filter replayed over the 197k-sample v2
capture:

| Threshold | Impulses surviving | Real taps lost | Trigger latency |
|---|---|---|---|
| 67 g | 1/3 leaked (75.9 g off +11 g baseline) | 0/465 | ~0 ms median |
| 55 g | 0/3 | 0/465 | uniform 1 sample (11.6 ms) |

75.9 g impulse slipped a 67 g threshold because it rose from an elevated
baseline (delta 65 g < 66.7 g). Default set to 5775 counts (~55 g):
trigger_force (75 g) minus the +/-20 g baseline noise observed between
moves. Uniform 1-sample latency ~= 23 um deeper contact, mesh-neutral,
absorbed by z_offset.

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

## 2026-07-26 (late) — external review fixes (6 findings, rebase-edits)

Second review pass (6 findings) closed via rebase-edit amends; stack is
still 4 commits: `3554f486` / `b24e23a6` / `e79848fd` / `e923087a`.

| Finding | Fix | Commit |
|---|---|---|
| 50ms settle wrong at 10 SPS (datasheet 400ms); post-cycle first read is reset-default A-128 | rate-aware `settle_ticks` (4 conversions + margin) + consume-and-discard first 4 conversions after wake (also covers gain-select programming) | `b24e23a6` |
| bad_frame chips feed stale mixed-age sums to the probe | bad-frame streak >2 -> recovery (bounded hold instead of infinite; rejected fail-move: transient faults must not nuke prints) | `b24e23a6` |
| stuck-low DOUT: infinite benign retries + liveness refresh = watchdog never fires | bad-streak -> recovery; recovery streak >3 -> fatal DESYNC latch (honest end-state for a dead chip) | `b24e23a6` |
| fixed 50ms pause aborts HX717 320 SPS homing (watchdog ~16ms) | same rate-aware settle: 4 periods at 320 SPS = 12.5ms | `b24e23a6` |
| capture task only woken on pending; watchdog unenforced when no chip presents DRDY | wake task on every chip event (also lets the recovery held-feed run during settle) | `e79848fd` |
| host `break` on RECOVERED marker dropped valid batch tail | `continue` | `b24e23a6` |

New tuning option: `settle_ms` (default derived from sample_rate). Full
set_tuning signature: `torn_retries stuck_ms spike_sum_threshold
settle_ms`. All commits build klipper.bin clean; on-device validation
pending (Paul's call).
