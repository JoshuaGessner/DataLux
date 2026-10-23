# Open-question registry

> **Status:** Draft
> **Owner:** Project lead
> **Last reviewed:** 2026-10-23
> **Related:** EXPERIMENT-REGISTRY.md, RISK-REGISTER.md, PHASE1-FINDINGS.md

Every unresolved question that could change a design decision. Questions are closed only
by evidence, and the closing evidence is recorded.

Severity: **Blocking** (a decision cannot be made without it) · **High** (would change the
plan) · **Medium** (would change a component) · **Low** (would refine a parameter).

---

## Platform

| ID | Question | Severity | Resolved by | Status |
|---|---|---|---|---|
| **OQ-001** | On reference devices, what is the maximum frame rate at which CPU-accessible `YUV_420_888` is delivered without drops, at a resolution sufficient for our densest grid? **ANSWERED for the S26 Ultra 2026-08-04 (F28):** **59.04 fps delivered of 60 requested at 1920×1440**, with a metronomic 16.66 ms modal interval and **0 duplicates in 600 written frames**. The caveat is ours, not the device's — writing 2.76 MB/frame saturates storage and costs 46% of frames, so "without drops" depends on the recorder, not the camera. Still open for the **Pixel 8**, which has not been measured | **Blocking** | EXP-007 | **Answered (S26 Ultra)** |
| **OQ-002** | Does the GPU-texture path in a high-speed session deliver genuinely *distinct* frames at 120 fps, or does the vendor pipeline duplicate/interpolate them? | **Blocking** (milestone 6) | EXP-007 | Open |
| **OQ-004** | Which presentation-verification mechanism is reliable per-frame at 60 and 120 Hz? (`ASurfaceTransaction` completion fences vs FrameTimeline vs Perfetto-only) | High | EXP-006 | Open |
| **OQ-005** | Does `setFrameRate` reliably hold a 120 Hz mode for a multi-minute transfer, or does thermal/power management drop it? | High | EXP-006, EXP-020 | Open |
| **OQ-015** | How much active capability *verification* is worth doing at startup versus at session negotiation? | Medium | CAP-03 design | Open |
| **OQ-016** | Do vendor `EDGE_MODE` / `NOISE_REDUCTION_MODE` / `TONEMAP_MODE` settings actually take effect, or are they silently ignored? **PARTLY ANSWERED 2026-08-04 (F28):** on the S26 Ultra, `EDGE_MODE` and `NOISE_REDUCTION_MODE` both **report OFF as requested** in `CaptureResult`, as do `CONTROL_AE_MODE` and an exact `SENSOR_EXPOSURE_TIME`. But *reporting* a mode is one step short of *behaving* as it: confirming no sharpening is applied needs a known high-spatial-frequency target, which needs a transmitter. `TONEMAP_MODE` untested | High | EXP-007, then a transmitter for the behavioural half | **Narrowed** |
| **OQ-017** | Is `SENSOR_ROLLING_SHUTTER_SKEW` reported, and is the reported value accurate? | Medium | EXP-007, EXP-008 | Open |
| **OQ-018** | Does panel subpixel structure (OLED PenTile vs LCD stripe) measurably affect binary luminance separation at our cell pitches? | Medium | EXP-021 | Open |
| **OQ-023** | Is there a lossless recording path for the capture harness, or does every available recording route compress lossily? **ANSWERED 2026-08-04 (F28, F29):** **yes — raw Y-plane bundles, no codec involved** — and one was pulled from a phone and parsed by `ffreplay`. It is bounded by **write throughput, ~50 MB/s** on app-private storage: 1280×720 at 60 fps records with ~5% loss, 1920×1080 at 60 fps needs 124 MB/s and will not, and the 240 fps arm cannot be recorded frame-for-frame by this route at all (needs a RAM ring buffer, faster storage, or a sampled recording). Lossy compression stays excluded — it destroys the cell structure the recordings exist to measure | High | EXP-007 | **Answered, with a throughput bound** |

## Signal and modulation

| ID | Question | Severity | Resolved by | Status |
|---|---|---|---|---|
| **OQ-003** | Which frame layout (A/B/C) and grid size? | **Blocking** | EXP-001, EXP-013 | Open |
| **OQ-006** | Which final modulation profile set? | High | EXP-010, EXP-013, EXP-014 | Open |
| **OQ-007** | Which four-colour constellation, if M3 is pursued at all? | Medium | EXP-014 | Open |
| **OQ-011** | What is the effective number of *independent* spatial subchannels at each cell pitch? (Crosstalk means it is fewer than the cell count) | Medium | EXP-001 | Open |
| **OQ-012** | What is the distribution of spatially-clustered damage region sizes? Drives interleaver design | Medium | Phase 2 characterisation | Open |
| **OQ-019** | What is the optimal cell interior sampling margin? | Medium | EXP-015 | Open |
| **OQ-020** | Is persistent tracking actually cheaper **and** more accurate than per-frame detection under handheld motion? | High | EXP-017 | Open |
| **OQ-022** | Which marker patterns maximise detection reliability at our cell pitches under defocus? | Medium | Simulator study | Open |
| **OQ-024** | Where is the density cliff — the grid pitch past which goodput collapses? | High | EXP-001 | Open |

## Coding

| ID | Question | Severity | Resolved by | Status |
|---|---|---|---|---|
| **OQ-009** | Which intra-frame code family and rate? | High | EXP-011 | Open |
| **OQ-010** | Which fountain scheme — and is RaptorQ legally available for smartphone deployment given the Qualcomm IPR tiering? **ANSWERED 2026-10-23 (ADR-0015, F49):** a systematic random linear fountain over GF(2) with Gaussian elimination — ~1.6 symbols over k at any loss rate (0.025 overhead at k = 64), no patent question. The RaptorQ legal review (RT-07) is no longer on the critical path; it would matter only for blocks far beyond the 1024-symbol bound | Low (was High) | ~~EXP-012 + RT-07 legal review~~ F49 | **Answered** |
| **OQ-014** | What fraction of one big core can the FEC decoder use before it competes with capture and reduces `Pc`? | Medium | EXP-011, EXP-016 | Open |
| **OQ-025** | Is 8-bit LLR quantisation sufficient, or does it cost measurable coding gain? | Low | EXP-011 | Open |

## Architecture and protocol

| ID | Question | Severity | Resolved by | Status |
|---|---|---|---|---|
| **OQ-013** | **On a one-way link, how does the transmitter learn channel state at all?** Profile cycling wastes display states; reverse optical control is Phase 9. This is the least-designed part of the architecture. **Partially quantified 2026-08-03 (EXP-023, F20):** the receiver *can* now identify the goodput-optimal code rate exactly, from telemetry it already produces — so the open part is narrowed to **delivery**, not estimation. A wrong fixed guess costs **50–93%** of available goodput on simulated channels, which raises the value of ADP-04 considerably. Still open, and still the least-designed area | **High** | Design work + Phase 5 measurement; re-measure the spread on real captures | Open |
| **OQ-008** | Can we self-calibrate lens distortion from the transmitted grid, avoiding per-device calibration? | Medium | Simulator study + Phase 2 | Open |
| **OQ-026** | What is the correct behaviour when the app is backgrounded mid-transfer — pause or abort? | Low | Design decision | Open |
| **OQ-027** | Should the receiver hash incrementally during reception (early corruption detection) or only at the end? | Low | Design decision | Open |
| **OQ-036** | What thermal policy should C14 apply (ADP-05)? Higher `nsym` costs more decode work, but no ARM64 decode-cost measurement exists, so any policy today would be invented. C14 deliberately ships **without** a `ThermalState` input rather than fake one | Medium | EXP-011 (decode cost), EXP-020 (throttling curve) | Open |
| **OQ-037** | **Should the fountain symbol size be decoupled from frame capacity?** Today `nsym` → FEC message size → fountain symbol size → manifest, which is fixed for the session, so **no capacity-changing knob can move mid-transfer** (F20). A fixed small symbol size carrying an integer number of whole symbols per frame would unblock mid-session rate adaptation without touching the fountain mathematics, and would also address F4's ~27% block-padding waste. Cost: more header bookkeeping per frame, and a protocol version bump. **NARROWED 2026-10-23 (ADR-0015):** the padding waste is gone by a different route (padding is never sent; the receiver primes it), and the code rate is now carried in every header, so no receiver ever needs to be told it. What remains is the core of the question: symbol size is still the FEC message size, fixed by the manifest, so `nsym` still cannot change mid-transfer | High | Design work; becomes an ADR if taken | **Narrowed** |
| **OQ-038** | **Should the receiver undistort with the camera's own lens calibration?** F51: corner refinement absorbs radial distortion up to \|k1\| ≈ 0.02 (simulated), and beyond that no single homography fits. Camera2 publishes `LENS_DISTORTION` and `LENS_INTRINSIC_CALIBRATION` (API 28+); applying them in C08 before the homography would remove the term at its source. Unknown whether either reference device reports them, or whether the ISP has already corrected the YUV stream they describe | Medium | C02 probe read-out of the two keys, then a real capture with and without | Open |

## Measurement and process

| ID | Question | Severity | Resolved by | Status |
|---|---|---|---|---|
| **OQ-028** | How faithful must the simulator's impairment model be before its conclusions transfer to hardware? | **High** | Phase 2 calibration (SIM-03) | Open |
| **OQ-029** | What is the real `Pc` on reference hardware? The model assumes 0.55–0.80; this is a guess and it is high-leverage | **High** | EXP-009 | Open |

## Research gaps

| ID | Question | Severity | Resolved by | Status |
|---|---|---|---|---|
| **OQ-030** | What do the visible-branch systems (COBRA, RainBar, RDCode, ShiftCode, FareQR) actually report, and in which metric? | High | RT-01, RT-02 | Open |
| **OQ-031** | What does the Visual MIMO capacity analysis actually say? Our dense-grid thesis currently rests on an unread foundation | High | RT-03 | Open |
| **OQ-032** | Does ShiftCode's greyscale mode really outperform its four-colour mode? If so it is evidence against M3 | Medium | RT-01 | Open |

## Project

| ID | Question | Severity | Resolved by | Status |
|---|---|---|---|---|
| ~~OQ-021~~ | ~~Which licence for this project?~~ | — | **CLOSED 2026-08-02: Apache-2.0** (ADR-0013 D4) | **Closed** |
| ~~OQ-033~~ | ~~Which specific reference devices?~~ | — | **CLOSED 2026-08-02: Pixel 8 + Galaxy S26 Ultra.** See [DEVICE-MATRIX.md](../planning/DEVICE-MATRIX.md). Both OLED, so EXP-021 (OLED vs LCD) cannot run as written | **Closed** |
| **OQ-039** | **How much frame margin does a panel's corner radius demand, and can it be discovered rather than guessed?** A full-bleed optical frame has its corners cut away by the display glass — measured at 20–77 camera px, over ten cells, enough to put a whole corner marker outside the emitting area and break localisation (F34). The fix is a letterbox margin, but margin costs payload cells, and the required amount depends on a corner radius no API we have found exposes. Options: measure per device once and cache it; derive it from a self-calibration frame; or set a conservative constant and accept the loss | **High** | Per-device measurement, or a calibration pattern | Open |
| **OQ-038** | **Which grid should be used when the app cannot get the panel's native resolution?** Measured 2026-08-04 (F31): an app requesting 1440×3120 on the S26 Ultra receives **1080×2340**, because Samsung gates resolution behind a system setting. That makes the **144×240 charter grid fractional (7.5 × 9.75 px/cell) and therefore unusable**, while the square-cell **120×260** is integer at both resolutions. Options: select the grid at runtime from the actual surface size; require the user to raise the display setting; or **prefer square-cell grids specifically because they survive a resolution change**. Bears directly on OQ-003 | **High** | Design decision + EXP-001/EXP-013 arms at each resolution | Open |
| **OQ-034** | Does the S26 Ultra's Privacy Display / Black Matrix restrict the optical channel even when disabled, and does it cause transmitter-side radial luminance falloff at close range? | **High** | EXP-003, EXP-018 (add Privacy-on/off arms) | Open |
| **OQ-035** | Does Samsung restrict ≥60 fps capture for third-party apps on the S26 Ultra? **NARROWED 2026-08-04 (F27):** the device *advertises* two 240 fps constrained high-speed modes (720p, 1080p) plus 60 fps CPU-readable modes to our third-party app, so the pessimistic reading is contradicted and milestone 6 does not rest on the Pixel 8 alone. Still open because being offered a mode is not being given frames — a high-speed session returning DUPLICATED frames is invisible to enumeration | Medium (was High) | EXP-007, verification half | **Narrowed** |

---

## The five most consequential

1. **OQ-002 — does the high-speed path deliver *distinct* frames?** OQ-001 is now answered
   for the S26 Ultra (60 fps CPU-readable, delivered, distinct — F28), which removes half of this
   pair. What remains is the half that gates milestone 6: the 240 fps constrained high-speed
   session has not been run, and duplication there is invisible to capability enumeration. The CPU
   path's clean result says nothing about it — a different API, a different buffer pipeline.
2. **OQ-029 — the real `Pc`.** The model's weakest input and among its highest-leverage
   variables. If `Pc` is ~0.35 rather than ~0.70, every projection halves.
3. **OQ-003 / OQ-024 — grid and layout, and where the density cliff sits.** The literature
   shows a cliff exists; we do not know where ours is, and the model does not contain one.
4. **OQ-013 / OQ-037 — adaptation on a one-way link, and the protocol constraint underneath
   it.** Still genuinely under-designed. EXP-023 narrowed it usefully: estimation is solved
   (the receiver names the optimal rung exactly, for free), so what remains is delivery — and
   the first obstacle is not the one-way link but our own manifest, which fixes the fountain
   symbol size for the session and therefore freezes every capacity-changing knob (OQ-037).
5. **OQ-028 — simulator fidelity.** Phase 1 produces conclusions that several architectural
   decisions depend on, and their validity is unknown until Phase 2 calibration.
