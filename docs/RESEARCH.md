# SmoothAF Research

This document records research, code-confirmed facts, technical inferences and hypotheses requiring physical validation. It does not enable new movement, Continuous AF, Movie AF or recording-time AF.

See [Architecture](ARCHITECTURE.md), [Testing](TESTING.md) and [Roadmap](ROADMAP.md).

## Evidence labels

- **Confirmed in code:** directly present in the reviewed Legacy or SmoothAF source.
- **Technical inference:** interpretation of code organization, without firmware guarantee.
- **Requires physical testing:** camera-, lens-, firmware-, exposure- or timing-dependent behavior.
- **Not supported:** no usable API, call site or sufficient evidence was found.

## Focus Peaking audit

The Legacy Focus Peaking implementation uses `calc_peak()` and `peak_d2xy()` with a five-point second-derivative kernel, approximately `4 * center - up - down - left - right`. Optional edge filtering reduces strong first-derivative edges treated as possible false positives. `peak_disp_filter()` and `focus_found_pixel()` consume the result for display overlays.

**Confirmed in code:** the implementation is coupled to display filters and overlay rendering. It does not expose a small stable metrics-only API that SmoothAF can safely call from its module task. Colored overlay points are therefore not a focus-distance or lens-position metric.

SmoothAF v0.3 keeps an independent integer-only Laplacian over the central 64×64 UYVY/YUV422 ROI. It imports no private overlay symbols, calls no renderer and exposes no `Peaking Experimental` option. The broad second-derivative relationship is a technical analogy, not proof that the implementations are interchangeable. A future peaking metric requires separate cost, overflow, noise and camera validation.

## Canon native AF and `lens_autofocus()`

The Legacy Lua module contains `lens_autofocus()` / `lens.autofocus()`. The observed general LiveView flow is:

1. Reject `is_manual_focus()`.
2. On some models, send `PROP_REMOTE_AFSTART_BUTTON` with value `1`.
3. Otherwise call `lens_setup_af(AF_ENABLE)`.
4. Send `MODULE_KEY_PRESS_HALFSHUTTER`.
5. Observe `lv_focus_status`, including focusing state `3` and idle/result states `1` or `2`, with bounded waits.
6. Return a boolean result.
7. Send `MODULE_KEY_UNPRESS_HALFSHUTTER` and call `lens_cleanup_af()` on the normal path.

**Confirmed in code:** this is a Lua callback, not a proven public C API for SmoothAF. The callback may block for hundreds of milliseconds or longer. Its behavior with Continuous AF is explicitly uncertain. Cleanup and result correlation are not sufficient evidence for safe invocation from a concurrent SmoothAF task.

## `lens_focus()` and focus state

The Legacy C API is:

```c
int lens_focus(int num_steps, int stepsize, int wait, int extra_delay);
```

**Confirmed in code:** `num_steps < 0` reverses direction; `stepsize` is bounded to `-3..3`; movement uses `PROP_LV_LENS_DRIVE_REMOTE`; `wait=1` waits through `PROP_LV_FOCUS_DONE`; `extra_delay` adds a delay; and the path rejects missing LiveView, MF and `is_continuous_af()`. The return value indicates that `lv_focus_error` was not set. It does not prove acquired focus or physical movement.

The Legacy code limits outstanding asynchronous requests because filling the Canon command queue can fail on some cameras. A local comment says `PROP_LV_FOCUS_DONE` does not reliably mean it is safe to send another command. `lens_focus_start()` and `lens_focus_stop()` control Follow/Rack Focus task direction; they are not a confirmed native-AF wrapper and `lens_focus_stop()` does not prove cancellation of an AF request already sent.

Relevant properties and events found in the reviewed Legacy tree include:

- `PROP_LV_FOCUS_STATUS` — LiveView focus state;
- `PROP_LV_AF_RESULT` — documented result value;
- `PROP_LV_FOCUS_STATE` — focus state values;
- `PROP_LV_FOCUS_DONE` — motor completion event with uncertain command-queue semantics;
- `PROP_LV_FOCUS_DATA` — rapidly changing focus data;
- `PROP_LV_FOCUS_STOP` — focus-stop identifier;
- `PROP_HALF_SHUTTER` — half-shutter state.

`PTP_AF_START` (`0x9160`) exists in `ptp.h`, but no local wrapper, dispatch path, parameter contract, per-request result correlation or safe cancellation path was found. Its identifier alone is not a usable SmoothAF AF API.

**Requires physical testing:** exact STM displacement, backlash, hysteresis, settling, response to adjacent commands, Canon AF timing and interaction between synthetic half-shutter and `PROP_LV_LENS_DRIVE_REMOTE`.

## Continuous AF, Movie and recording

The Legacy code distinguishes photo Continuous AF and Movie Servo AF through `is_continuous_af()`. SmoothAF and `lens_focus()` reject those states. SmoothAF also blocks Movie Mode and recording states (`RECORDING_H264`, `RECORDING_CUSTOM`, `RECORDING_RAW`). These gates remain required.

No evidence proves that EOS 600D firmware 1.0.2 accepts a module-triggered AF operation during recording, preserves exposure, keeps FPS stable, avoids LiveView interruption or supports safe cancellation. LiveView outside recording, Movie Mode in standby and active recording are separate environments. Only the first is a candidate for a future manual PoC.

Face Detection Live Mode integration was not established. The presence of Canon focus states does not prove that a module call preserves or invokes face selection.

## AF exposure / brightness question

The code audit does not establish whether the observed brightening during AF changes real ISO, shutter, aperture, analog/digital gain, preview gain only or recorded frames. Controlled tests comparing AE and manual exposure would be required. ISO, shutter, aperture, registers and internal routines must not be changed to mask the effect without a confirmed cause.

## Future Canon AF architecture

A Hybrid Canon AF Assist plus Laplacian refinement could help large defocus because Canon AF would acquire and SmoothAF would refine. This is a conceptual architecture only. It inherits trigger, state, cleanup, cancellation, exposure and movement-conflict risks.

Any future PoC must be incremental:

1. **Canon AF Trigger:** one manual operation in LiveView outside recording, no simultaneous `lens_focus()`, with logging of request, status, result, timeout and cleanup; no repetition.
2. **Canon AF Repeat:** only after complete state observation and conflict-free PoC 1; wait for final state and cooldown before each request.
3. **Focus Monitor:** only after PoC 2; use hysteresis, a consistent loss window, cooldown and operation limits; do not treat `PROP_LV_FOCUS_DATA` alone as acquired focus.
4. **Video Compatibility:** only after separate standby tests; active recording remains blocked until exposure, frames, FPS, latency and cancellation are specifically validated.

The required state machine would include `CANON_AF_IDLE`, `REQUESTED`, `FOCUSING`, `DONE`, `FAILED`, `TIMEOUT` and `CANCEL_UNSUPPORTED`, a single active request ID, mutual exclusion with all lens controllers, external timeout semantics that do not claim to cancel Canon firmware, guaranteed cleanup and no movement after Stop, timeout or invalid result.

## SmoothAF v0.5 passive Vlog Monitor

The v0.5 monitor is a read-only viewer over manually produced measurements. A centralized publication path keeps SAD and Laplacian values separate, rejects invalid samples as valid scores, and supplies only new valid samples to an active monitoring session. Start resets the session reference, smoothing and sample count; Stop freezes the monitor-owned values. It displays a conservative smoothed/reference view and reports `manual/N/A` rather than claiming a periodic sampling interval. It does not create a task, read VRAM from a menu callback, call `lens_focus()` or invoke Canon AF. Missing or invalid samples remain outside the valid sample count. These states are heuristics and require physical validation; they do not prove focus quality.

An asynchronous periodic monitor remains a future roadmap item. It must first be evaluated for DIGIC IV cost, timing, invalid-frame behavior, scene changes and strict exclusion from automatic lens movement.

## Current recommendation

The v0.4 branch reserves a host-testable, unavailable Canon AF Assist adapter only. It does not implement hardware movement because no safe C-level interface is proven. Do not call the static Lua callback, assemble its primitives in a new task, use `PTP_AF_START` as an API, or treat `lens_focus()` as Native AF Assist.

Do not implement Continuous AF or Hybrid AF. Keep Legacy HC as default/fallback, Adaptive HC experimental and Normal AF bounded single-shot. A future native implementation requires a proven C-level interface and state/result correlation for `600D.102`, followed by cleanup, exclusion and physical tests outside recording. No benchmark can prove physical compatibility, exposure stability, STM behavior or recording safety.
