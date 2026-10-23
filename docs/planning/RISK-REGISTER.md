# Risk register

> **Status:** Draft
> **Owner:** Project lead
> **Last reviewed:** 2026-08-02
> **Related:** ROADMAP.md, EXPERIMENT-REGISTRY.md, ADR set

Likelihood and impact: `High` / `Medium` / `Low`.
**Trigger** = the observable event that means the risk is materialising and the mitigation
must start. A risk without a trigger is a worry, not a managed risk.

---

## Platform and hardware

### RISK-001 — Camera cannot deliver accessible uncompressed 120 fps frames
**Likelihood.** Medium · **Impact.** High (milestone 6 only) · **Owner.** RX
**Detail.** Confirmed `[FACT]` that high-speed sessions exclude `ImageReader`. Remaining
question is whether the GPU `SurfaceTexture` path delivers *genuinely distinct* frames at
120 fps, or whether the vendor pipeline duplicates them.
**Mitigation.** GPU path (ADR-0005); if that also fails, report milestone 6 as not
achievable on available hardware and say so plainly.
**Trigger.** EXP-007 finds duplicate or interpolated frames at 120 fps.

### RISK-002 — High-speed mode restricts usable outputs or resolutions
**Likelihood.** Medium-High · **Impact.** High · **Owner.** RX
**Detail.** `getHighSpeedVideoSizes` may not include a resolution sufficient to resolve our
densest grid, forcing a coarser grid exactly where we wanted more rate.
**Mitigation.** Enumerate in the capability probe; adapt grid to available resolution;
model the goodput consequence explicitly rather than assuming density is free.
**Trigger.** EXP-007 shows high-speed resolutions below the grid requirement.
**FIRED 2026-08-04 (F27, F28).** The SM-S948U1 offers exactly two high-speed modes, **720p and
1080p, both 240 fps** — nothing higher. At the 144×240 charter grid on its own 1440×3120 panel, a
1080p capture yields roughly **4.9 px/cell** across rows. So on the high-rate path the **receiver,
not the panel, bounds grid density**, and the 180×390 grid the panel permits is pointless at
240 fps. `[HYP]` on the px/cell figure — it assumes ~80% frame fill.
**Consequence:** EXP-001 and EXP-013 must treat **1080p capture as its own arm** rather than
assuming the transmitter is the limiting side. The mitigation's last clause — model the goodput
consequence rather than assuming density is free — is now a requirement, not a precaution.

### RISK-003 — Display requests do not yield deterministic presentation
**Likelihood.** Medium · **Impact.** High · **Owner.** TX
**Mitigation.** **Every frame carries its own sequence number and phase indicator**, so the
receiver never depends on the transmitter's intended schedule. Presentation verification
where available; abort/renegotiate on mode change.
**Trigger.** EXP-006 shows presented-state rate diverging from requested by more than a
few percent.

### RISK-004 — Display and camera clocks create excessive frame mixtures
**Likelihood.** High (expected behaviour, not a defect) · **Impact.** Medium
**Mitigation.** Fountain coding absorbs the loss; M4 eventually recovers mixed frames;
`Pc` measured honestly rather than assumed.
**Trigger.** EXP-009 measures `Pc` below ~0.5.

### RISK-005 — Dense grids suffer moiré
**Likelihood.** Medium · **Impact.** Medium
**Mitigation.** Grid sweep finds the practical limit (EXP-001); interior-margin sampling
averages over the cell; avoid grid pitches that resonate with panel subpixel structure.
**Trigger.** EXP-001 shows a sharp non-monotonic error-rate response at specific pitches.

### RISK-011 — Device vendors misreport capabilities
**Likelihood.** High · **Impact.** Medium
**Mitigation.** Capability *verification*, not just enumeration (CAP-03). Verify requested
settings appear in returned `CaptureResult` metadata.
**Trigger.** Any device where requested manual settings are not reflected in results.
**First evidence 2026-08-04 (F28) — and it does NOT fire.** On the SM-S948U1 every requested
control is reflected in `CaptureResult`: exposure exact (4,166,666 ns), ISO quantised 400→398,
`CONTROL_AE_MODE` OFF, `EDGE_MODE` OFF, `NOISE_REDUCTION_MODE` OFF. One device, one session, so
"High" likelihood stands for the population — but the mitigation worked, and the read-back is now
routine in the recorder rather than a special exercise.

**Two cautions this evidence does not remove.** *Reporting* a mode is one step short of *behaving*
as it: proving `EDGE_MODE` OFF applies no sharpening needs a known high-spatial-frequency target,
i.e. a transmitter. And this risk's real sting arrived from the opposite direction — the rate
shortfall in F28 looked exactly like a vendor misreporting 60 fps, and RISK-011 supplied a
ready-made story for why. It was our own missing `SENSOR_FRAME_DURATION`. **A register entry that
makes a wrong conclusion more attractive is a hazard in its own right.**

### RISK-025 — The S26 Ultra's Privacy Display restricts the optical channel
**Likelihood.** High (the feature is present) · **Impact.** Medium-High · **Owner.** TX
**Detail.** The Galaxy S26 Ultra ships a hardware "Privacy Display" (Black Matrix / Flex
Magic Pixel) splitting subpixels into narrow- and wide-emitting groups. With it enabled the
screen is reported unreadable beyond ~30° off-axis `[LIT]`, which makes the receiving camera
a bystander **by design**. Press reporting further claims viewing angles are reduced versus
older panels **even with the feature off** `[LIT — unverified]`.
Third and subtlest: at 30 cm a 6.9" screen subtends a wide angle, so the receiver views the
corners further off-axis than the centre. Restricted angular emission then produces a
**radial luminance falloff belonging to the transmitter, not the lens** — it mimics
vignetting, worsens at closer range (exactly where we want density), and is not corrected
by lens-based assumptions.
**Mitigation.** Require Privacy Display off and detect/instruct; distributed pilot lattice
(layout Candidate B) absorbs an arbitrary illumination field by interpolation regardless of
cause — an unplanned argument for B over A; add a Privacy-on/off arm to EXP-003 and EXP-018.
**Trigger.** EXP-003 showing materially worse luminance separation at frame corners on the
S26 Ultra than on the Pixel 8 at the same distance and angle.
**Update 2026-08-02 (finding F7).** Severity **revised down for falloff alone**: simulation
shows pure multiplicative dimming does not defeat a global threshold until the bright level
falls by more than ~50%, because the black level sits near zero. Severity **revised up for
falloff combined with off-axis glare**, which is the realistic handheld case — the same
off-axis geometry that dims a privacy display also catches reflections, and glare lifts the
black level from the other side until the two regions' level ranges overlap. **EXP-003 and
EXP-018 must therefore vary ambient lighting AND angle together; varying either alone will
under-report this risk.** Mitigation implemented and tested (`PhotometricField`): at a
plausible falloff+glare setting, global thresholding produced 39 confident wrong payload
bits and the interpolated field produced 0.

### RISK-021 — Reference hardware unavailable or unrepresentative
**Likelihood.** Medium · **Impact.** Medium
**Detail.** The reference set has not yet been chosen or procured; milestone 6 needs a
120 fps-capable device that also permits the GPU capture path.
**Mitigation.** Choose and record the set early in `DEVICE-MATRIX.md`; verify high-speed
capability before committing to milestone 6 planning.
**Trigger.** Procurement finds no available device supporting the required combination.

---

## Signal and algorithm

### RISK-006 — Camera-side processing cannot keep up
**Likelihood.** Medium · **Impact.** High
**Mitigation.** GPU sampling; NEON on CPU; explicit back-pressure with deliberate,
logged frame dropping (RX-05) rather than unbounded queueing.
**Trigger.** Decode latency exceeding the frame period at target grid in EXP-016.

### RISK-007 — Multilevel symbols reduce goodput rather than increasing it
**Likelihood.** Medium · **Impact.** High (milestone 4 depends on M2)
**Detail.** M2 doubles `B` but reduces noise margin, forcing `Rfec` and `Pc` down. Net
effect could be neutral or negative. A secondary report suggests ShiftCode's greyscale mode
beat its four-colour mode `[LIT — unverified]`.
**Mitigation.** Gate M2/M3 on measured end-to-end goodput (EXP-013, EXP-014), not on
bits per cell.
**Trigger.** EXP-013 shows M2 goodput ≤ M0/M1 goodput.
**Consequence if it fires.** Milestone 4 would require `Fd > 60`, which depends on
RISK-001. **These two risks firing together would put milestone 4 out of reach**, and that
combination is the most serious scenario in this register.

### RISK-008 — Colour channels too unstable across phones
**Likelihood.** Medium-High · **Impact.** Low (M3 is P2)
**Mitigation.** M3 is optional and gated; chroma subsampling constraint already suggests
M3 starts behind M2.
**Trigger.** EXP-014 shows constellation separation varying widely across device pairs.

### RISK-018 — Differential modulation does not pay
**Likelihood.** Medium · **Impact.** Medium
**Detail.** ADR-0008 is our lowest-confidence decision. M1b halves `Fd`; M1a/d propagate
errors under frame loss — in a channel where frame loss is normal.
**Mitigation.** EXP-010 tests cheaply in the simulator before device implementation;
explicit gate to supersede ADR-0008 and move effort to M2.
**Trigger.** EXP-010 shows no temporal scheme beating calibrated M0 on goodput.

### RISK-022 — One-way link makes adaptation ineffective
**Likelihood.** Medium-High · **Impact.** Medium
**Detail.** With no reverse channel the transmitter cannot learn channel state, so it must
cycle profiles and waste display states on ones that do not work (OQ-013). This is
currently the least-designed part of the architecture.
**Mitigation.** Design the profile schedule carefully; consider prioritising reverse
optical control (Phase 9) earlier than planned.
**Trigger.** Phase 5 shows profile-cycling waste exceeding ~15% of display states.
**Update 2026-08-03 (EXP-023, findings F20/F23).** Severity **revised up**, and the shape of
the risk changed.

Half of it is retired: the receiver can now identify the goodput-optimal code rate **exactly**,
from telemetry it already produces, at the cost of one histogram (rung distance 0 on 3/3
scoreable channels). Adaptation is not ineffective for want of an estimator.

The other half is worse than recorded. **A wrong fixed choice costs 50%, 67% and 93% of
available goodput** on the clean, mild and impaired simulated channels — far above the ~15%
trigger, which was written about *cycling waste* and turns out to understate the *guessing*
penalty by several times. And the immediate blocker is not the missing reverse channel at all:
every capacity-changing knob alters the fountain symbol size, which the manifest fixes for the
session, so **there is currently nothing to cycle** (OQ-037). Simulated and uncalibrated
(RISK-024), so this raises the priority of measuring the same spread on real captures rather
than of building ADP-04 immediately.
**Revised trigger.** Real captures showing a goodput spread across the code-rate ladder wider
than ~30%, *or* OQ-037 resolved and profile cycling then measured above ~15% waste.

---

## Coding and dependencies

### RISK-015 — FEC implementation too slow
**Likelihood.** Medium · **Impact.** High
**Mitigation.** EXP-011 measures ARM64 decode time as a first-class selection criterion;
adaptive code rate; GPU decode as a last resort.
**Trigger.** EXP-011 shows no candidate meeting the throughput budget under throttle.

### RISK-016 — Fountain library licensing or patent exposure
**Likelihood.** Medium · **Impact.** Medium
**Detail.** Qualcomm's RFC 6330 IPR declaration offers non-assert for devices **not**
implementing a wireless wide-area standard; smartphones do implement such standards, so on
a plain reading our targets fall in the licence-required tier. `[FACT]` Not a legal
conclusion — a flag.
**Mitigation.** Legal review (RT-07) **before** RaptorQ becomes a dependency; LT or online
codes as fallback at a few percent overhead cost (quantified by EXP-012).
**Trigger.** Legal review returns unfavourable, or is not completed before the fountain
layer needs a decision.
**Status 2026-10-23: RETIRED as a schedule risk** (ADR-0015, F49). The fountain layer needed its
decision and got one that needs no licence: a systematic random linear fountain over GF(2), within
~1.6 symbols of optimal at k ≥ 64. Note the mitigation above was *wrong in its number*: v1's LT
fallback cost not "a few percent" but 20–35% of goodput under loss — because of how it combined
with a systematic prefix, not because LT is inherently that bad. RaptorQ would now matter only for
blocks far beyond the 1024-symbol bound.

### RISK-023 — In-house coding implementations underperform or take too long
**Likelihood.** Medium · **Impact.** Medium
**Detail.** The FEC evaluation leans toward building LDPC and possibly the fountain code
in-house. That is a real cost and a real schedule risk.
**Mitigation.** EXP-011 explicitly includes off-the-shelf options; if one performs
adequately, **take it** — the bias toward building is a preference, not a requirement.
**Trigger.** In-house LDPC work exceeding its Phase 1 allocation without meeting targets.

---

## System behaviour

### RISK-010 — Thermal throttling reduces sustained performance
**Likelihood.** High · **Impact.** Medium
**Detail.** Maximum brightness + full-rate capture + GPU sampling + FEC decode, sustained
for a large transfer, on a phone. Throttling is close to certain; the question is how much.
**Mitigation.** Thermal-aware adaptation (ADP-05); characterise the degradation curve
(EXP-020); report sustained goodput, not peak.
**Trigger.** EXP-020 shows goodput falling more than ~20% within a 100 MB transfer.

### RISK-014 — Excessive battery use
**Likelihood.** High · **Impact.** Low-Medium
**Mitigation.** Measure and report battery drain as a benchmark metric; it is a product
concern, not a research blocker.
**Trigger.** Benchmark shows drain that would make realistic transfers impractical.

### RISK-012 — Screen burn-in or flicker concerns
**Likelihood.** Low (burn-in), Medium (perceived flicker) · **Impact.** Medium
**Detail.** Sustained maximum-brightness high-contrast patterns on OLED. Static structural
elements (markers, header position) are the burn-in concern, not the payload region.
**Mitigation.** Randomise or slowly translate static elements between sessions; bound
session duration; document the risk to users.
**Trigger.** Any observed persistence on test hardware.

### RISK-013 — Visual discomfort
**Likelihood.** Medium · **Impact.** Medium
**Detail.** A rapidly changing high-contrast full-screen pattern is uncomfortable to look
at, and rapid flicker warrants care around photosensitivity.
**Mitigation.** Warn before starting (UI-06); the receiving user need not look at the
screen; consider a reduced-contrast profile at a goodput cost.
**Trigger.** Any tester reporting discomfort.

---

## Process and integrity

### RISK-017 — Test results fail to reproduce across devices
**Likelihood.** High · **Impact.** Medium
**Mitigation.** Cross-device benchmark category from Phase 4 (not Phase 10); report spread,
never best runs; capture harness preserves difficult cases as regression tests.
**Trigger.** Goodput varying more than ~2× across the reference set under identical conditions.

### RISK-019 — Project optimises raw rate instead of useful throughput
**Likelihood.** Medium · **Impact.** High
**Detail.** The failure mode ADR-0012 exists to prevent. ChromaCode's data — goodput
falling ~119× while raw rate fell ~6× `[LIT]` — shows how misleading raw rate can be.
**Mitigation.** ADR-0012; the six-rate labelling discipline; goodput as the only
acceptance gate.
**Trigger.** Any accepted optimisation justified by a component metric alone.

### RISK-020 — Documentation diverges from implementation
**Likelihood.** High · **Impact.** Medium
**Mitigation.** CONTRIBUTING rules; ADRs amended when experiments contradict them;
periodic divergence review (DOC-05); automated link checking (DOC-04, **done** — the `docs` CI
job).
**Trigger.** Any ADR contradicted by an experiment but left unamended.
**Update 2026-08-04 — it fired again, an order of magnitude larger (F30).** An audit before
starting the transmitter found **~30 entries in FEATURE-REGISTRY marked `Planned` that were built,
tested and written up in F2–F19**: the whole fountain layer, intra-frame FEC, the interleaver,
header codec, hash gate, filename sanitisation, bounds/overflow/fuzzing, and the entire CV chain.
A fresh reader would have concluded the project had a simulator and little else.

Worse, the **recurrence rate**: PHASE1-FINDINGS' own "not validated" list was wrong **three times
in about thirty hours**, each time understating progress, each time corrected within a day of being
written. The cause is structural — CONTRIBUTING's update rule is honoured for *documents* and
skipped for *code*, because the registry is not where work happens and **nothing fails when it goes
stale**. Link checking cannot catch it; every stale entry had valid links.

Two changes rather than another promise to be careful: `DOC-05` is re-scoped from a one-off task to
a **periodic** review, and the registry now separates **implemented** from **acceptance met** so an
entry cannot be silently wrong in either direction.

**Update 2026-08-03 — this risk has fired repeatedly, and "High" is if anything generous.** A
divergence review after the C14 work found five items, none of which any automated check could
have caught, because **every one had valid links and compiling code**:

| Divergence | Nature |
|---|---|
| `README.md` said "Phase 0 — there is no implementation yet" | False through the entire Phase 1 build. The front page. |
| `README.md` said the licence was unselected | OQ-021 closed 2026-08-02; `LICENSE` was already committed |
| `PHASE1-FINDINGS.md` "what has NOT been validated" claimed no CV path and no payload FEC | Both landed in F9/F10/F17 and F18 |
| `ENGINEERING-PRACTICES.md` §2 repo tree named ten `core/src/` subdirectories | None exist; `core/src/` is flat. Second instance — the `protocol/` note under D1 was the first |
| `EXPERIMENT-REGISTRY.md` front matter said "no experiment has been run" | EXP-023 had run |

**The pattern worth acting on:** every one is a *status claim inside a document whose subject is
something else*, and status claims are what go stale. Link checking cannot see them and neither
can the compiler. The mitigations that would work are a scheduled divergence review with an
explicit checklist of the counts and status lines that drift (test count, doc count, registry
ranges, phase, "what exists" lists), or moving those claims out of prose into generated output.
DOC-05 is currently unscheduled, which is the actual gap.

### RISK-009 — Protocol parser accepts unsafe values
**Likelihood.** Medium · **Impact.** High
**Detail.** The receiver parses attacker-controlled optical input in C++.
**Mitigation.** Bounds before allocation; integer overflow discipline; fuzzing from
Phase 4; ASan/UBSan in CI; malformed-input test vectors.
**Trigger.** Any fuzzing crash, or any allocation whose size derives from unvalidated input.
**See.** [INPUT-VALIDATION.md](../security/INPUT-VALIDATION.md)

### RISK-024 — Simulator models a kinder channel than reality
**Likelihood.** High · **Impact.** High
**Detail.** Phase 1 produces conclusions from a simulator whose fidelity is unvalidated
until Phase 2. Several architectural decisions (EXP-010, EXP-011, EXP-012) would rest on it.
**Mitigation.** Phase 2 calibration against recorded captures, with predicted-versus-
measured error reported as a number; simulator results tagged `[HYP]` until calibrated;
no simulator-only result treated as predictive.
**Trigger.** Phase 2 calibration showing predicted symbol error rates materially better
than measured ones.

---

## Highest-priority risks

Ranked by expected impact on delivering the milestones:

| Rank | Risk | Why |
|---|---|---|
| 1 | **RISK-007 + RISK-001 together** | If multilevel does not pay *and* >60 states/s is unavailable, milestone 4 is unreachable. These are the two independent routes to the headline target |
| 2 | RISK-024 | Would invalidate the Phase 1 conclusions the whole plan rests on |
| 3 | RISK-019 | Would silently misdirect the entire optimisation effort |
| 4 | RISK-009 | Security defect in attacker-facing C++ |
| 5 | RISK-010 | Directly attacks *sustained* goodput, which is what the milestones actually require |
