# Session handoff prompt

Copy everything below the line into a fresh agent session working in the repository root.

---

You are the lead systems architect and research engineer for **FileFlow**, a native
phone-to-phone optical data-transfer project. The working directory is
the checkout of this repository. Substantial work already exists — read this whole prompt
before touching anything.

## The project

Transfer arbitrary files from the **display of one Android phone** to the **camera of another**
using no Wi-Fi, cellular, Bluetooth, NFC, USB or accessory. The only channel is light. The goal
is **maximum verified application-layer goodput**, replacing rapidly-changing QR streams with a
purpose-built screen-to-camera protocol.

Read these first, in order:

1. `docs/INDEX.md` — the map of all 53 documents
2. `docs/vision/PERFORMANCE-PHILOSOPHY.md` — the metric discipline, non-negotiable
3. `docs/experiments/PHASE1-FINDINGS.md` — **55 findings; this is the project's memory**
4. `CONTRIBUTING.md` — evidence tags and the rules below
5. `docs/architecture/COMPONENT-REGISTRY.md` — component status, C01–C20

## Operating rules — these are not style preferences

- **Never report a target as an achievement.** 200 KB/s is a milestone, not a result.
- **Never delete an unfavourable result.** F14 is a retraction kept in full because the
  retraction is more instructive than the finding was. Do the same.
- **Always name the metric.** Six different rates get confused in this domain (display refresh,
  display state rate `Fd`, camera fps, raw symbol rate, corrected bit rate, payload goodput).
  An unlabelled number is a documentation defect.
- **Tag every claim**: `[FACT]` (primary source, cite it), `[LIT]` (published, cite + access
  note), `[HYP]` (unvalidated, needs a linked experiment), `[OPEN]`.
- **Every simulator number is `[HYP]`.** The simulator is uncalibrated — RISK-024, and the
  second-highest risk in the register.
- **Measure, don't assume.** Several findings below exist because a plausible hypothesis was
  wrong. Where you can run the experiment, run it.
- No design decision may live only in a chat log. It goes in an ADR, a finding, or a registry.

## Build and verify

```bash
cmake --preset desktop-release && cmake --build build/desktop-release -j8
./build/desktop-release/core/tests/fileflow_tests          # must show 284 passing

cmake --preset desktop-asan   && cmake --build build/desktop-asan -j8
./build/desktop-asan/core/tests/fileflow_tests             # ASan+UBSan, must be clean

cmake --preset core-only      && cmake --build build/core-only -j8   # enforces ADR-0010
python3 tools/check_links.py                               # every link must resolve
```

Presets: `desktop-debug`, `desktop-release`, `desktop-asan`, `desktop-fuzz`, `core-only`.
CI has 7 jobs. **Apple clang ships no libFuzzer**, so the 5 fuzz targets run on Linux CI only;
there are deterministic in-suite equivalents for macOS.

Tools:
- `build/desktop-release/sim/ffsim` — channel simulator, `--help` for flags
- `build/desktop-release/harness/ffreplay` — replay a recorded capture bundle
- `tools/grid_sweep.sh`, `tools/grid_fit.py`, `tools/adapt_sweep.sh`,
  `tools/perf_model/perf_model.py`, `tools/check_links.py`

## What exists

**Portable C++20 in `core/`** — no Android headers, enforced by the `core-only` preset:
GF(256) + Reed–Solomon **with errors-and-erasures decoding**, a systematic GF(2) random linear
fountain code (protocol v2, ADR-0015), frame/header codec with erasure-assisted decoding (F50),
M0 binary luminance modulation with soft symbols, intra-frame FEC (interleaved RS), the
**session layer C13a** (`session.cpp`: one transmit path, one receive path, in-band manifest,
SHA-256 verification), the full CV path: homography (DLT + Hartley
normalisation), screen detection, frame-to-frame tracking, subpixel cell sampling, photometric
field with pilot-residual confidence — and the C14 adaptive link controller (`link.cpp`:
receiver-side channel estimation plus goodput-optimal code-rate selection with hysteresis,
reporting only — see F20).

`FramePipeline` (`core/src/pipeline.cpp`) is the **single shared image→symbols stage**, and
`TransmitSession` / `ReceiveSession` (`core/src/session.cpp`) the single shared cells↔file stage.
The simulator, the replay harness, the JNI transmitter and eventually the live receiver all drive
them. Do not duplicate either — both were duplicated once (F52), and the copy is exactly where the
simulator stopped measuring the receiver that ships. A receiver is told the **grid and nothing
else**; anything else you hand it in a test is something a real capture cannot give it.

**`sim/`** — 3D viewing geometry (yaw/pitch/roll/distance), impairments, `ffsim --image-path`
runs the real geometry chain, `--record DIR` writes capture bundles.

**`harness/`** — capture bundles (`capture.meta` + raw `.gray` frames), `ReplaySource`,
`ffreplay`. Proven **bit-identical to live decode**.

A file transfer verifies end-to-end through the complete chain under perspective, jitter, blur,
glare and falloff.

## Findings that will save you time

Full detail in `docs/experiments/PHASE1-FINDINGS.md`. The ones that change how you work:

- **F52: until 2026-10-23 no real capture could have completed a transfer.** The manifest was
  never transmitted and every receiver was handed the transmitter's. Fixed by the in-band manifest
  (ADR-0015). The general lesson: a test that supplies what the real system must earn tests
  nothing.
- **F49: the fountain code, not the channel, cost 20–35% of goodput under loss** — and swapping in
  Gaussian elimination would not have found it, because the waste was in the LT rows. Dense GF(2)
  rows fixed it (ADR-0015).
- **F53: a moving phone loses frames to motion BLUR, not to the tracker.** Constant-velocity
  prediction was built, measured, and shipped off — it changed nothing. The remedy for motion is
  telling the person (F54), and `ffsim --shake/--pan` is how to test any change against it.
- **F51: alignment objectives need a gradient where it matters.** Scored through the decode
  window, the pilot-contrast search is flat for errors under ~0.3 cells. Score on *every*
  structural cell through a wider window — they are all known, so F15 still holds.

- **F14 is RETRACTED.** A "~6 px/cell detection floor" was an artifact of the F15 bug, and the
  corrected boundary turned out to be a `min_area_fraction` config threshold, not optics.
  **Do not plan grid geometry around any detection floor.**
- **F16: EXP-001 cannot be answered in simulation.** The grid sweep found no density cliff down
  to 5.00 px/cell — because the renderer models neither display subpixel structure, sensor MTF,
  moiré nor diffraction, which are the mechanisms that *cause* a cliff. A simulator omitting
  that physics is guaranteed to report no cliff. **This needs real captures.**
- **F15: localisation must never depend on payload content.** It did once (an area test on
  lit-pixel count), and zero-padded frames became undetectable. Geometry reads the boundary
  ring and markers only.
- **F18: intra-frame FEC is load-bearing, not an optimisation.** Without it the transfer does
  not complete on an impaired channel at all. Erasure decoding is where the value is — RS
  corrects `nsym` erasures but only `nsym/2` errors, and the receiver already knows which cells
  it could not read.
- **F19: soft-decision input buys nothing for M0.** Symbol error rate is 0.00000 up to noise
  amplitude 120; failures are structural (occlusion, tracking, photometric), not decisional.
  Expect this to change for M2, where levels sit at one third the spacing.
- **F13: the tracking saving is a property of the search region, not of tracking.** A
  bounding-box window saved nothing when the screen filled the frame; a boundary annulus fixed
  it. Measured 1.93× decoder throughput at identical decode quality.
- **F20: the code-rate ladder is free to evaluate, and impossible to act on.** `IntraFec`'s
  codeword count depends on frame capacity alone, so per-codeword damage is invariant in `nsym`
  and one histogram scores every rung counterfactually. But `nsym` fixes the fountain symbol
  size, which the manifest fixes for the session — so **rate adaptation is a session-start
  decision, not a control loop** (OQ-037 proposes the fix). Do not plan a mid-session profile
  loop until that is resolved.
- **F23: erasures are not what the code spends — the budget `2·errors + erasures` is.** An
  erasure-only estimator predicted a 100% frame success rate at `nsym = 16` on a channel where
  the decoder failed 11 of 117 frames, and cost 24% of the goodput. Errors are recoverable after
  the fact as `corrected − erasures`. Also read this next to F19: *pixel noise* does not produce
  marginal M0 decisions, but *crosstalk* does.
- **F22: a hysteresis margin is meaningless without its ladder.** A 10% promote margin over
  rungs 3.35% apart does not slow adaptation, it stops it permanently and silently. Validate the
  pair, never either alone.

## Reference hardware

**Google Pixel 8** and **Samsung Galaxy S26 Ultra** (`docs/planning/DEVICE-MATRIX.md`).

- Cell pitch must be an **integer number of panel pixels**, and each charter grid is integer on
  exactly one of the two devices: 120×200 suits the Pixel 8 (9×12 px), 144×240 suits the S26
  Ultra (10×13 px). The grid is therefore device-dependent and negotiated in the frame header.
- **RISK-025**: the S26 Ultra has a hardware Privacy Display (Black Matrix). It must be off, and
  press reporting says viewing angles are reduced even when disabled. At close range this can
  produce a *transmitter-side* radial luminance falloff that mimics vignetting.
- Samsung may restrict ≥60 fps capture for third-party apps (OQ-035, unverified). If true, the
  Pixel 8 is the only viable high-frame-rate receiver and milestone 6 rests on it.

## The single most important constraint

> **Current state, 2026-10-23.** Everything below this box was written on 2026-08-04/06 and parts
> are now stale — kept, not rewritten. Since then: an Android **transmitter exists and has been
> read** (F36, `H` up to 0.378); alignment work took cell erasure from 0.76 to 0.44 on real frames
> (F45–F48); and protocol v2 (ADR-0015) made a real capture **able to complete a transfer at all**
> (F52) and the fountain layer near-optimal (F49). The constraint is now: **get one verified
> transfer off real optics**, and count `Fd` from the same bundle. Both need only a capture —
> `ffreplay` reports goodput and the display-state rates directly. No phones were attached when
> protocol v2 landed, so it has not yet met real hardware.

**`Fd` has never been measured, and no goodput figure exists on hardware.** That is now the
constraint — it is narrower than it was on 2026-08-03, when nothing had run on a phone at all.

**What HAS run on hardware** (samsung SM-S948U1, SoC SM8850, Android 16 / API 36):
- The **C02 probe** (F27). Four DEVICE-MATRIX press figures confirmed against the real panel,
  including the 144×240 charter grid at an integer 10×13 px pitch. The panel offers 120 Hz at full
  1440×3120 and has no 144 Hz mode. Camera advertises LEVEL_3, `MANUAL_SENSOR`, REALTIME
  timestamps, 60 fps CPU-readable, and two 240 fps high-speed modes (720p, 1080p only).
- The **C05 CPU capture path** (F28). **59.04 fps delivered of 60 requested** at 1920×1440,
  **0 duplicate frames in 600**, and manual exposure / ISO / AE / `EDGE_MODE` /
  `NOISE_REDUCTION_MODE` all honoured — so RISK-011 does **not** bite here, which is worth knowing
  because the register expects the opposite.
- A **real capture bundle through `ffreplay`** (F29), which correctly **refused** it: no
  transmitter, so no grid, so incomplete metadata. C17's guard firing on the first real capture is
  what building it early was for.

**Read F28 before trusting any rate you measure.** Requested 60 fps, got 32, and the tempting
conclusion — that the vendor overstated it — was wrong. Two of our own defects came first: a
missing `SENSOR_FRAME_DURATION` (with AE off, `CONTROL_AE_TARGET_FPS_RANGE` does nothing), and a
test script that reported a *stale* run. The A/B against a no-write arm is what settled it. That
would have been the third time this project measured its own bug and concluded something about the
world (F14, F15).

**Still unmeasured, and this is the gap:** `Fd` — nothing has presented a display state, so nothing
has counted one (EXP-006). The **240 fps high-speed path**, which gates milestone 6 and is where
duplication risk actually sits (OQ-002); the CPU path's clean result says nothing about it.
Rolling-shutter skew (F25). `Pc` and every optical property of the channel. The probe still
correctly returns **UNSUPPORTED**.

**Android status.** `platform/android/` + `app/` build a real APK containing exactly one
`arm64-v8a` `libfileflow.so`; `core/` cross-compiles under NDK r29 **unchanged**, which validated
ADR-0010 for the first time. Toolchain here: JDK 21 (Android Studio's JBR), SDK 33–36, NDK
29.0.14206865, Gradle wrapper 8.11.1, AGP 8.7.3.

```bash
export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
./gradlew :app:assembleDebug        # produces app/build/outputs/apk/debug/app-debug.apk
```

Implemented: **C02's decision logic** (`core/device.cpp`, a pure function, 19 desktop tests) with
a thin Kotlin marshalling layer per **ADR-0014**, and **C05's recording path**
(`platform/android/src/jni_recorder.cpp`) reusing `harness::CaptureWriter` so bundles are
byte-compatible with what `ffreplay` consumes by construction.

Also implemented and **run**: the **live `CameraCaptureSession`** (`CameraRecorder.kt`,
`CaptureActivity.kt`) with manual locking and read-back verification, and duplicate detection in
the writer. Drive it with `tools/android_capture.sh` and analyse cadence with
`tools/frame_cadence.py`; the `write` arm is the A/B that separates camera rate from writer I/O.

**What is NOT implemented:** ~~any **transmitter** (C03/C04 on Android), which is why `Fd` is
unmeasured and why nothing has been decoded from real optics.~~ *(Stale: the transmitter landed
before F36 and now runs through `TransmitSession`.)* Nothing upgrades `Evidence` to
`kVerified` yet either, so the probe's tier stays `kUnsupported` — correct, not a bug.

**A recording-throughput ceiling to plan around (F28):** app-private storage sustains ~50 MB/s, so
720p60 records with ~5% loss, 1080p60 needs 124 MB/s and will not, and the 240 fps arm cannot be
recorded frame-for-frame by this route at all. This bounds the **harness**, not the link — the live
receiver decodes rather than writes. Lossy compression stays excluded (C17).

**Beware F25's lesson before trusting any platform claim in `docs/research/`:** the first one to
meet a compiler was wrong. `SENSOR_ROLLING_SHUTTER_SKEW` is a `CaptureResult` key, not a
`CameraCharacteristics` key, so C02 could never have read it.

## Suggested next work, in value order

**Revised 2026-10-23.** The list further down is the August list, kept for its reasoning.

1. **One verified transfer on real optics — needs the rig, nothing else.** Install the APK on
   both phones, press Transmit on one and **Receive** on the other: Receive now decodes live
   (C13b, F54) and finishes by itself with "verified" and a saved file, or tells you what is
   wrong — point at the screen, hold steadier, or screen found but unreadable. For an analysable
   dataset, use the scripted recording path and `ffreplay`, which prints verified goodput or
   names the layer that stopped it. Nothing in C13b has run on a phone yet: watch the first live
   run with the scripted path alongside, per the verify-on-the-user's-entry-point rule (F43). If the payload FEC is the wall (0.44 erasure ≫ the ~12% nsym = 32 absorbs),
   try `--fec-nsym 64` on the transmitter — the receiver needs no change, it reads the rate from
   the header.
2. **EXP-006 from the same bundles.** `ffreplay` prints submission rate, distinct-state rate and
   states never read. Repeat at divisor 4 (30 states/s on the 120 Hz panel) so the 60 fps camera is
   ≥ 2× the state rate and a missing state means a display drop.
3. **OQ-038 — read `LENS_DISTORTION` in C02** and see whether either phone reports it. Corner
   refinement (F51) absorbs mild radial distortion; beyond |k1| ≈ 0.02 only undistortion works.
4. **OQ-037** — narrowed by ADR-0015 but still the gate for mid-session rate change.
5. **M1 / M2** — unchanged from below.

The August list:

1. ~~**Adaptive link controller (C14).**~~ **DONE 2026-08-03** — `core/src/link.cpp`,
   `ChannelEstimator` + `LinkController`, EXP-023, findings F20–F23. It picks the brute-force
   optimum exactly on every simulated channel that completes. **It cannot actuate**, and that is
   the finding, not a gap in the implementation: read F20 before touching adaptation again.
2. **OQ-037 — decouple the fountain symbol size from frame capacity.** This is now the gating
   item for all of adaptation, and it also fixes F4's ~27% block-padding waste. A fixed small
   symbol size carrying an integer number of whole symbols per frame. Touches `transfer.cpp`,
   the frame header and `ffsim`; it is a protocol change, so it needs an ADR and a version bump.
   **Highest value of the remaining simulator work**, because two separate findings point at it.
3. **M1 differential modulation.** ADR-0008 is the project's lowest-confidence decision and
   EXP-010 runs entirely in the simulator — it can be validated or killed cheaply.
4. **M2 four-level luminance.** F19 predicts this is where soft information starts to matter,
   and F23 sharpens the prediction: crosstalk already produces real symbol errors on the
   cell-sample path, and M2's levels sit at one third the spacing.
5. ~~**Android shell**~~ **DONE, and run on hardware 2026-08-04** — F27, F28, F29. See the
   constraint section above for exactly what was measured and what was not.
6. **Android transmitter (C03/C04) — the current work.** Present optical frames via
   `AChoreographer_postVsyncCallback` at native resolution with no scaling, and verify presentation
   rather than assuming it. This unblocks **EXP-006 (`Fd`)**, the model's highest-leverage
   unmeasured input, and it is what makes a real decode possible at all.

> **Note on process, worth reading before planning (F30).** Phase 1's exit criteria are **not
> met** — EXP-010, EXP-011 and EXP-012 have not run, and C18's deterministic test vectors do not
> exist — yet Phase 2 hardware work is underway. That was a deliberate reordering once hardware
> became available, not an oversight, but it means **the simulator conclusions Phase 2 is meant to
> calibrate against are themselves incomplete**. EXP-010 in particular can kill or confirm ADR-0008
> cheaply, entirely in simulation.

## What to tell the user

Report honestly, including negative results and retractions. If you find that something
previously recorded is wrong, correct it *and* say so plainly — that has happened twice already
(F14 retracted, and a "tracking is more reliable" conclusion in F15 that turned out to be a bug
artifact). Distinguish measured results from model outputs every time.

38 open questions are tracked in `docs/experiments/OPEN-QUESTIONS.md`. Add to it rather than
resolving uncertainty by assumption.
