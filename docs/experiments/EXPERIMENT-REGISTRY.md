# Experiment registry

> **Status:** Draft — **EXP-023 run** (2026-08-03); **EXP-001 has its first two hardware
> points** (2026-08-06) after a simulator dry run established it *cannot* be answered in
> simulation (F16); EXP-007 partially run. All others unrun.
> **Owner:** Research / engineering
> **Last reviewed:** 2026-08-06
> **Related:** OPEN-QUESTIONS.md, FIRST-THREE-EXPERIMENTS.md, BENCHMARK-METHODOLOGY.md

Every major uncertainty is an experiment. Entries are created **before** the run, with the
hypothesis and success threshold fixed in advance — a threshold chosen after seeing results
is not a threshold.

Raw results: `data/experiments/<EXP-ID>/raw/` — **append-only, never deleted**
(CONTRIBUTING). Processed results and conclusions alongside.

Confidence: `High` | `Medium` | `Low` | `—` (not yet run).

---

## EXP-001 — Maximum resolvable binary grid

**Question.** At what cell pitch does binary luminance stop decoding reliably, per device
pair, distance and angle?
**Hypothesis.** A usable limit exists between 96×160 and 144×240 at 30 cm; goodput rises
with density then **falls sharply** past a threshold (the "density cliff").
**Independent variables.** Grid size (swept finely, beyond the three candidates); distance
(20/30/50/80 cm); angle (0/15/30/45°).
**Controlled.** Device pair, brightness (max), ambient (fixed), exposure/ISO/focus locked,
rigid mount, modulation M0, single frame layout.
**Hardware.** Reference device pairs. **Software.** Commit recorded per run.
**Procedure.** Display static optical frames of known content at each grid size; capture
≥100 frames per configuration; measure symbol error rate and decodable-frame fraction.
**Input data.** Deterministic pseudorandom cell patterns from a fixed seed.
**Metrics.** Symbol error rate, frame decode rate, estimated goodput.
**Success threshold.** Identify, per device pair and distance, the grid maximising
estimated goodput, and locate the cliff within one sweep step.
**Raw.** `data/experiments/EXP-001/raw/` · **Processed.** `data/experiments/EXP-001/`

> **Simulator dry run completed 2026-08-02 — and it establishes that this experiment CANNOT be
> answered in simulation** (findings F14 retracted, F16). `tools/grid_sweep.sh` decodes cleanly
> from 108×180 down to 192×320 at 5.00 px/cell, goodput rising monotonically 120 → 240 KB/s,
> with **no cliff anywhere**. That null result is an artifact of the renderer, which models
> neither display subpixel structure, nor sensor MTF, nor moiré, nor diffraction — the very
> mechanisms that create the cliff ChromaCode measured on real hardware.
>
> **EXP-001 therefore requires real captures**, and this is now the strongest argument for
> prioritising the capture harness. The sweep tooling is nonetheless reusable: point it at
> replayed real frames and the same table drops out.
>
> An earlier claim of a ~6 px/cell detection floor (F14) has been **retracted** — it was an
> artifact of the F15 bug, and the corrected boundary turned out to be the `min_area_fraction`
> config threshold rather than optics. Do not plan grid geometry around it.
> **FIRST HARDWARE OBSERVATION 2026-08-06 — the cliff exists, and it is between 5.93 and 7.30
> px/cell on this pair.** Two captures, Galaxy S26 Ultra → Pixel 8, grid 120×260, M0, exposure
> 15 ms. Raw: `data/experiments/EXP-001/raw/below-cliff-5.93pxcell-20260806.txt`.
>
> | px/cell | local separation | headers decoded |
> |--------:|-----------------:|----------------:|
> | 7.30    | 142.0            | **14**          |
> | 5.93    | 4.5              | 0               |
>
> **The diagnostic detail is that GLOBAL separation was essentially unchanged** (~211 vs ~225)
> across both captures. Only the **local** separation, measured against the pilot lattice,
> collapsed. A whole-frame brightness metric says these two captures are equally good; the link
> works in one and not at all in the other. Any future exposure or brightness metric that is
> computed globally will be blind to exactly this failure.
>
> This is the mechanism F16 predicted the simulator could never produce — `tools/grid_sweep.sh`
> swept to 5.00 px/cell with goodput rising monotonically and no cliff anywhere. It took real
> optics to find, which retires the open question of whether F16's null result was a modelling
> artifact: it was.
>
> **This is two points, not a sweep.** It brackets the cliff; it does not locate it, and the
> success threshold above asks for it to be located within one sweep step. Distance and angle are
> unswept, and n = 1 per point. Treat 7.5 px/cell as a working floor with one observation behind
> it, not as a characterised limit.

**Conclusion.** Bracketed, not located: the cliff lies between 5.93 and 7.30 px/cell for this
device pair at this distance. · **Confidence.** Low — two captures, one pair, one distance, no
repeats. The *existence* of a sharp transition is High; its position is not.
· **Follow-up.** A real sweep over distance to locate it; feeds EXP-013 and the grid decision
(OQ-003).
**Note.** ChromaCode's measured non-monotonic cell-size response `[LIT]` is the reason this
sweeps finely rather than testing three points.

---

## EXP-002 — Dynamic QR baseline

**Question.** What verified payload goodput does a well-tuned dynamic QR stream achieve on
our reference hardware?
**Hypothesis.** Order of 1–10 KB/s; FileFlow should exceed it by a large multiple.
**Independent variables.** QR version, error-correction level, frames per second, distance.
**Controlled.** Same hardware, lighting, mounting and file as FileFlow benchmarks.
**Procedure.** Implement or integrate a dynamic QR stream; tune it honestly (best version,
ECC level and rate we can achieve); measure end-to-end verified goodput on the standard
test files.
**Metrics.** Verified payload goodput; failure rate.
**Success threshold.** A defensible baseline number we would be comfortable publishing as
the thing we beat.
**Conclusion.** — · **Confidence.** —
**Why this matters.** G4 and ADR-0006 cannot be claimed without it. **We must not cite
someone else's QR number.** Tuning this honestly — rather than building a weak strawman —
is a matter of research integrity.

---

## EXP-003 — Binary brightness separation

**Question.** What luminance separation between dark and bright cells is achievable at the
receiver, across conditions?
**Hypothesis.** Separation is sufficient at ≤50 cm indoors but degrades sharply with
ambient light and angle.
**Independent variables.** Screen brightness, ambient illuminance, distance, angle, panel
type. **Controlled.** Grid, exposure, ISO, focus.
**Metrics.** Mean separation in sensor counts; per-region σ; effective SNR.
**Success threshold.** Characterise the envelope where SNR supports M0, and identify where
it would support M2's four levels.
**Conclusion.** — · **Confidence.** — · **Follow-up.** Gates M2 feasibility.

---

## EXP-004 — Optimal exposure

**Question.** What exposure time maximises goodput?
**Hypothesis.** Shorter exposure reduces temporal mixing and motion blur but costs SNR;
an interior optimum exists well below the frame period.
**Independent variables.** `SENSOR_EXPOSURE_TIME` swept. **Controlled.** ISO, focus, grid.
**Metrics.** Symbol error rate, clean-frame ratio `Pc`, estimated goodput.
**Success threshold.** An exposure recommendation per device and lighting condition.
**Conclusion.** — · **Confidence.** —

## EXP-005 — Optimal ISO

**Question.** What sensitivity best trades noise against the short exposure EXP-004 wants?
**Hypothesis.** Lowest ISO meeting the exposure target is best; noise dominates above it.
**Metrics.** Symbol error rate, per-region σ, goodput. **Conclusion.** — · **Confidence.** —

## EXP-006 — Display state rate verification (30 vs 60 vs 120)

**Question.** Are requested display states actually presented, and at what rate?
**Hypothesis.** 30 and 60 are reliable on reference devices; 120 is not reliably
distinct on all of them.
**Independent variables.** Requested rate; rendering API (GLES vs Vulkan).
**Procedure.** Render a known counting pattern; capture with Perfetto traces **and** with a
high-speed camera or a second device; compare intended versus presented sequences.
**Metrics.** Presented-state rate, missed/duplicated frame rate, jitter.
**Success threshold.** A verified `Fd` per device, with a measured discrepancy rate.
**Conclusion.** — · **Confidence.** —
**Why it matters.** `Fd` appears linearly in the goodput model and every scenario assumes
it. It is currently unvalidated. Also decides whether Vulkan is needed (ADR-0004).
**Tooling ready 2026-10-23.** No high-speed camera or Perfetto run is needed for the first answer:
every frame header carries the transmitter's sequence number, and `ffreplay` now reports, from
any capture bundle, the **submission rate** (sequence span / time), the **distinct-state rate**
read by the receiver (a lower bound on `Fd`), repeat captures, and states never read — and says
whether the camera was fast enough (≥ 2× the state rate) for a missing state to mean the display
dropped it. Procedure: transmit at divisor 1, 2 and 4 on the S26 Ultra's 120 Hz panel, capture at
60 fps on the Pixel 8, replay each bundle.

## EXP-007 — Camera frame-rate and access-path characterisation

**Question.** What is the maximum frame rate with *accessible, distinct* frames on each
path, at a resolution sufficient for our grids?
**Hypothesis.** CPU `YUV_420_888` reaches 60 fps at adequate resolution; the ≥120 fps
high-speed session works only via `SurfaceTexture` `[FACT]` and may not deliver genuinely
distinct frames.
**Independent variables.** Session type, output surface type, resolution, requested FPS.
**Procedure.** For each reference device: enumerate high-speed sizes and ranges; attempt
CPU and GPU paths at each rate; verify frame *distinctness* by displaying a known counting
pattern; measure drop rate and GPU readback latency; test whether any lossless recording
path exists for the capture harness.
**Metrics.** Sustained fps, drop rate, distinct-frame fraction, readback latency,
resolution available.
**Success threshold.** A definitive per-device answer on which capture path to use, and
whether milestone 6 is testable at all on the available hardware.

> **PARTIAL RESULT 2026-08-04 — the ENUMERATION half only** (finding F27). Raw:
> `data/experiments/EXP-007/raw/probe-SM-S948U1-20260804.txt`. Device: samsung SM-S948U1
> (SoC SM8850, Android 16 / API 36).
>
> Advertised capability set: **LEVEL_3**, `MANUAL_SENSOR`, **REALTIME** timestamps, fastest
> CPU-readable `YUV_420_888` **60 fps**, and **two** constrained high-speed modes, both
> **240 fps** (1280×720 and 1920×1080). `SENSOR_ROLLING_SHUTTER_SKEW` is absent from
> `CameraCharacteristics`, independently confirming F25.
>
> **The verification half has NOT run, and it is the half that matters.** No frame has been
> captured, so distinct-frame fraction, drop rate, readback latency and manual-control obedience
> are all still unknown. Enumeration cannot answer them: a high-speed session that returns
> *duplicated* frames advertises identically to one that does not.
>
> Two conclusions already follow from enumeration alone:
> 1. **The CPU path caps `Fd` at 60** on this device, so milestone 6 is unreachable on it
>    regardless of the display — ADR-0005's dual-path split is vindicated.
> 2. **The ≥120 fps path caps capture at 1080p**, which caps resolvable grid density to roughly
>    5 px/cell at the 144×240 charter grid. On the high-rate path the receiver, not the panel,
>    bounds density (F27).

> **VERIFICATION HALF, CPU PATH ONLY — 2026-08-04** (findings F28, F29). Raw:
> `data/experiments/EXP-007/raw/capture-cpu-path-SM-S948U1-20260804.md`. Reproduce with
> `tools/android_capture.sh` and `tools/frame_cadence.py`.
>
> - **The CPU path delivers 59.04 of a requested 60 fps at 1920×1440**, measured with disk writes
>   disabled. OQ-001 is answered for this device: 60 fps CPU-readable `YUV_420_888` is real, not
>   just advertised.
> - **Frames are genuinely distinct: 0 duplicates in 600 written frames.** A duplicate is a frame
>   byte-identical to its predecessor, which sensor noise makes a decisive test. This does **not**
>   cover the 240 fps high-speed path, where the duplication risk actually sits.
> - **Manual control is honoured.** Exposure exact, ISO quantised 400→398, AE/EDGE/NOISE_REDUCTION
>   all reported OFF as requested. RISK-011 does not bite on this device for these controls —
>   which is worth recording precisely because the risk register expects the opposite.
> - **The recorder, not the camera, was the limit**: 32 fps with writes on versus 59 with them
>   off, at 2.76 MB/frame. The sensor's modal interval is 16.66 ms (60.02 fps) with 17 single-frame
>   drops. See F28; it bounds the *harness*, not the link.
>
> **Still outstanding, and it is the part that gates milestone 6:** the 240 fps constrained
> high-speed arm, which needs a `SurfaceTexture` path because such a session rejects `ImageReader`
> `[FACT]`. Nothing here tests it.

**Conclusion.** CPU path verified (60 fps, distinct frames, manual control honoured); high-speed
path untested. · **Confidence.** High for the CPU path on this device, from a single run per arm;
**none** for the ≥120 fps path.
**Follow-up.** Build the `SurfaceTexture` high-speed arm and re-run the distinctness test at
240 fps. Separately, a capture run supplies the rolling-shutter skew the probe structurally cannot
(F25), and that has not been collected yet.
**Why it matters.** Determines a large implementation fork (ADR-0005) and whether
milestone 6 has any evidence behind it. **Should run early.**

## EXP-008 — Duplicate and mixed-frame classification

**Question.** How accurately can we classify camera frames as clean, mixed or duplicate,
and locate the transition band?
**Hypothesis.** >95% accuracy with timing tracks and a phase indicator, when
`SENSOR_INFO_TIMESTAMP_SOURCE = REALTIME`; degraded when `UNKNOWN`.
**Procedure.** Simulator first (exact ground truth), then device with known patterns.
**Metrics.** Classification accuracy per class; band localisation error in rows.
**Success threshold.** >95% simulated, >90% on device.
**Conclusion.** — · **Confidence.** — · **Follow-up.** Gates M4 (Phase 7).

## EXP-009 — Clean-frame ratio `Pc` measurement

**Question.** What fraction of display states are actually captured cleanly and decoded, in
realistic conditions?
**Hypothesis.** 0.55–0.80 as assumed in the performance model.
**Metrics.** `Pc` per condition, with distribution not just mean.
**Success threshold.** A measured `Pc` replacing the model's guess.
**Conclusion.** — · **Confidence.** —
**Why it matters.** `Pc` is the model's weakest input *and* among its highest-leverage
variables. If real `Pc` is ~0.35, milestone 4 is unreachable even with M2.

## EXP-010 — Differential versus absolute decoding

**Question.** Do any of the four M1 differential schemes beat well-calibrated absolute
M0 decoding, on **end-to-end goodput**?
**Hypothesis.** M1c (cell versus local pilot) wins; **M1b (complementary frames) loses
because it halves `Fd`**; M1a/M1d suffer error propagation under frame loss.
**Independent variables.** Scheme (M1a–d vs calibrated M0); static-distortion severity;
frame-loss rate. **Controlled.** Grid, code rate, channel model.
**Procedure.** Simulator sweep first (cheap, exact ground truth), then device confirmation
of the surviving schemes.
**Metrics.** **End-to-end goodput** (primary); symbol error rate (diagnostic).
**Success threshold.** A clear ranking; an explicit decision to keep or drop each scheme.
**Conclusion.** — · **Confidence.** —
**Gate.** If no temporal scheme beats calibrated M0, **ADR-0008 is superseded** and effort
moves to M2. This is a genuine possible outcome.

## EXP-011 — Intra-frame FEC comparison

**Question.** Which code family and rate maximises end-to-end goodput within our CPU budget?
**Hypothesis.** Soft-input LDPC beats hard-decision RS on goodput; RS/BCH is right for the
header.
**Independent variables.** Code family (LDPC, RS, BCH, polar, non-binary), code rate,
LLR quantisation (4/6/8-bit).
**Controlled.** Error patterns from the simulator: independent, clustered, full erasure.
**Metrics.** Post-decode frame error rate, ARM64 decode time per frame, peak memory,
miscorrection rate, **resulting end-to-end goodput**.
**Success threshold.** A selection justified on goodput, with decode time fitting the
budget under thermal throttle.
**Conclusion.** — · **Confidence.** — · **Runs in.** Simulator only — no device needed.

## EXP-012 — Fountain overhead measurement

**Question.** What reception overhead do candidate fountain schemes actually incur at our
block sizes and loss rates?
**Hypothesis.** RaptorQ-class ≈2%; LT-class materially worse but acceptable.
**Independent variables.** Scheme, block size `k`, loss rate, loss burstiness.
**Metrics.** Overhead fraction, decode time, peak memory, decode failure rate.
**Success threshold.** A measured `Rfountain` per scheme, and a decision on whether the
LT/online fallback costs enough goodput to justify pursuing RaptorQ licensing.
**Conclusion.** **Run 2026-10-23 (F49), decided by ADR-0015.** The hypothesis was wrong about LT in
the configuration we had: systematic prefix + robust-soliton LT measured 0.20–0.33 overhead at
5–30% i.i.d. loss, k = 64, and Gaussian elimination on the same rows recovered almost none of it.
A systematic dense GF(2) code measured **0.022–0.025** (k = 64) and **0.006** (k = 256) across the
same range — RaptorQ-class, without RaptorQ. Licensing is not worth pursuing at these block sizes.
**Not yet measured:** bursty loss, and decode time on ARM64. Regression guard:
`Fountain.OverheadUnderLossIsNearOptimal`.
· **Confidence.** High for the code property (it does not depend on the channel model).
· **Runs in.** Simulator only.
**Note.** Legal review (RT-07) runs in parallel and is a separate gate — no longer a blocking one.

## EXP-013 — Frame layout and four-level luminance evaluation

**Question.** Which of the three candidate frame layouts, at which grid and parameters,
maximises goodput — and does four-level luminance pay?
**Hypothesis.** Candidate B (distributed pilot lattice) wins at 60 states/s; Candidate C
(tiles) wins once M4 is in play. M2 beats M0 on goodput despite lower `Rfec`.
**Independent variables.** Layout (A/B/C), grid size, pilot pitch, tile size, guard width,
interior sampling margin, header cell size and replication; modulation (M0 vs M2).
**Procedure.** Automated simulator sweep to narrow the space, then on-device evaluation of
survivors. Report the **full response surface**, not just the winner.
**Metrics.** End-to-end goodput, `H`, `Pc`, symbol error rate.
**Success threshold.** A layout and grid selection with a *broad* optimum — a sharp peak is
rejected in favour of a robust plateau even at slightly lower peak goodput.
**Conclusion.** — · **Confidence.** —

## EXP-014 — Candidate four-colour constellations

**Question.** Does colour modulation beat four-level luminance at the cell size each
actually requires?
**Hypothesis.** **M3 loses to M2**, because `YUV_420_888` chroma is 2×2 subsampled `[FACT]`
so colour cells must be larger, cancelling the 2-bits-per-cell benefit M3 shares with M2.
**Independent variables.** Constellation (several candidates, **not** assuming R/G/B/W);
cell size per mode; device pair.
**Metrics.** Post-decorrelation separation, symbol error rate, **goodput at each mode's
required cell size**.
**Success threshold.** A clear answer on whether M3 is worth implementing at all.
**Conclusion.** — · **Confidence.** —

## EXP-015 — Subpixel sampling accuracy

**Question.** Which sampling method and interior margin minimises effective crosstalk?
**Independent variables.** Method (nearest / bilinear / interior-weighted / matched filter);
interior margin fraction. **Metrics.** Symbol error rate, effective crosstalk coefficient.
**Success threshold.** A recommended method and margin. **Conclusion.** — · **Confidence.** —

## EXP-016 — CPU versus GPU rectification and sampling

**Question.** Which path is faster, and does GPU readback serialise the pipeline?
**Hypothesis.** GPU wins at large grids; readback latency is the deciding factor.
**Metrics.** Sampling time per frame, end-to-end latency, CPU and GPU utilisation, power.
**Also required.** CPU/GPU output equivalence within tolerance — a mandatory CI guard.
**Conclusion.** — · **Confidence.** —

## EXP-017 — Tracking versus per-frame detection

**Question.** Is persistent tracking cheaper *and* at least as accurate as re-detecting
every frame?
**Hypothesis.** Yes on both counts — this is a load-bearing assumption of ADR-0006.
**Independent variables.** Method; motion condition (rigid / light handheld / heavy handheld).
**Metrics.** Time per frame, geometric error versus ground truth, lock-loss rate,
reacquisition time.
**Success threshold.** Tracking is faster and no less accurate under rigid and light
handheld conditions. **Conclusion.** — · **Confidence.** —
**Note.** If tracking loses, a core premise of the architecture weakens and ADR-0006 needs
revisiting.

## EXP-018 — Distance and viewing angle envelope
**Question.** Over what distance and angle range does the link work, per profile?
**Independent variables.** Distance (15–100 cm), angle (0–60°).
**Metrics.** Goodput, symbol error rate, lock rate. **Conclusion.** — · **Confidence.** —

## EXP-019 — Ambient lighting and screen brightness
**Question.** How do indoor lighting conditions and screen brightness affect goodput?
**Independent variables.** Ambient (dark / office / bright / direct sun); screen brightness.
**Metrics.** Goodput, separation, saturation/glare incidence. **Conclusion.** — · **Confidence.** —

## EXP-020 — Thermal throttling under sustained transfer
**Question.** How does sustained transfer affect goodput over time?
**Hypothesis.** Goodput degrades measurably within minutes at maximum brightness and
full-rate capture and decode.
**Procedure.** Large-file transfers (100 MB) with thermal state, clock and goodput logged
over time. **Metrics.** Goodput versus elapsed time; thermal status; frame drop rate.
**Success threshold.** A characterised degradation curve and a thermal policy.
**Conclusion.** — · **Confidence.** — · **Related.** RISK-010

## EXP-021 — OLED versus LCD transmitter
**Question.** Does panel technology materially affect achievable goodput?
**Hypothesis.** OLED subpixel layout and response differ enough to change the optimal grid
and possibly the colour constellation.
**Metrics.** Symbol error rate, moiré incidence, achievable grid, goodput.
**Conclusion.** — · **Confidence.** — · **Note.** Requires both panel types in the
reference set.

## EXP-022 — SIMD cell sampling optimisation
**Question.** What NEON sampling throughput is achievable, and does it meet budget?
**Metrics.** Cells/second, cycles/cell, cache behaviour. **Conclusion.** — · **Confidence.** —

## EXP-023 — Adaptive intra-frame code rate selection

> **Registered 2026-08-03, before the run** (CONTRIBUTING). The hypotheses and thresholds
> below were fixed in advance and are not to be edited after seeing results — only the
> **Conclusion** line is written afterwards.

**Question.** Two parts, and the second is the decision-relevant one.
**(a)** Can a receiver-side controller (C14) select the intra-frame `nsym` that maximises
end-to-end goodput, using *only* telemetry the live receiver already produces?
**(b)** What does a **wrong** `nsym` guess cost in goodput? That number sets the value of
one-way adaptation (OQ-013), and therefore whether reverse optical control (ADP-04) belongs
in Phase 9 or much earlier.

**Hypotheses, fixed in advance.**
- **H1 (structural).** The per-codeword erasure-load distribution is invariant in `nsym`,
  because `IntraFec` derives its codeword count from frame capacity alone
  (`codewords = frame_capacity / codeword_bytes`) and `nsym` shrinks only the message. If so,
  **one frame's erasure pattern predicts the decode outcome at every rung of the ladder at
  zero extra cost** — the receiver can evaluate the whole ladder counterfactually.
- **H2 (accuracy).** The controller's selected `nsym` lands within **one ladder rung** of the
  brute-force goodput optimum on at least 3 of the 4 test channels.
- **H3 (stability).** Under a channel whose optimum straddles a decision boundary, hysteresis
  holds profile changes to **≤ 1 per 100 frames**. The component registry names oscillation
  as a C14 failure mode explicitly, so this is a threshold and not a nicety.
- **H4 (cost of guessing).** A fixed `nsym` chosen for the wrong channel costs **more than
  30%** of the goodput available at that channel's optimum. F18's sweep suggests ~2.7× at the
  extreme; H4 asserts the penalty is large enough to matter across realistic channels, not
  only adversarial ones.

**Independent variables.** `nsym` ∈ {8, 16, 24, 32, 40, 48, 64} (the ladder); channel
severity (four channels, clean to severe); seed.
**Controlled.** Grid 120×200, layout Candidate B, M0, payload size, block structure, and
`Fd` = 60 states/s assumed throughout.
**Procedure.** For each channel, run the full ladder to obtain **brute-force ground truth**
for the goodput-optimal `nsym`. Independently, run once at a fixed reference `nsym` and ask
the controller — which sees only receiver telemetry — to name the optimum. Compare.
**Metrics.** Verified payload goodput per rung (primary); controller's selected rung;
rung-distance from the optimum; predicted vs measured frame-success rate per rung; profile
changes per 100 frames.
**Success threshold.** H2 and H3 both hold, **and** the controller's predicted frame-success
curve tracks the measured one closely enough that its *ranking* of rungs is right even where
its absolute predictions are not. A controller that picks correctly for the wrong reason has
not passed.
**Raw.** `data/experiments/EXP-023/raw/` · **Processed.** `data/experiments/EXP-023/`
**Runs in.** Simulator only — no device needed. ⚠ Every number is therefore `[HYP]`
(RISK-024): the channel model is uncalibrated, so this measures whether the *decision rule*
is sound, **not** which `nsym` real hardware will want.
**Raw.** `data/experiments/EXP-023/raw/adapt_sweep_seed5_v2.txt` · **Script.**
`tools/adapt_sweep.sh` · **Processed.** `data/experiments/EXP-023/RESULTS.md`

> **RUN 2026-08-03.** Findings F20–F23. Verdict per hypothesis:
>
> - **H1 (structural invariance) — CONFIRMED**, and proven exactly rather than statistically:
>   `LinkCounterfactual.OneRungPredictsTheWholeLadder` requires the predicted frame-success rate
>   to match brute force through the real codec to 1e-12 at every rung. Confirmed end to end too
>   — the worst erasure load is identical at all seven rungs on every channel.
> - **H2 (within one rung on ≥3 of 4 channels) — PASSED, and the threshold was too weak.**
>   The controller hit the optimum **exactly** (rung distance 0) on all three channels that
>   complete a verified transfer. The fourth completes at no rung at all, so it has no optimum to
>   be scored against. But H2 as registered would also have passed the **wrong** answer the first
>   implementation gave: `nsym = 16` is one rung from the optimum and costs 24% of the goodput
>   (F23). Distance in rungs is not a proxy for cost. Left as registered — a threshold is not
>   edited after seeing results — and noted as a lesson in choosing them.
> - **H3 (≤1 profile change per 100 frames) — CONFIRMED** by
>   `HoldsStillOnAChannelStraddlingADecisionBoundary`, which additionally requires a margin-free
>   controller to flap on the same channel, so the result cannot be an artifact of an easy
>   scenario.
> - **H4 (a wrong guess costs >30%) — CONFIRMED, and larger than expected**: 50.0%, 66.8% and
>   93.2% on the clean, mild and F18-impaired channels.
>
> **The finding that matters most was not a hypothesis.** The recommendation cannot be actuated:
> `nsym` fixes the fountain symbol size, which the manifest fixes for the session, so code-rate
> selection is a session-START decision and C14's specified mid-session control loop is not
> implementable against this protocol (F20). OQ-037 proposes the fix.

**Conclusion.** The decision rule is sound and cheap — one histogram, no new per-frame
measurement, exact ladder prediction, optimum found on 3/3 scoreable channels. The *delivery
path* does not exist. · **Confidence.** Medium for the rule (exact against the real codec, but
every channel is simulated and uncalibrated — RISK-024); High for the structural blocker, which
is a property of the code and the manifest rather than of the channel.
**Follow-up.** OQ-037 (decouple symbol size from frame capacity); re-measure the cost-of-guessing
spread on real captures before using it to argue ADP-04's phase.
**Related.** F18 (the `nsym` sweep that motivated it), F20–F23, OQ-013, OQ-036, OQ-037,
ADP-01, ADP-02, C14.

---

## Scheduling notes

**Runnable with no device, as soon as the simulator exists (Phase 1):**
EXP-010, EXP-011, EXP-012, EXP-008 (simulated portion), EXP-013 (sweep portion), EXP-015.

These should be started immediately once Phase 1 lands — they are cheap, parallel, have
exact ground truth, and several of them can overturn architectural decisions (notably
EXP-010 versus ADR-0008).

**Gating experiments — run early, they determine large forks:**
EXP-007 (capture path), EXP-006 (`Fd` validity), EXP-001 (grid).

**Cannot start until a working link exists:** EXP-009, EXP-018, EXP-019, EXP-020.
