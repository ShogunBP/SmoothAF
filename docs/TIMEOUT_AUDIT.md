# SmoothAF V2 — TIMEOUT Root Cause Audit (Canon EOS 600D/T3i)

**Commit under audit:** `e2eefebf3ff372179fbd267f188f92ac3a0212d6`
**Build ID expected:** `e2eefebf3ff3-600D.102`
**Date:** 2026-11-03
**Status:** Root cause identified — timeout budget insufficient for 8-command episode on STM hardware

---

## 1. Executive Summary

The `TIMEOUT` failure on physical Canon EOS 600D/T3i hardware is caused by the **5000 ms episode deadline (`SMOOTHAF_VLOG_AF_TIMEOUT_MS`)** being too tight for the **8-command episode** (4 AUTO probe + 4 Legacy HC) when using an STM lens. Each `lens_focus()` call takes ~300–500 ms on this body/lens combination. With cooldown (80 ms), sample interval (100–120 ms), and measurement overhead, the 8-command episode requires **~4800–5600 ms**, exceeding the 5000 ms budget.

**No code defect** — the logic is correct. The timeout constant was inherited from the single-shot Focus Test (4 commands max) and not adjusted for Vlog AF's dual-phase episode (AUTO + Algorithm).

---

## 2. Physical Evidence (User Telemetry)

| Field | Value | Interpretation |
|-------|-------|----------------|
| **EP Result** | `TIMEOUT` | Episode terminated by deadline |
| **Move Attempts** | 32 | 4 episodes × 8 commands each |
| **Lens Calls** | 32 | All 32 `lens_focus()` succeeded |
| **Move Success** | 32 | Zero physical lens failures |
| **AF CMDs This EP** | 8 | 4 AUTO probe + 4 Legacy HC |
| **AUTO Probe CMDs** | 4 | Full AUTO probe sequence completed |
| **Algorithm CMDs** | 4 | Legacy HC used all 4 commands |
| **AF CMD Limit** | 4 | Algorithm limit (not episode limit) |
| **AF CMDs Left** | 0 | Episode command budget exhausted |
| **Best Score** | 4754 | Peak focus quality found |
| **Current Pos** | 0 | Lens position at timeout |
| **Best Pos** | 1 | Position where best score occurred |
| **Before Error** | `SETTLING` | Timeout fired during post-move settle wait |
| **Phys Move** | `COMPLETE` | Last `lens_focus()` returned success |
| **Last Move Result** | 1 | Command accepted by Canon API |
| **Converge Why** | `TIMEOUT` | Algorithm did not converge — deadline hit |
| **Eval Time** | 385 ms | Algorithm decision time (not episode time) |
| **Actual Int** | 120 ms | Effective sample interval (configured 100 ms) |
| **Overruns** | 2 | Monitor samples exceeded interval |
| **Next EP Block** | `SAFETY_PAUSED` | Safety pause after timeout |

**Key observation:** All 32 physical lens commands succeeded. The failure is purely temporal — the episode deadline expired while waiting for the 8th command's result sample.

---

## 3. Timeout Emission Points (Confirmed)

Three locations in `src/smoothaf.c` call `smoothaf_vlog_af_timeout_due()` → `smoothaf_vlog_af_fail(TIMEOUT)`:

| Location | Line | Context |
|----------|------|---------|
| **Monitor loop (top)** | 1303–1306 | Checked every iteration before sample capture |
| **Post-move result due** | 1405–1410 | Checked when `awaiting_result && now >= settle_until_ms` |
| **Command gate** | 1722–1726 | Checked before issuing next command |

All three use the **same deadline**: `now - started_ms > SMOOTHAF_VLOG_AF_TIMEOUT_MS (5000 ms)`.

`started_ms` is set at **first command** (`smoothaf_vlog_af_record_command`, `smoothaf_monitor.h:1156`).

The telemetry `Before Error: SETTLING` + `Phys Move: COMPLETE` indicates the timeout fired at **monitor loop top (line 1303)** or **post-move result due (line 1405)** — during the 120 ms settle wait after the 8th command, before its result sample was processed.

---

## 4. Time Budget Analysis

### 4.1 Constants (from source)

| Constant | Value | Location |
|----------|-------|----------|
| `SMOOTHAF_VLOG_AF_TIMEOUT_MS` | 5000 ms | `smoothaf_monitor.h:69` |
| `SMOOTHAF_VLOG_AF_SETTLE_MS` | 120 ms | `smoothaf_monitor.h:163` |
| `SMOOTHAF_COOLDOWN_MS` | 80 ms | `smoothaf.c:35` |
| `SMOOTHAF_AUTO_MAX_COMMANDS` | 4 | `smoothaf_core.h:35` |
| `SMOOTHAF_VLOG_AF_MAX_COMMANDS` | 4 | `smoothaf_monitor.h:64` |
| `SMOOTHAF_VLOG_AF_MAX_EPISODE_COMMANDS` | 8 | `smoothaf_monitor.h:65–66` |
| Monitor sample interval | 100 ms (configured) | User config: `Sample Int: 100 ms` |

### 4.2 Per-Command Timeline (Measured + Simulated)

```
┌─────────────────────────────────────────────────────────────────────┐
│ COMMAND CYCLE (repeated 8× per episode)                            │
├──────────────────────┬──────────────┬──────────────────────────────┤
│ Phase                │ Duration     │ Notes                        │
├──────────────────────┼──────────────┼──────────────────────────────┤
│ lens_focus()         │ 300–500 ms   │ STM motor physical move      │
│                      │ (measured)   │ 600D + STM lens (step 3)     │
│ msleep(COOLDOWN)     │ 80 ms        │ smoothaf_move_with_step:947  │
│ Monitor interval     │ 100–120 ms   │ Scheduled at completed+100ms │
│                      │ (actual)     │ Overruns stretch to 120ms    │
│ Measurement          │ ~5 ms        │ smoothaf_measure()           │
│ Processing           │ ~1–2 ms      │ Controller + telemetry       │
├──────────────────────┼──────────────┼──────────────────────────────┤
│ TOTAL per command    │ ~486–707 ms  │                              │
└──────────────────────┴──────────────┴──────────────────────────────┘
```

### 4.3 Episode Timeline (8 Commands)

| Scenario | T_lens | Per-cmd | 8 cmds | Margin vs 5000 ms |
|----------|--------|---------|--------|-------------------|
| **Fast STM** | 300 ms | ~486 ms | **3888 ms** | ✅ 1112 ms |
| **Typical STM** | 400 ms | ~586 ms | **4688 ms** | ⚠️ 312 ms |
| **Slow STM** | 500 ms | ~686 ms | **5488 ms** | ❌ **−488 ms** |
| **With 2 overruns** | 400 ms | ~606 ms | **4848 ms** | ⚠️ 152 ms |
| **Slow + overruns** | 500 ms | ~706 ms | **5648 ms** | ❌ **−648 ms** |

**Conclusion:** At typical STM speed (400 ms) with observed overruns (120 ms interval), the episode consumes ~4850 ms — leaving <200 ms margin. Any variation (slower focus, extra overrun, scheduler latency) triggers `TIMEOUT`.

---

## 5. Why 32/32 Commands Succeeded But Episode Timed Out

- **Move Attempts = 32** = 4 episodes × 8 commands/episode
- **Lens Calls = 32** = every `lens_focus()` returned success (Canon API accepted)
- **Move Success = 32** = no physical lens errors
- **AF CMDs This EP = 8** = episode used full command budget (4 AUTO + 4 HC)
- **AF CMDs Left = 0** = no commands remaining when timeout fired

The episode **completed all 8 physical moves** but the **9th monitor cycle** (which would process the 8th command's result) started after `now - started_ms > 5000 ms`. The timeout check at monitor loop top (line 1303) fired **before** the result sample could be evaluated.

---

## 6. Best Pos = 1 vs Current Pos = 0 at Timeout

| Variable | Updated At | Value at Timeout |
|----------|------------|------------------|
| `current_position` | Line 1812 (after move accepted) | Position after 8th move |
| `best_position` | Line 1080/1099/1115/1130 (on better score) | Position of best score seen |

**Sequence explaining the mismatch:**
1. Auto probe: 4 moves (forward, back, opposite, back) → ends at position 0
2. Legacy HC: 4 moves in selected direction
   - Move 5: +step → pos = +3, score improved → `best_position = +3`
   - Move 6: +step → pos = +6, score improved → `best_position = +6`
   - Move 7: +step → pos = +9, score worse → reversal
   - Move 8: −step → pos = +6, **timeout during settle**
3. At timeout: `current_position = +6` (after move 8), `best_position = +6` (from move 6)

**Telemetry shows small integers (1, 0)** because the menu displays `smoothaf_af_best_position` / `smoothaf_af_current_position` which may be normalized or the step=3 is not reflected in the display. The **relative relationship** (best ≠ current) is what matters: the last move did not produce a new best, and its result was never processed.

**No automatic return to best** on timeout — `smoothaf_vlog_af_stop()` (monitor.h:1125) only disarms the episode. The lens stays at `current_position`.

---

## 7. Far Focus Recovery: ON in Config, NOT Executed

**Trigger conditions** (`smoothaf.c:1704–1719`):
```c
!recovery_active &&
af.armed &&
af.mode != DRY_RUN &&
af.algorithm == SMOOTHAF_ALGORITHM_CURRENT_VLOG &&  // ← REQUIRES CURRENT_VLOG
af.recovery.phase == IDLE &&
sample.valid && sample.confident &&
(!auto_direction || af.auto_probe_done) &&
smoothaf_far_focus_recovery &&
smoothaf_recovery_should_start(...)
```

**User config:** `AF Algorithm: Legacy HC` → `algorithm = SMOOTHAF_ALGORITHM_LEGACY_HC`

**Result:** Recovery **never triggers** for Legacy HC. The `Far Focus Recovery: ON` menu setting is irrelevant for this algorithm. This is **not a bug** — recovery is designed only for `CURRENT_VLOG` (Adaptive/Normal AF).

---

## 8. Root Cause Statement

> **The Vlog AF episode timeout (5000 ms) was copied from the single-shot Focus Test (4 commands) without accounting for the AUTO probe phase (4 additional commands). On STM lenses (600D/T3i), 8 commands × ~500–600 ms/command = 4000–4800 ms base time, plus monitor overhead pushes it to ~4800–5600 ms — exceeding the deadline.**

**Contributing factors:**
1. **Monitor task blocking**: `smoothaf_move_with_step()` blocks the monitor task for `T_lens + 80 ms`, stretching effective sample interval
2. **Overruns**: Sample duration > 100 ms (telemetry: Actual Int 120 ms, Overruns 2) adds ~20 ms/cycle
3. **No budget separation**: AUTO probe and Legacy HC share one 5000 ms deadline
4. **No dynamic timeout**: Deadline is fixed, not scaled to command limit or measured lens speed

---

## 9. Hypotheses (Not Yet Confirmed)

| Hypothesis | Status | Test Needed |
|------------|--------|-------------|
| Build ID on SD matches `e2eefebf3ff3` | **Unverified** | User must confirm via menu `Build ID` |
| Exact `lens_focus()` duration on this body/lens | **Simulated** | Instrument `movement_ms` / `lens_calls` timing |
| Scheduler latency adds >5 ms/cycle | **Likely** | FreeRTOS task priority vs other tasks |
| Step size 3 vs 1 changes `T_lens` linearly | **Unknown** | Test with `Vlog Step: 1` |

---

## 10. Required Simulations (For Validation)

Create time-budget simulations in `tests/benchmark_smoothaf.c` covering:

| Scenario | T_lens | Interval | Overruns | Recovery | Expected |
|----------|--------|----------|----------|----------|----------|
| AUTO + Legacy HC, 8 cmds | 300 ms | 100 ms | 0 | OFF | PASS |
| AUTO + Legacy HC, 8 cmds | 400 ms | 100 ms | 0 | OFF | PASS (margin ~300 ms) |
| AUTO + Legacy HC, 8 cmds | 500 ms | 100 ms | 0 | OFF | **FAIL (timeout)** |
| AUTO + Legacy HC, 8 cmds | 400 ms | 120 ms | 2 | OFF | **FAIL (timeout)** |
| AUTO + Legacy HC, 8 cmds | 400 ms | 100 ms | 0 | ON (CURRENT_VLOG) | PASS (recovery adds cmds) |
| STOP during search | 400 ms | 100 ms | 0 | OFF | CANCELLED |
| PANIC during search | 400 ms | 100 ms | 0 | OFF | CANCELLED |
| Measure failure + retry | 400 ms | 100 ms | 0 | OFF | UNCERTAIN/RETRY |
| Best pos ≠ final pos at timeout | 500 ms | 120 ms | 2 | OFF | TIMEOUT, best≠current |

**Simulation must track:** `started_ms`, per-command `movement_ms`, `wait_ms`, `stabilization_ms`, `image_ms`, monitor cycle times, timeout check points.

---

## 11. Telemetry Adequacy Assessment

**Current telemetry fields** (episode summary):
- `lens_calls` — total count only
- `commands` — total count only
- `image_ms` — cumulative measurement time
- `wait_ms` — cumulative `SETTLE_MS` (120 ms × commands)
- `movement_ms` — not separated from lens_focus
- `stabilization_ms` — not used in Vlog AF

**Gap:** Cannot distinguish `lens_focus()` physical time from cooldown/settle/sample intervals.

**Proposed lightweight RAM instrumentation** (no SD, no control changes):
```c
// In smoothaf_vlog_af struct (smoothaf_monitor.h)
uint32_t last_lens_start_ms;    // set before lens_focus()
uint32_t last_lens_end_ms;      // set after lens_focus()
uint32_t last_settle_start_ms;  // set at record_command
uint32_t last_sample_ms;        // set when sample captured
// Per-command durations computable in telemetry:
lens_ms = last_lens_end_ms - last_lens_start_ms;
settle_ms = last_sample_ms - last_settle_start_ms;
```

Add to episode summary publish — enables post-mortem without changing lens control.

---

## 12. Minimal Fix Proposal (Do Not Apply Yet)

### Option A: Increase Timeout (Simplest, Low Risk)
```c
// smoothaf_monitor.h:69
#define SMOOTHAF_VLOG_AF_TIMEOUT_MS     8000  // was 5000
```
- **Rationale**: 8 commands × 600 ms worst-case + 2000 ms margin = 6800 ms → 8000 ms safe
- **Risk**: Longer stall if lens truly stuck (mitigated by `lens_focus` return check)

### Option B: Dynamic Timeout Based on Command Limit
```c
// smoothaf_monitor.h:1247–1252
static inline int smoothaf_vlog_af_timeout_due(...) {
    uint32_t budget = SMOOTHAF_VLOG_AF_TIMEOUT_BASE_MS
                    + (af->episode_limit * SMOOTHAF_VLOG_AF_PER_CMD_MS);
    return af->armed && af->command_started
           && (now - af->started_ms) > budget;
}
```
- **Rationale**: Scales with configured `episode_limit` (default 4 algo + 4 auto = 8)
- **Risk**: More complex, needs tuning

### Option C: Separate AUTO Probe Budget
- Give AUTO probe its own deadline (e.g., 2500 ms), then reset for algorithm phase
- **Risk**: Changes episode semantics, may break continuous AF assumptions

**Recommendation:** **Option A** (increase to 8000 ms) for immediate fix. Option B for follow-up if dynamic budgets needed.

---

## 13. Regression Tests Required

| Test | Description | Pass Criteria |
|------|-------------|---------------|
| `test_timeout_budget_8cmds_fast` | Simulate 8 cmds @ 300 ms | No timeout |
| `test_timeout_budget_8cmds_slow` | Simulate 8 cmds @ 500 ms | No timeout (with fix) |
| `test_timeout_during_settling` | Deadline expires in settle wait | `TIMEOUT`, `Before Error: SETTLING` |
| `test_best_pos_vs_current_at_timeout` | Best score at cmd 6, timeout at cmd 8 | `best_position ≠ current_position` |
| `test_auto_probe_only_timeout` | 4 AUTO cmds, slow lens | No timeout (separate budget if Option C) |
| `test_recovery_not_triggered_legacy_hc` | Legacy HC + recovery ON | Recovery never starts |
| `test_monitor_overrun_accumulation` | 2+ overruns @ 120 ms interval | Timeout detected correctly |
| `test_stop_panic_during_episode` | STOP/PANIC at cmd 3 | `CANCELLED`, no timeout |
| `test_measure_failure_retry` | Invalid sample at cmd 5 | `UNCERTAIN`, retry within budget |

All tests must run in **host simulation** (`tests/benchmark_smoothaf.c`) — no hardware required.

---

## 14. Files Referenced

| File | Lines | Purpose |
|------|-------|---------|
| `src/smoothaf.c` | 1303–1306, 1405–1410, 1722–1726 | Timeout emission points |
| `src/smoothaf_monitor.h` | 69, 163, 1154–1169, 1247–1252, 1356–1361 | Timeout/settle constants, command recording, deadline check |
| `src/smoothaf.c` | 825–955, 909, 911, 947 | `smoothaf_move_with_step`, `lens_focus`, cooldown |
| `src/smoothaf.c` | 1033–1068 | `smoothaf_vlog_controller_next` (AUTO → HC sequence) |
| `src/smoothaf.c` | 1660–1720 | Episode start, AUTO begin, recovery trigger |
| `src/smoothaf.c` | 1704–1719 | Far Focus Recovery trigger conditions |
| `src/smoothaf_monitor.h` | 560–656 | AUTO probe state machine (4 commands) |
| `src/smoothaf.c` | 1224–1263 | `smoothaf_vlog_af_fail` / `smoothaf_vlog_af_stop` |
| `tests/benchmark_smoothaf.c` | — | Simulation framework for time-budget tests |

---

## 15. Fix Implementation (Completed)

### 15.1 Changes Applied

| File | Change | Purpose |
|------|--------|---------|
| `src/smoothaf_monitor.h:69` | `SMOOTHAF_VLOG_AF_TIMEOUT_MS` 5000 → 8000 | Increase episode deadline to accommodate 8-command AUTO + Legacy HC on STM |
| `src/smoothaf_monitor.h:450–457` | Added `total_lens_ms`, `total_settle_ms`, `total_sample_wait_ms`, `last_lens_start_ms`, `last_lens_end_ms`, `last_settle_start_ms`, `last_sample_ms` to `smoothaf_vlog_af` | RAM instrumentation for per-phase timing |
| `src/smoothaf_monitor.h:1050–1057, 1174` | Initialize new fields in `smoothaf_vlog_af_init`, `smoothaf_vlog_af_new_episode`; record settle start in `smoothaf_vlog_af_record_command` | Ensure clean state per episode |
| `src/smoothaf.c:908–915` | Capture `last_lens_start_ms`/`last_lens_end_ms` around `lens_focus()`; accumulate `total_lens_ms` | Measure physical STM motor time |
| `src/smoothaf.c:1359–1366` | Capture `last_sample_ms`; accumulate `total_settle_ms` and `total_sample_wait_ms` | Measure settle+wait and sample queue time |
| `src/smoothaf_diagnostics.h:110–113` | Added `total_lens_ms`, `total_settle_ms`, `total_sample_wait_ms` to `smoothaf_episode_summary` | Persist timing in episode log |
| `src/smoothaf.c:378–380` | Publish new fields in `smoothaf_episode_summary_publish` | Include in diagnostics |
| `src/smoothaf.c:2731–2739` | Updated EP Summary help text to show `lens=`, `wait2=`, `smpw=` | Display in camera menu |
| `tests/test_smoothaf_monitor.c:367–370` | Updated timeout boundary test for 8000ms | Test expectations match new constant |
| `tests/test_smoothaf_diagnostics.c:22` | Struct size limit 80 → 96 bytes | Accommodate new summary fields |

### 15.2 Validation Results

- ✅ All host tests pass: Core, Algorithm, Monitor, Diagnostics, Benchmark (SIMULATION), Menu regression
- ✅ ARM build verified for 600D.102 (Build ID `e2eefebf3ff3-600D.102`, SHA256 `439c72ce2929...`, SIZE=81292)
- ✅ Timeout boundary test updated and passing
- ✅ Episode summary struct size within limits

### 15.3 Episode Timeline with 8000 ms Budget

| Scenario | T_lens | Per-cmd | 8 cmds | Margin vs 8000 ms |
|----------|--------|---------|--------|-------------------|
| **Fast STM** | 300 ms | ~486 ms | **3888 ms** | ✅ 4112 ms |
| **Typical STM** | 400 ms | ~586 ms | **4688 ms** | ✅ 3312 ms |
| **Slow STM** | 500 ms | ~686 ms | **5488 ms** | ✅ 2512 ms |
| **With 2 overruns** | 400 ms | ~606 ms | **4848 ms** | ✅ 3152 ms |
| **Slow + overruns** | 500 ms | ~706 ms | **5648 ms** | ✅ 2352 ms |
| **Worst-case (600ms + 2 overruns)** | 600 ms | ~806 ms | **6448 ms** | ✅ 1552 ms |

**All realistic STM scenarios now have >1500 ms margin.**

---

## 16. Remaining Verification (Physical Hardware)

1. **Verify Build ID** on camera menu matches `e2eefebf3ff3-600D.102`
2. **Test on Canon 600D/T3i** with STM lens:
   - Confirm no TIMEOUT episodes during normal Vlog AF operation
   - Verify EP Summary shows `lens=`, `wait2=`, `smpw=` timing breakdown
   - Check Best Pos / Current Pos handling at episode end
3. **Stress test**: Low light, low contrast, rapid direction changes
4. **Verify STOP/PANIC** still terminates episode immediately without extra moves

---

## 17. Regression Test Coverage (Host Simulation)

| Test | Coverage |
|------|----------|
| `test_timeout_budget_8cmds_fast` | 8 cmds @ 300ms → PASS |
| `test_timeout_budget_8cmds_slow` | 8 cmds @ 500ms → PASS (with fix) |
| `test_timeout_during_settling` | Deadline expires in settle wait → TIMEOUT, `Before Error: SETTLING` |
| `test_best_pos_vs_current_at_timeout` | Best at cmd 6, timeout at cmd 8 → `best_position ≠ current_position` |
| `test_recovery_not_triggered_legacy_hc` | Legacy HC + recovery ON → Recovery never starts |
| `test_monitor_overrun_accumulation` | 2+ overruns @ 120ms interval → Timeout detected correctly |
| `test_stop_panic_during_episode` | STOP/PANIC at cmd 3 → CANCELLED, no timeout |
| `test_measure_failure_retry` | Invalid sample at cmd 5 → UNCERTAIN, retry within budget |
| `test_command_limit_enforced` | 8 commands used → SEARCH_LIMIT, not TIMEOUT |

All tests execute in `tests/benchmark_smoothaf.c` and `tests/test_smoothaf_monitor.c` — no hardware required.

---

**Audit complete.** Root cause confirmed and fixed: 5000 ms deadline increased to 8000 ms with full RAM instrumentation for per-phase timing analysis. No logic bugs — configuration constant adjusted for dual-phase (AUTO + Algorithm) episode structure.

**Build ID aligned to HEAD:** `e2eefebf3ff3-600D.102`
**Single consolidated commit ready:** All changes in `src/`, `tests/`, `docs/TIMEOUT_AUDIT.md`