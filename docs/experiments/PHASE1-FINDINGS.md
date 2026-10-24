# Phase 1 findings — scaffolding and first implementation

> **Status:** Current
> **Owner:** Architecture
> **Last reviewed:** 2026-10-23
> **Related:** ADR-0006, ADR-0009, ADR-0013, ADR-0015, OPTICAL-FRAME-CANDIDATES.md,
> PERFORMANCE-MODEL.md, MODULATION-SPEC.md, EXPERIMENT-REGISTRY.md

Findings F1–F60: F1–F23 from implementing the Phase 1 core, F24–F36 from first contact with
real hardware, F37–F44 from putting the thing in an operator's hands, F45–F48 from the first
above-cliff capture, F49–F52 from the 2026-10-23 review that produced protocol v2 (ADR-0015),
F53–F55 from making the receiver work in a moving hand, and F56–F59 from the first hand-held
two-phone test. Recorded here because several are
**design defects the specifications did not anticipate**, one is a **retraction** (F14), and
several are model-versus-implementation gaps. Almost all were surfaced by tests or by building
a consumer for something that already existed — not by inspection.

**If you read only a few:** F14 (retracted — do not plan around a detection floor), F15
(localisation must never depend on payload), F18 (intra-frame FEC is load-bearing), F20 (the
code-rate ladder can be scored for free, and the answer cannot be delivered), F23 (erasures are
not what the code spends), F36 (first real data off a screen), F43 (four verified fixes that
changed nothing the operator saw), F44 (a component that reported a transform it did not apply),
F49 (the fountain code was the loss, and swapping the decoder would not have found it), F52 (no
real capture could ever have completed a transfer).

---

## F1 — The distributed pilot lattice produced only ONE reference level `[FACT]`

**Severity: high. Would have silently broken photometric normalisation on every frame.**

`PilotValue()` alternated on `(col + row) % 2`. But lattice points sit at
`col, row ≡ pitch/2 (mod pitch)`, so `col + row` is **always even** across the entire
lattice — every pilot rendered bright, and the receiver could never estimate the dark
reference. `EstimateReference()` fell back to a default dark of 0 against a measured bright,
placing the threshold at roughly half the true value.

On an identity channel this still decoded (the threshold was wrong but on the right side of
every sample), which is exactly why it would have survived casual testing and then failed
in the field as soon as the channel had any offset.

**Fix:** alternate on **lattice indices** (`col/pitch + row/pitch`), not raw coordinates.

**Generalisation worth remembering:** any parity or periodic function evaluated over a
sub-lattice can be constant over that sub-lattice. This will recur in timing tracks, guard
patterns and the M3 colour constellation. Caught by
`M0.DegenerateSeparationForcesErasures`.

---

## F2 — The fountain layer ingested corrupted symbols `[FACT]`

**Severity: high. Violated a rule the protocol spec already stated.**

PROTOCOL-SPEC says "uncorrectable ⇒ erasure signalled upward… never pass through
best-effort data." The implementation did exactly the opposite: occluded cells became
zeros, were packed into a symbol, and were handed to the fountain decoder as if valid.

**A fountain code is an ERASURE code, not an error-detecting one.** It cannot distinguish a
damaged symbol from a good one. A single corrupt symbol propagates through peeling into
every symbol that references it, and surfaces only as a final SHA-256 mismatch — with no
way to attribute the failure or recover from it. The transfer fails completely rather than
degrading.

**Fix:** a `payload_crc` field in every frame header. The receiver verifies it before
ingesting; a mismatch makes the frame a **frame erasure**, which is precisely what the
fountain layer is designed to absorb. The transfer now completes correctly instead of
failing.

**Consequences worth noting:**
- Frame header grew by 4 bytes (`kPlainSize` 28 → 32).
- This CRC is **not** a substitute for the end-to-end SHA-256. A malicious transmitter
  controls the CRC too, so the hash gate remains the last line of defence — covered by
  `Transfer.HashGateCatchesCorruptionThatEvadesEveryChecksum`.
- Caught by `EndToEnd.OcclusionProducesErasuresAndStillVerifies`.

**Architectural lesson:** the erasure-propagation discipline in DATA-FLOW.md is not
self-enforcing. Every boundary where soft information becomes bytes needs an explicit
integrity check, or the erasure information is silently discarded.

---

## F3 — Measured layout overhead is far below the modelled range `[FACT]`

The implemented Candidate B layout at 120×200 gives **`O` = 0.0831**. The performance model
assumed **0.12–0.20**.

The model was pessimistic: 22,005 of 24,000 cells carry payload. This is good news, but it
means **the model's scenarios understate goodput slightly** for this layout, and the
`O` inputs should be updated once the layout stabilises.

> **Amended 2026-08-02.** First recorded as `O` = 0.0595 (22,573 payload cells). Adding the
> persistent boundary ring (finding F10) spent 568 cells, moving `O` to 0.0831. Recorded
> rather than silently corrected: the ring is a real and deliberate cost paid for screen
> localisation, and the point of the original finding — that the model's `O` band is too
> pessimistic — survives it comfortably.

Caveat before anyone gets excited: the model's sensitivity analysis shows `O` has only a
~7% goodput swing, so this is the least important variable to have been wrong about. The
high-leverage inputs (`Pc`, `Fd`, `B`, `Rfec`) remain entirely unvalidated.

**Action:** revisit `O` in `tools/perf_model/perf_model.py` after EXP-013 selects a layout.
Do not retune now — the layout is not final.

---

## F4 — Simulated goodput, first measurement `[HYP]`

From `ffsim` at 120×200, M0 binary, assumed `Fd` = 60 states/s:

| Channel | Payload goodput | Fountain overhead |
|---|---|---|
| Identity (no impairment) | **120.0 KB/s** | 0.00 |
| Noise σ=12, shot 0.6, crosstalk 0.15, γ=2.2, vignetting 0.35, 20% frame loss | **33.0 KB/s** | 0.54 |

**These are simulator outputs, not measurements.** They are `[HYP]` until Phase 2
calibration (SIM-03, RISK-024) and the channel model is currently uncalibrated and
probably kind.

Two observations that matter:

1. **The clean figure (120 KB/s) exceeds the model's binary@60 expected case (78 KB/s)**,
   consistent with F3's lower overhead and with there being no payload FEC yet
   (`Rfec` effectively 1.0). It is not evidence of anything except internal consistency.

2. **The impaired figure collapses to 33 KB/s** — a 3.6× drop — driven mostly by **LT
   fountain overhead of 0.54**, far worse than RaptorQ's ~2% class. This is early,
   uncalibrated evidence that **the LT fallback is expensive**, and it raises the value of
   resolving the RaptorQ licensing question (RT-07, RISK-016). EXP-012 should quantify this
   properly and the result should feed directly into whether legal review is worth pursuing.

Also note: **block padding waste.** 262,144 payload bytes into 2 blocks × 64 symbols ×
2,821 bytes = 361,088 bytes of capacity — roughly 27% padding. Variable last-block sizing
is an obvious optimisation not yet implemented.

---

## F5 — Apple clang ships no libFuzzer `[FACT]`

`libclang_rt.fuzzer_osx.a` is not present in the Xcode toolchain, so fuzz targets cannot be
built with the macOS system compiler. They are opt-in (`FILEFLOW_BUILD_FUZZ`, default OFF)
and run on Linux in CI; local macOS fuzzing needs Homebrew LLVM.

This matters because D1 chose plain C++20 **without** the hardened-parser hedge, making
fuzzing and sanitisers the primary defence on the attacker-facing surface. ASan and UBSan
do work with Apple clang and run clean, so local coverage is not absent — but the fuzzing
half of that defence is CI-only on macOS development machines.

---

## F6 — The colour-pilot rule starved the WHITE reference level `[FACT]`

**Found:** 2026-08-02, while building the photometric field (component C09).

The layout reserves every fourth pilot-lattice point for colour (unused until M3). The rule
was `(i+j) % 4 == 0`. But `PilotValue()` alternates bright/dark on `(i+j)` parity, and
`(i+j)%4==0` implies `(i+j)` is **even** — so the reservation drew **exclusively from the
bright class**. Measured over a 64×64 lattice: bright 25%, dark 50%. **The white level was
estimated from half as many pilots as the black level.**

That is exactly backwards. The white level is the one degraded by angular luminance falloff
(RISK-025) and by any exposure shortfall, so it needs *more* support than the black level,
not half. Dark cells sit near zero and are comparatively stable.

**Fix:** `(i + 2j) % 4 == 0` selects the same 25% of lattice points while splitting evenly
across parities — measured bright 37.5%, dark 37.5%. One line, in `grid.cpp`.

**Why it was invisible:** every existing test used a flat illumination field, where one
bright pilot is as good as fifty. The defect only bites when the bright level *varies*,
which is the case we care about. Regression test:
`PhotometricField.PilotLatticeIsBalancedAcrossBothLevels`.

---

## F7 — Multiplicative falloff alone does NOT defeat a global threshold `[FACT]`

**Found:** 2026-08-02, while writing the RISK-025 mitigation test.

The first version of the test applied radial luminance falloff (corners at 40% of centre)
and expected a global threshold to fail. **It did not — zero errors.**

The reason is a genuine property of the channel, not a test bug. With a near-zero black
level, the global threshold sits at roughly half the mean bright level, so the *bright* level
must fall by **more than ~50%** before a cell crosses it. Pure multiplicative dimming is
therefore surprisingly benign for binary luminance.

What *does* defeat a global threshold is a **spatially varying black-level lift** —
directional glare, i.e. ambient light reflecting off the panel — because it squeezes from
the other side. Glare on one region plus falloff on another makes the two regions' level
ranges *overlap*, and then no single threshold separates both.

**Consequences:**
1. The simulator's impairment set needs a **directional glare gradient**, not just uniform
   black lift. Added to the test renderer; should be promoted into `sim/`.
2. RISK-025's severity estimate should be **revised down for falloff alone** and **up for
   falloff combined with off-axis glare** — which is the realistic handheld case, since the
   same off-axis geometry that dims a privacy display also catches reflections.
3. EXP-003 and EXP-018 must vary ambient lighting **and** angle together. Varying either
   alone will under-report the problem.

**Measured at the settings now in the test:** global thresholding → **39 payload errors**
(~1.3% raw BER, all confident wrong bits); interpolated field → **0**.

---

## F8 — Interpolation hides occlusion unless residual is checked `[FACT]`

**Found:** 2026-08-02, same session.

Inverse-distance weighting is *dangerously* well-behaved. When a block of the frame was
occluded (pilots reading a flat mid-grey), the field simply blended in pilots from outside
the damaged area and returned a **confident, healthy-looking reference** for the occluded
region. Local separation stayed large, nothing was erased, and every payload cell there
would have decoded to a confident wrong bit — the most expensive failure mode there is.

The interpolator cannot detect this from its output; it needs to notice that **its own model
does not fit**. Added a per-node **pilot-fit residual**: the weighted spread of contributing
pilots about their fitted level. Where pilots that should agree do not, the region is
erased regardless of how healthy its separation looks.

**This generalises well beyond occlusion** — it is the same signal for a finger over the
screen, a glare hotspot, a tracking error that has shifted the sampling grid off the cells,
or a partially-visible frame. It is the first real **detection-confidence** mechanism in the
receiver, and the optical-frame spec already called for confidence fields without saying how
they would be computed. This is how.

Exposed as `PhotometricField::ResidualAt()` so telemetry can report *why* cells were erased.
Test: `PhotometricField.OccludedRegionErasesRatherThanGuesses`.

---

## F9 — The corner markers could not resolve orientation at all `[FACT]`

**Found:** 2026-08-02, when every screen-detection test failed at once.

The markers distinguished corners by a **notch of 0–3 cells** knocked out of a 6×6 block.
Measured pairwise Hamming distances between the four corner patterns:

| | id0 | id1 | id2 | id3 |
|---|--:|--:|--:|--:|
| **id0** | — | **1** | 2 | 3 |
| **id1** | | — | **1** | 2 |
| **id2** | | | — | **1** |

**Minimum distance: 1 cell out of 36.** Worst-case rotation margin **0.028** — a 2.8%
signal that a single noisy cell flips. Orientation was effectively a coin toss, and a
90° error decodes to pure garbage.

Worse, the notch rule (`lc == 3 && lr < 3 + corner_id`) hardcoded index 3, so it silently
produced **no notch at all** for the top-left corner and broke entirely for
`marker_size < 4`.

**Fix — and the reason it is a redesign, not a patch:** once localisation moved to the
persistent boundary ring, the markers stopped needing to be a *detection* target and became
purely a **4-way code**. Designed as such: outer edge always bright (anchors the ring),
`d==1` always dark (every corner keeps visible dark structure), and the entire inner block
carries a 2-bit corner ID as {solid bright, solid dark, checker, inverse checker}.

**Minimum distance 8/36; worst-case rotation margin 0.222 — 8× better.** All eleven
detection tests passed immediately on the redesign.

**Generalisable lesson:** the notch was designed when markers were expected to do both
localisation *and* orientation, and it was a reasonable compromise for that job. Splitting
the two responsibilities did not just simplify the detector — it removed the constraint that
was making the code weak. Regression test asserts the Hamming property directly
(`MarkerCodeHasEnoughHammingDistanceToResolveRotation`), not merely its downstream effect,
so a future redesign cannot quietly reintroduce a weak code.

---

## F10 — The layout had no persistent boundary, though the spec required one `[FACT]`

**Found:** 2026-08-02, while designing screen localisation.

`OPTICAL-FRAME-CANDIDATES.md` lists "persistent screen boundary — cheap edge tracking,
subpixel refinement" as a required frame element. **Candidate B never implemented one.**
Perimeter cells were payload (left/right edges) or alternating timing track (top/bottom).

This is a documentation-versus-implementation divergence of exactly the kind
CONTRIBUTING.md warns about, and it was only exposed by trying to build the component that
depended on it.

**Implemented:** a 1-cell always-bright perimeter ring (`CellRole::kBoundary`), with the
timing tracks moved one cell inward. Costs `2*(cols+rows)-4` cells — ~2.7% at 120×200.

**Why it is worth 2.7%:** it converts screen localisation from "find and centroid four small
fiducials" into "fit four long straight lines and intersect them". Every pixel along an edge
constrains the fit, so the corners come out far more accurately, and unlike small fiducials
the signal does not degrade first with distance and defocus. Measured worst cell-centre error
against ground truth: **< 0.25 cells** across the frame.

It also makes the boundary *persistent* in the useful sense — it is present in every frame
regardless of payload, so a frame whose payload decode fails still contributes a geometry
update, which is what makes tracking cheaper than re-acquisition (the ADR-0006 bet).

---

## F11 — `FF_ASSIGN_OR_RETURN` collided with itself `[FACT]`

**Found:** 2026-08-02, the first time the macro was used twice in one function.

```c
#define FF_ASSIGN_OR_RETURN(decl, expr)  auto _ff_r_##__LINE__ = (expr); ...
```

`##` pastes **before** `__LINE__` expands, so every expansion declared the same literal
identifier `_ff_r___LINE__`. Two uses in a scope → redefinition error. Fixed with the
standard two-level indirection (`FF_CONCAT` → `FF_CONCAT_INNER`).

Low severity — it fails loudly at compile time and cannot produce wrong runtime behaviour.
Recorded because it is a good example of a defect that looks fine, passes every test, and is
simply *waiting* for the first caller to do the obvious thing. The whole macro existed for
exactly the two-fallible-calls-in-a-row case and had never been used for it.

---

## F12 — The detector allocated a point per lit pixel `[FACT]`

**Found:** 2026-08-02, reviewing the detector's hot path.

`ScreenDetector::Detect` collected every lit pixel into a `std::vector<Point2>` and then
scanned it for four extremes. Cost of that vector, computed for real capture resolutions:

| Capture | Lit points | Transient allocation |
|---|--:|--:|
| 1080p preview | 0.5 M | 8 MB / frame |
| 4K video | 2.1 M | 32 MB / frame |
| Pixel 8, 12 MP | 3.1 M | **48 MB / frame** |
| S26 Ultra, full 200 MP | 49.9 M | **762 MB / frame** |

At 4K60 that is **~1.85 GB/s of transient allocation**. Three separate problems:

1. Unusable on the hot path at any realistic capture resolution.
2. The allocation size is chosen by **attacker-controlled pixel content** — a hostile
   all-bright frame maximises it. Resource exhaustion via untrusted input (THREAT-MODEL T2).
3. All of it to compute four extremes, which need **no storage whatsoever**.

**Fix:** a streaming accumulator, O(1) state, zero allocation. Note the seeding: the
accumulators start at ±1e300, not 0, because a lit pixel at (0,0) has both sum and diff equal
to zero and a 0-seeded accumulator would silently refuse to record it — which would shift the
detected top-left corner inward on any screen flush with the image origin. Regression tests:
`FullyLitImageCostsNoAllocation`, `FindsAScreenTouchingTheImageOrigin`.

---

## F13 — The tracking saving is geometry-bound, and vanishes in our target regime `[HYP]`

**Found:** 2026-08-02, measuring ADR-0006's central claim in our own code.

ADR-0006 asserts a persistent tracked grid beats repeated independent detection. Now that
`ScreenTracker` exists, that claim is measurable. Pixels examined per frame, tracked vs
full acquisition, simulated:

| Screen area / frame area | tracked ÷ acquired pixels |
|--:|--:|
| 0.645 | **1.000 — no saving at all** |
| 0.300 | 0.673 |
| 0.133 | 0.299 |
| 0.075 | 0.168 |

The relationship is simply `ratio ≈ (1 + 2·margin)² × (screen area / frame area)`. The saving
comes from **how little of the frame the screen occupies**, not from tracking being clever.

**The uncomfortable part:** we *want* the screen to fill the camera frame, because that
maximises pixels per cell and therefore resolvable grid density. So the configuration we are
optimising for is exactly the one where this benefit of tracking is **zero**.

This does not sink ADR-0006, but it does narrow it. Tracking still buys temporal consistency
of the homography and fewer opportunities to lock onto the wrong bright object — but those
are *different* claims and must be argued and measured separately rather than folded into
"tracking is cheaper". The pixel-count argument, stated plainly, is much weaker than the ADR
currently implies.

### RESOLVED, same day — boundary-annulus search

The corner extremes lie **on** the boundary ring, so scanning the quad's interior finds
nothing and costs everything. Replacing the dilated-bounding-box window with an annulus
around the boundary:

| Screen area / frame | bounding box | **annulus** | improvement |
|--:|--:|--:|--:|
| 0.645 | 1.000 | **0.554** | 1.8× |
| 0.300 | 0.673 | **0.299** | 2.3× |
| 0.133 | 0.299 | **0.133** | 2.2× |
| 0.075 | 0.168 | **0.075** | 2.2× |

Headline case: **0.415 → 0.185**. The saving now holds across the whole range, including
the dense-framing regime where it had previously vanished entirely.

The ratio now lands at roughly the screen-area fraction itself, which is the expected result:
annulus area ≈ perimeter × band width, and with band width proportional to quad size that is
proportional to quad area.

One design detail that is load-bearing: the annulus is built by scaling the quad about its
**centroid**, not by insetting a bounding box. A bbox annulus does not follow a *rotated*
quad's boundary, so it would miss the ring exactly when the receiver is held at an angle —
the common case, not the exception. Test: `AnnulusTracksARotatedScreen`.

A second consequence: tracked frames now reuse the binarisation threshold from acquisition
rather than recomputing Otsu, which would have cost a full-image pass and defeated the point.
That is consistent with the design rather than a shortcut — exposure, ISO and white balance
are locked for the session precisely so the photometric channel stays stable — and it is
self-correcting, since drift fails marker verification and triggers reacquisition.

**Status of the ADR-0006 concern:** largely resolved for the pixel-cost claim. The wider point
stands and is worth keeping: the saving was never *intrinsic* to tracking, it is a property of
the search region, and it took a measurement to notice that the obvious implementation threw
it away in the one regime that matters.

---

## F14 — ~~Detection has a floor of roughly 6 pixels per cell~~ **RETRACTED** `[WRONG]`

> **RETRACTED 2026-08-02, the same day it was recorded.** The claim was an artifact of the
> F15 bug, and the corrected number is not a resolution limit either. The original text is
> kept below because the retraction is more instructive than the finding was, and because
> deleting unfavourable results is forbidden (CONTRIBUTING.md).

### What was originally claimed

A sweep of apparent screen size on a 48×80 grid put the detection floor **between 5.42 and
6.25 px/cell**, and I drew operating-envelope conclusions from it — notably that a 144-column
grid with the screen filling only 70% of frame width would fail outright, making tight framing
close to mandatory.

### Why it was wrong, twice over

**First error: it measured the F15 bug.** That sweep ran *before* the payload-dependence fix.
Shrinking the screen also shrank the lit-pixel count, and the old area test compared lit pixels
against the image area — so small screens were refused for having too few *lit pixels*, not for
being unresolvable. Re-running the identical sweep after the F15 fix moves the boundary from
5.42–6.25 down to **3.75–4.58 px/cell**.

**Second error: the remaining boundary is a config threshold, not optics.** Checking the
geometry of the four sweep points against `min_area_fraction = 0.03`:

| px/cell | quad area ÷ image | vs 0.03 | observed |
|--:|--:|:--|:--|
| 6.25 | 0.0750 | pass | detected |
| 5.42 | 0.0563 | pass | detected |
| 4.58 | 0.0403 | pass | detected |
| 3.75 | **0.0270** | **fail** | refused |

The transition sits **exactly** on `min_area_fraction`. The detector never reached a resolution
limit anywhere in that sweep — it was rejecting screens for being too small a *fraction of the
frame*, which is a deliberate configuration choice about what counts as a plausible screen.

### What we actually know

**We have not found the density cliff.** The independent grid sweep
(`tools/grid_sweep.sh`) decodes cleanly all the way to **192×320 at 5.00 px/cell** — header
success 1.0000, worst geometric error 0.28 cells, verified transfer, goodput rising
monotonically 120 → 240 KB/s across the range.

**And that null result should not be believed.** The renderer does nearest-neighbour cell
lookup plus an optional box blur. It does **not** model display subpixel structure, camera
sensor MTF or anti-alias filtering, moiré between the display grid and the sensor grid, or
diffraction — and those are precisely the mechanisms that produce a density cliff in real
systems. ChromaCode measured a real cliff on real hardware; our simulator finding no cliff is
the expected consequence of not modelling the physics that causes one, **not** evidence that
none exists.

### Consequences

- ADR-0006 and two source comments cited the ~6 px/cell figure. Corrected.
- The operating-envelope conclusions drawn from it were unfounded and are withdrawn. Tight
  framing may still be desirable for pixels-per-cell, but it is **not** forced by a detection
  floor we have demonstrated.
- **EXP-001 cannot be answered in simulation with the current renderer.** Locating the density
  cliff requires either real captures or a renderer modelling display/sensor sampling. This
  is now the strongest argument for prioritising the capture harness over further simulator
  work, and it is a concrete instance of RISK-024 — a simulator kinder than reality producing
  a confident wrong conclusion.

---

## F15 — Screen localisation depended on payload data `[FACT]`

**Found:** 2026-08-02, chasing a discrepancy in the ffsim tracking A/B. The tracked run needed
64 frames; the per-frame-acquisition run needed **447**, with header success **0.33** and
**301 detection failures**. Identical geometry, identical channel — so something other than
geometry was deciding whether the screen could be found.

**Mechanism.** `ScreenDetector::Detect` tested `min_area_fraction` — documented as *"the screen
must occupy at least this fraction of the image"* — against the **count of lit pixels**. Those
are different quantities, and the lit-pixel count depends on what is being transmitted:

| Frame | Bright cells | Lit px ÷ image | Result |
|---|--:|--:|---|
| Real data | 12,048 / 24,000 | 0.321 | detected |
| Zero-padded | 1,075 / 24,000 | **0.0287** | **refused** (needs 0.030) |

The screen occupied ~64% of the image in *both* cases. Of 60 real transmitter frames, **54 were
refused, every one a systematic symbol** from the zero-padded tail — a 16 KB payload into a
176 KB block leaves most systematic symbols entirely zero.

**Why this was worse than the failure rate suggests.** It made screen localisation depend on
payload content, which is exactly what the persistent boundary ring and corner markers exist to
prevent. In the field it would have appeared as unreproducible flakiness on any file containing
a run of zeros, or on any transfer whose final block was padded — the sort of defect that is
near-impossible to diagnose on hardware and trivially visible in simulation.

**Fix.** Test quad area, which is what the check always meant. Costs nothing: the pixel scan
runs either way, and the early-out only skipped a homography solve and marker scoring. Spurious
quads from scattered noise still fail marker verification, which is the real gate.
**54/60 refused → 0/60.** Regression tests: `RealTransmitterFramesReproduceTheFfsimFailure`,
`DetectsAnAlmostEntirelyDarkFrame`.

**A correction to my own reasoning, recorded because the error was instructive.** Mid-investigation
the evidence looked like "tracking is more *reliable* than per-frame detection, because it freezes
the binarisation threshold instead of re-deriving it from changing content". That was a plausible
mechanism and it was wrong. With F15 fixed, both paths decode identically — 64 frames, H = 1.0,
zero failures. The entire reliability gap was this bug. **Tracking buys speed, not accuracy**, and
had the bug not been chased to the bottom the project would have acquired a confident,
wrong belief about why its central architectural bet works.

---

## F16 — Grid sweep: goodput scales with density, no cliff found `[HYP]`

**Run:** 2026-08-02, `tools/grid_sweep.sh` — a simulator dry run of EXP-001 through the full
image path. 512 KB payload, 1200×2000 capture, head-on, no impairments.

| Grid | Cells | px/cell | Geom err (cells) | Frames | `H` | Goodput | Verified |
|---|--:|--:|--:|--:|--:|--:|:--|
| 48×80 … 96×160 | — | — | — | — | — | — | header too big for grid |
| 108×180 | 19,440 | 8.89 | 0.153 | 256 | 1.0000 | 120 KB/s | yes |
| 120×200 | 24,000 | 8.00 | 0.168 | 192 | 1.0000 | 160 KB/s | yes |
| 132×220 | 29,040 | 7.27 | 0.187 | 192 | 1.0000 | 160 KB/s | yes |
| 144×240 | 34,560 | 6.67 | 0.207 | 192 | 1.0000 | 160 KB/s | yes |
| 156×260 | 40,560 | 6.15 | 0.162 | 128 | 1.0000 | 240 KB/s | yes |
| 168×280 | 47,040 | 5.71 | 0.239 | 128 | 1.0000 | 240 KB/s | yes |
| 192×320 | 61,440 | 5.00 | 0.279 | 128 | 1.0000 | 240 KB/s | yes |

Goodput rises monotonically 120 → 240 KB/s with no degradation anywhere: header success stays
at 1.0000 and worst geometric error stays under 0.28 cells throughout. Goodput is quantised
into steps because whole frames and whole blocks are quantised, not because of any channel
effect.

**The null result is the finding, and it must not be over-read.** See the F14 retraction: the
renderer models neither display subpixel structure, nor sensor MTF, nor moiré, nor diffraction
— exactly the mechanisms that produce a density cliff. Finding no cliff here is what a
simulator that omits the relevant physics is *guaranteed* to report.

**Two things that only showed up by running it:**

1. **The first attempt measured nothing.** With a 16 KB payload every grid finished in the same
   64 frames — one symbol per frame, block heavily under-filled — so goodput was pinned at
   `payload ÷ (64/Fd)` and every row read an identical 15.0 KB/s. A sweep can look perfectly
   healthy while varying nothing; the payload has to be large enough that per-frame capacity
   actually determines completion time.
2. **Small grids fail on the header, not the channel.** `FrameLayout::Create` validates geometry
   but cannot know how large the *coded* header is, so grids below ~108 columns build a
   valid-looking layout and then fail at render with a generic out-of-range error. `ffsim` now
   checks this where both facts are in scope and says so plainly; the sweep reports it as a
   configuration limit rather than letting it masquerade as a decode failure.

**Consequence for EXP-001:** it cannot be answered in simulation with the current renderer.
Locating the density cliff needs real captures, or a renderer that models display and sensor
sampling. This is the strongest argument yet for prioritising the capture harness.

---

## F17 — Replay is bit-identical to live decode `[FACT]`

**Built and proven:** 2026-08-02. The capture/replay harness (component C17) is the path every
real capture will arrive on, and its value depends entirely on one property: replaying a
recording must produce **exactly** what decoding those frames live produced. If it does not, a
bug reproducing in replay might be a harness artifact, and a fix validated against replayed
frames might not hold on device.

`ReplayIsBitIdenticalToLiveDecode` decodes rendered frames straight through `FramePipeline`,
then writes those same images to a bundle and replays them, requiring every cell sample to
match with `EXPECT_DOUBLE_EQ` — not a tolerance — and the pipeline diagnostics
(`frames_decoded`, `total_pixels_examined`) to match too. It passes.

**Establishing this before hardware exists is the point.** The first real capture lands on a
path already known to be faithful, so any surprise in that data is about the channel rather
than about our tooling.

**What made it possible:** extracting `FramePipeline` (component C06a). The simulator already
owned a detect→rectify→sample→normalise chain, and replay needed the same one. Rather than
duplicate it — which would have guaranteed eventual drift, and with it the quiet invalidation
of every recorded dataset — the chain became a single shared object that the simulator, the
harness and eventually the live receiver all drive.

**End-to-end today, with no hardware:** `ffsim --record DIR` writes a synthetic bundle,
`ffreplay DIR` decodes it through the production chain, and the ADR-0006 A/B runs on replayed
frames (831,518 vs 1,350,000 geometry pixels/frame, 1 acquisition + 108 refined). CI performs
the record→replay loop on every push.

---

## F18 — Intra-frame FEC was missing, and the channel does not work without it `[HYP]`

**Found and built:** 2026-08-02. ADR-0009 specifies intra-frame FEC **plus** cross-frame
fountain coding. The fountain half had worked since Phase 1; **the intra-frame half did not
exist.** A frame with a handful of unreadable cells failed its CRC and was discarded entirely,
throwing away thousands of good cells to lose a few bad ones, and the fountain then spent a
whole extra frame recovering what a few parity bytes could have fixed in place.

Worse, `ReedSolomon` had **no erasure decoding at all** — only blind error correction. Every
stage upstream exists to identify unreadable cells rather than guess at them (NaN samples,
`SoftSymbol::erased`, the photometric residual check of F8), and all of that information was
being discarded at the final step.

### Why erasures are worth double

An error is wrong at an *unknown* position: the code must spend budget finding both where and
what. An erasure has a *known* position, so only the value must be solved. Hence
`2·errors + erasures ≤ nsym` — the same parity corrects **nsym erasures but only nsym/2
errors**. Supplying positions doubles the correction power at zero cost in parity.

### The measurement

Identical impaired channel (noise 26, shot 0.9, crosstalk 0.20, occlusion 3%, frame drop 10%),
128 KB payload, seed 5:

| `nsym` | `Rfec` | Frames | Uncorrectable | Goodput | Verified |
|--:|--:|--:|--:|--:|:--|
| **0 (off)** | 1.0000 | hit the 200,000 cap | — | — | **NO — never completes** |
| 16 | 0.9373 | 117 | 11 | 65.6 KB/s | yes |
| **32** | **0.8745** | **89** | **0** | **86.3 KB/s** | yes |
| 48 | 0.8118 | 112 | 0 | 68.6 KB/s | yes |
| 64 | 0.7490 | 241 | 0 | 31.9 KB/s | yes |

**Without intra-frame FEC the transfer never completes on this channel.** With it, 89 frames.

The curve also shows the code-rate trade-off in the raw: too little parity (16) leaves 11
uncorrectable frames for the fountain to re-send; too much (64) wastes over a quarter of every
frame on parity and needs 2.7× the frames. The optimum is somewhere near 32 **for this channel**
— and since the optimum is channel-dependent, this is precisely the knob the adaptive link
controller must select at runtime rather than a constant to bake in.

### Interleaving is not optional

Optical damage is spatially clustered — a glare spot, a fingerprint, a rolling-shutter
transition band. Without interleaving a burst lands inside one codeword and exceeds its budget
while its neighbours sit idle. Payload bytes are therefore scattered across all 10 codewords,
so a 300-byte contiguous burst becomes ~30 isolated erasures in each — comfortably inside a
32-byte budget. Tested directly (`SurvivesASpatialBurstThatWouldKillOneCodeword`), and the
scatter property is asserted on its own so a broken interleave reports as a broken interleave
rather than a mysterious FEC regression.

### A bug worth recording

The first erasure implementation passed the headline test (16 erasures at nsym=16) and failed
the pure-erasure cases below it. The cause: Berlekamp-Massey was consuming the Forney syndromes
from index 0, but the key equation `Λ_err·T ≡ Ω (mod x^nsym)` has `deg Ω < f+e`, so the first
`f` coefficients belong to Ω and carry no information about the unknown errors — feeding them
in invents a spurious error locator. The symptom was diagnostic: it worked **only** at
`f == nsym`, where the BM loop does not execute and the bug cannot express itself.

### Known limitation, recorded not hidden

Reed-Solomon works on bytes, so one unreadable cell condemns its whole byte — eight cells'
worth of budget spent on one. That promotion is lossy and is the price of a byte-oriented code.
A bit-level or non-binary code would not pay it (OQ-025, EXP-011).

---

## F19 — The soft information was saturated, and M0 barely needs it anyway `[HYP]`

**Found:** 2026-08-02, testing whether erasure-marking from LLR magnitude buys anything.

The project charter states *"soft symbol confidence should be preserved for forward-error
correction"*, and the whole `SoftSymbol`/LLR path exists to honour it. That hypothesis had
never been measured. It has two halves and both now have answers, neither of them the expected
one.

### Half one: the LLR was saturated — a real bug

Measured `|llr|` distribution over 37,392 cells, swept across noise amplitudes 0 → 120:

| Noise amplitude | Share in the top band (112–127) |
|--:|--:|
| 0 | **1.000** |
| 30 | **1.000** |
| 60 | **1.000** |
| 90 | **1.000** |
| 120 | **1.000** |

**Every cell, at every noise level, in one band.** The magnitude carried no information
whatsoever — the "soft" path was a one-bit signal with an erasure flag wearing a costume.

Cause: `QuantiseLlr` used a fixed scale of 32, mapping only `d ∈ [0,4]` sigma onto the whole
int8 range. With 3×3 subsampling the per-cell SNR is far higher than that, so everything
clamped. Saturation at high SNR is not itself wrong — a certain cell *is* certain — but
collapsing every distinguishable confidence into one value discards exactly the gradation the
FEC layer is meant to exploit.

**Fixed** by scaling against the *measured* clean-cell distance, so `|llr|` means "fraction of
the way to an unambiguous decision" relative to this frame's own separation and noise. The
distribution now responds to the channel (top band 0.929 → 0.866 across the sweep) where
before it was pinned at 1.000.

### Half two: M0 decisions are essentially never marginal

Even with the quantisation fixed, the **symbol error rate is 0.00000 at every noise level
tested, up to amplitude 120** — nearly half the full 0–255 range.

The reason is structural: the sampler averages 3×3 subsamples, each a bilinear interpolation
over ~4 pixels, so roughly 36 pixels back every cell and noise falls by about 6×. Binary
luminance then sits ~6σ from the threshold. There is no marginal decision for soft information
to disambiguate.

**Erasure-marking sweeps confirm it.** Converting low-confidence cells to erasures (which cost
half what errors cost) never helped:

| `--erase-below` | 0 | 4 | 8 | 16 | 24 | 32 |
|---|--:|--:|--:|--:|--:|--:|
| Goodput (KB/s) | **13.8** | 13.3 | 10.8 | 2.7 | 2.7 | 2.7 |

Zero is optimal. On the image path the threshold had *no effect at all* at any setting.

### What this means

**For M0, soft-decision FEC has little to offer.** Frame failures are **structural** —
occlusion, tracking loss, photometric failure, cells outside the frame — not decisional. Those
already produce erasures through the NaN path, which is where the value has been all along
(F18: intra-frame FEC with erasure positions is the difference between completing and not).

This tempers a stated architectural assumption rather than refuting it. The soft path should
matter considerably more for **M2 four-level luminance**, where adjacent levels sit at one
third the spacing and effective noise is ~3× worse — which is precisely when the fixed-scale
saturation would have hidden the benefit. Fixing the quantisation now means M2 can be evaluated
on its merits instead of against a broken confidence signal.

⚠ Simulator result, uncalibrated (RISK-024). The renderer models neither cell-to-cell crosstalk
beyond a box blur, nor fixed-pattern sensor noise, nor display non-uniformity at cell scale —
all of which would push real decisions closer to the threshold. The *mechanism* (36-pixel
averaging cuts noise ~6×) is real and will hold on hardware; the resulting error rate is
optimistic.

---

## F20 — The whole code-rate ladder can be scored from one run. The answer cannot be delivered `[HYP]`

**Found and built:** 2026-08-03, implementing the adaptive link controller (component C14,
features ADP-01 and ADP-02). Registered as EXP-023 before running.

F18 established that the optimal `nsym` is channel-dependent and closed with "this is precisely
the knob the adaptive link controller must select at runtime". Building that controller produced
one good surprise and one hard structural limit.

### The good surprise: the counterfactual is free

`IntraFec` derives its codeword count from the **frame capacity alone**
(`codewords = frame_capacity / codeword_bytes`); `nsym` shrinks only the message. So the
interleave mapping, the coded frame length and therefore the per-codeword damage pattern are
**all invariant in `nsym`**. A codeword decodes at `nsym = n` exactly when the budget it needs,
`2·errors + erasures`, is `≤ n` — and neither term depends on `n`.

Therefore **the per-frame worst codeword budget is a sufficient statistic for the entire
ladder.** One 256-bin histogram, one increment per frame, no allocation, scores every candidate
rung counterfactually from a run made at a single rung. The receiver never has to try a rung to
learn what it would have done, and C14 needs **no new per-frame measurement** — only a
subtraction over numbers the decoder already produces (F23).

**Measured (EXP-023, four channels, ladder `nsym ∈ {8,16,24,32,40,48,64}`):** on all three
channels that complete a verified transfer, the controller — seeing only receiver telemetry from
a single run at `nsym = 32` — named the brute-force goodput optimum exactly. **Rung distance 0,
three for three.**

| Channel | Brute-force optimum | Controller's pick | Rung distance | Cost of the worst rung |
|---|---|---|--:|--:|
| clean | 8 @ 120.0 KB/s | **8** | 0 | 50.0% |
| mild | 8 @ 112.9 KB/s | **8** | 0 | 66.8% |
| F18-impaired | 24 @ 86.3 KB/s | **24** | 0 | 93.2% |
| severe | none — no rung verifies | 40 | — | not scoreable |

⚠ Simulator, uncalibrated (RISK-024). This measures whether the **decision rule** is sound. It
says nothing about which `nsym` real hardware will want.

### The hard limit: there is nowhere to send the answer

**`nsym` cannot be changed mid-session against this protocol.** `nsym` sets the FEC message
size, which sets the fountain symbol size, which `FileManifest` fixes for the whole transfer —
and it must, because an XOR-based erasure code cannot combine symbols of unequal length
(`FountainDecoder::out_` is `k · symbol_size`). Every other capacity-changing knob — modulation
profile, grid — has exactly the same consequence.

So **C14 as the component registry specifies it — a mid-session control loop over profile and
code rate — cannot be implemented against the current protocol.** What can be implemented, and
now is, is the decision function: an estimator and a policy that name the right rung. Code-rate
selection is a **session-start** decision here, and on a one-way link the transmitter cannot
hear the receiver's answer at all (OQ-013).

This is recorded rather than worked around. The controller reports its recommendation and
`ffsim --adapt` prints an explicit note that the figure is **not actuated**, because a
"+7% headroom" line with no delivery path is exactly the kind of number that becomes a claim.

### A second, duller reason the control loop would not have mattered much

Even with actuation, **most of these transfers are shorter than the controller's own dwell.**
The EXP-023 runs complete in 64–241 display states; the promote dwell is 96 frames on top of a
32-frame observation minimum. So the hysteresis walk never moves at all on the clean and
impaired channels — correctly, since a 64-frame session offers no time to converge.

Two consequences. First, for transfers of this length **only the session-start choice matters**,
which is a second independent argument for treating rate selection as a negotiation problem
rather than a control problem. Second, the dwell constants were chosen for a *sustained* link
and are the wrong order of magnitude for a 128 KB transfer; they should be re-derived once a
realistic transfer duration is known, and until then a controller tuned for a long session
should not be assumed to do anything on a short one. `ffsim --adapt` now says so explicitly when
the walk does not move, so the recommendation and the walk cannot be misread as contradicting
each other.

### What the cost of guessing tells us about OQ-013

OQ-013 has been "the least-designed part of the architecture" without a number attached. Now it
has one: a fixed `nsym` chosen for the wrong channel costs **50%, 67% and 93%** of the available
goodput on the three channels above. H4 was registered at ">30%" and is comfortably confirmed.

That is a large enough penalty to say plainly: **the value of ever closing the loop is high**,
and reverse optical control (ADP-04, currently Phase 9) is worth more than its position
suggests. It is not yet an argument for moving it — these are uncalibrated simulator channels,
and a real link may sit in a broad plateau where any middle rung is fine. It *is* an argument
for measuring the spread on real captures early.

### The cheaper fix that comes first

Decoupling the fountain symbol size from the frame capacity — a **fixed, small symbol size with
an integer number of whole symbols per frame** — would make code rate adaptable within a session
without touching the fountain code's mathematics. It would also address the **~27% block padding
waste** F4 recorded, which has the same root cause: symbol size chosen to fill the frame. Raised
as OQ-037; it is a protocol change and belongs in an ADR if it is taken.

---

## F21 — The FEC telemetry was truncated exactly on the frames that mattered `[FACT]`

**Found:** 2026-08-03, building C14's consumer of `IntraFec::Stats`.

`worst_erasures_in_codeword` was accumulated **inside** the decode loop, after each codeword's
`ReedSolomon::Decode` call. So an early return on the first uncorrectable codeword left the
statistic reflecting only the codewords already processed — and **never the failing one**.

The bias is precisely backwards. A frame that failed is the only evidence that a stronger code
was needed, and it reported whatever load the earlier, healthier codewords happened to carry. A
frame with 4 erasures in codeword 0 and 60 in codeword 7 reported **4**.

**Fix:** compute the worst load across all codewords *before* attempting any decode. The loads
are all known at that point, so completeness costs nothing. Regression test asserts the failing
codeword's own load survives the early return (`StatsReportTheFailingCodewordsOwnLoad`).

**Worth remembering:** this is telemetry that was correct on the success path and absent on the
failure path, which is the one that matters. It survived F18's own measurements — the
`budget_used` accessor was written *for* the adaptive controller and documented as reporting
headroom — and was only exposed by building the consumer. **A field's first real reader is its
first real test.**

---

## F22 — The hysteresis margin and the ladder spacing are not independent `[FACT]`

**Found:** 2026-08-03, when C14 refused to leave `nsym = 24` on a channel with no errors at all.

"Fall back fast, promote slowly" (MODULATION-SPEC) asks for an asymmetric margin, and 10% for
the promote direction looks like an entirely reasonable hysteresis figure. It is not, and the
reason is arithmetic: adjacent rungs of the default ladder differ by only **3.35%** of frame
capacity at the dense end (`247/239 − 1`). A 10% margin therefore exceeds the gain any single
promotion can offer.

The resulting behaviour: on a **perfectly clean channel** the controller promoted from 32 to 24,
found every further step worth less than its own margin, and **parked there permanently** —
leaving ~6% of every frame unused for the rest of the session.

**This is a worse failure mode than oscillation, because nothing about it looks wrong.** No
error, no thrash, no warning; just a session running at less capacity than it had. The
registry's C14 entry names oscillation and optimising-the-wrong-metric as the failure modes to
watch. This is a third one, and it is the quiet one.

**Fix, in two parts.** The default `promote_margin` drops to 0.02, below the smallest single-rung
gain. And `LinkController::Create` now **rejects** a margin no promotion on its ladder could
clear (`kDegenerateParameters`), rather than leaving the trap set for whoever next tunes either
field. `SmallestPromotionGain()` is public so the constraint is inspectable instead of
folkloric. Test: `RejectsAPromoteMarginThatWouldStrandTheLadder`.

**Generalisable:** a hysteresis band is meaningless in isolation — it is only ever large or small
*relative to the step it gates*. Any tunable pair like this should be validated jointly at
construction, because the failure is silent and the two values are usually chosen by different
people at different times.

---

## F23 — Erasures are not what the code spends. The budget is `[HYP]`

**Found:** 2026-08-03, when C14's first version confidently recommended a rung that measurement
showed to be 24% worse.

The first estimator binned the worst per-codeword **erasure load**, reasoning that erasures are
what the receiver knows about — the same reasoning that makes F18's erasure decoding valuable.
On F18's impaired channel it reported the erasure load as **9 at every rung** (F20's invariance,
confirmed end to end), predicted a **1.0000** frame success rate at `nsym = 16`, and recommended
it.

**The real decoder failed 11 of 117 frames at `nsym = 16`**, and its measured goodput there is
**65.6 KB/s against 86.3 KB/s at the optimum — a 24% loss, chosen deliberately by a controller
that was certain.**

### Why

The exact per-codeword predicate is `2·errors + erasures ≤ nsym`. **Undetected errors cost two
parity bytes each**, and the erasure count omits them entirely. On this channel the worst
erasure load was 9 while the worst *budget* was **30** (mean 14, p95 19) — the missing term
dominated.

### The receiver knew more than it thought

Errors look unobservable by definition: if the receiver knew where they were, they would be
erasures. But they are recoverable **after** the fact. `ReedSolomon::Decode` returns the number
of symbols it changed, and a verified correction accounts for exactly the declared erasures plus
the errors its locator found — so `errors = corrected − erasures`. No new instrumentation, no
extra pass, no change to the decoder.

With `worst_budget_used` as the statistic, the controller recommends **24** on that channel: the
brute-force optimum, rung distance 0 (F20's table).

### The limitation, stated rather than buried

**A frame that does not decode cannot have its budget measured** — decoding is how the budget
gets measured. Such frames yield only the bound "more than the rung it was tried at", are
recorded at that floor, and are **counted** (`censored_observations`). The consequence is
directional and worth memorising: **an estimate taken at a rung that loses frames is trustworthy
for weakening the code and optimistic for strengthening it.**

The severe channel of EXP-023 shows this working as designed and failing as documented:
**28,497 of 40,000 observations censored**, every rung crediting the censored frames with
success, and the controller's answer correspondingly unvalidatable — which `ffsim --adapt` says
out loud rather than printing a clean-looking recommendation.

### A note on F19's scope

F19 measured a symbol error rate of **0.00000** up to noise amplitude 120 and concluded that M0
decisions are essentially never marginal. That measurement stands, but it was taken on the
**image path** with additive pixel noise. This channel is the **cell-sample path with crosstalk
0.20**, and it produces undetected symbol errors in quantity — roughly 10 per codeword in the
worst frame. F19's conclusion is narrower than its wording suggests: *pixel noise* does not
produce marginal M0 decisions; *inter-cell crosstalk* does. No retraction is needed, but anyone
citing F19 for "M0 has no errors" should read this alongside it.

---

## F24 — A CI configuration that has never run is a hypothesis, not a safety net `[FACT]`

**Found:** 2026-08-03, pushing the repository to GitHub for the first time. Seven CI jobs had
been written, committed and cited in ADR-0013 as validation. **None had ever executed.** Their
first run failed four jobs and exposed **four independent defects**, one of them a genuine bug on
the attacker-facing surface.

That is the finding. The individual defects are below, but the lesson is the aggregate: an
unexecuted CI file is a *claim* about the code, and this project had been treating it as
*evidence* for a day. ADR-0013's "validated by Phase 1 scaffolding" was true only of the macOS
developer machine.

### 24a — `fuzz_screen_detect` crashed after 5,022 executions

The fuzz targets' first real run found a **contract violation in the homography solver**.
`Homography::FromCorrespondences` normalised `m[8]` to 1 and checked finiteness — neither of
which implies a non-zero determinant — so it could return a **singular matrix that `Inverse()`
then rejected**. `ScreenDetector` handed back a `Detection` whose homography could not be
inverted, and the tracker and every other consumer of the inverse inherited a broken contract.

Fixed at the contract level: a projective transform is invertible by definition, so
`FromCorrespondences` now checks the determinant with the same tolerance `Inverse()` uses, and
the two cannot disagree. Behaviour on valid input is unchanged — zero detection failures, same
worst geometric error, everything still verifies.

**It reproduces only on Linux.** The crashing input is a 3×2-pixel image against a 24×40 grid;
whether its determinant lands inside the tolerance depends on floating-point rounding. Chasing
the arithmetic would have been the wrong fix.

### 24b — GCC rejected the build where clang does not

`-Wmissing-field-initializers` (enabled by `-Wextra`) fires under GCC on **every C++20 designated
initializer that omits a member** — `{.pilot_pitch = 8, .marker_size = 4}` and friends, used
throughout. Omitting a member is the point of the feature, and in C++ it cannot indicate an
uninitialised read: aggregate initialisation value-initialises anything not named. Suppressed
with that rationale rather than dropping `-Werror` or naming every field at every call site. One
genuine `-Wdangling-else` (an unbraced `if (cond) EXPECT_EQ(...)`, where the macro expands to an
`if/else`) fixed with braces. Two integer-promotion narrowings in `core/` made explicit.

### 24c — The determinism gate could never have passed

ENGINEERING-PRACTICES calls "same seed ⇒ byte-identical output" the foundation of simulator and
replay regression testing. The gate compared `ffsim`'s **full stdout**, which includes decoder
throughput and real-time headroom — wall-clock measurements of the host. Two runs cannot produce
identical text. **The gate was arithmetically impossible to satisfy from the day it was written.**

`ffsim --no-host-metrics` now suppresses exactly the three host-dependent lines, so the tool
declares which of its output is reproducible instead of leaving a filter in YAML to drift. The
gate additionally now checks the **image path**, which renders pixels and exercises far more
floating point than the cell-sample path — where the hazard actually lives. Both are
deterministic, which is the first time that has been demonstrated rather than assumed.

### 24d — Local fuzzing and local GCC are both unavailable

A corollary worth stating because it shapes how this project must work. F5 recorded that Apple
clang ships no libFuzzer. To that add: **no GCC is available on this machine either**, so the
`-Wconversion` class of defect is *also* Linux-CI-only. Two of the four defects above were
undetectable locally by construction.

**Consequence:** every fuzzer crash now gets a permanent deterministic replay in
`core/tests/test_fuzz_regressions.cpp`, including the property behind it rather than only the one
input that exposed it. A macOS developer cannot run the fuzzer, but they can run the regression.

---

## F25 — `SENSOR_ROLLING_SHUTTER_SKEW` is not a camera characteristic `[FACT]`

**Found:** 2026-08-03, writing the capability probe's Kotlin layer against the real SDK.

`SENSOR_ROLLING_SHUTTER_SKEW` is a **`CaptureResult`** key, not a `CameraCharacteristics` key.
Verified directly against `android.jar` (API 35): `CameraCharacteristics` has no such member;
`CaptureResult` has `Key<Long> SENSOR_ROLLING_SHUTTER_SKEW`.

**Why it matters.** `docs/research/android-camera-pipeline.md` and OQ-017 both read as though it
could be read at startup, and C02's registry entry lists rolling-shutter skew among the things
the probe outputs. **A startup probe cannot obtain it at all** — it arrives per-frame from an
active capture session. So:

- The probe leaves it at its negative sentinel rather than defaulting to a plausible number.
- Skew must reach `DeviceReport` from the **recorder**, not the probe, which changes where
  OQ-017 gets answered: not by capability enumeration, but by a capture run.
- C02's specified output list was wrong on this point, and is corrected.

**The generalisable part.** This is the ADR-0014 payoff running in the *opposite* direction to
the one intended. That ADR argued for putting judgement in portable C++ because Kotlin cannot be
tested off-device. But writing the thin Kotlin layer against the real SDK is itself a test — of
the documentation — and it caught a platform misunderstanding the research notes had recorded as
settled. **Every platform claim in this repository is documentation-derived (`[FACT]`-from-docs)
and one of the first to meet a compiler was wrong.** That is a specific, measured reason to
distrust the rest until they compile or run, and it argues for getting the remaining platform
claims in front of a toolchain early rather than treating the research phase as finished.

---

## F26 — The verdict contradicted its own claims block `[FACT]`

**Found:** 2026-08-04, reading the first real hardware report — by reading it, not by testing it.

The probe printed, on one screen:

```
  realtime clock   true                            <- CLAIMED BY THE PLATFORM
  - no trustworthy REALTIME timestamp source ...   <- NATIVE VERDICT
```

**The logic was right and the wording was wrong.** `clock_cross_check_available` requires the
timestamp evidence to be trustworthy *and* the source to be REALTIME. The device claims REALTIME
but the claim is unverified, so refusing the cross-check is exactly correct (RISK-011). The note
just described that refusal as though the device had said the opposite.

In a project whose entire discipline is separating a claim from evidence for it, a verdict that
appears to deny a claim the device actually made is worse than a cosmetic bug: it teaches the
reader to distrust the report, and the report is the whole product of C02.

**Fix.** Split the note three ways, matching the pattern the `MANUAL_SENSOR` check twelve lines
above it already used: *refuted*, *not claimed at all*, and *claimed but unverified*. These call
for different work — a device that does not claim REALTIME never will, while a device that claims
it needs EXP-008 — so collapsing them also destroyed information. Test:
`AClaimedButUnverifiedClockIsNotDescribedAsMissing` asserts the refusal stands *and* that the note
does not deny the claim.

**The generalisable part.** The inconsistency sat **inside a single function**, twelve lines
apart, where one check handled its three cases and the next handled one. Neither was wrong in
isolation and no test compared them. It took printing both next to each other on a phone to see
it — the first time those two blocks had ever been rendered together. **Output formatting is not
cosmetic when the output is the deliverable**, and reading a report as its audience is a different
activity from testing it as a function.

---

## F27 — First real hardware data: what the S26 Ultra actually claims `[FACT]`

**Collected:** 2026-08-04, C02 probe on **samsung SM-S948U1** (SoC `SM8850`, Android 16 / API 36),
build `BP4A.251205.006`. Raw:
`data/experiments/EXP-007/raw/probe-SM-S948U1-20260804.txt`.

**This is the project's first measurement on a physical device.** Every platform claim in this
repository was previously documentation- or press-derived. These are read from the device's own
APIs.

A scoping note before the numbers: **enumeration is not verification.** What follows is what this
device *advertises to a third-party app*. Whether the advertised modes deliver frames, deliver
*distinct* frames, or honour manual settings is EXP-007 and EXP-006, and none of it is answered
here. The probe correctly returned **UNSUPPORTED** for that reason.

### DEVICE-MATRIX claims that held

| Claim (press/vendor) | Device reports | Verdict |
|---|---|---|
| Native resolution 1440 × 3120 | `1440x3120` | **confirmed** |
| 120 Hz capable | 120.000 Hz | **confirmed** |
| Snapdragon 8 Elite Gen 5 | `SM8850` | consistent — model number, not marketing name |
| Charter grid 144 × 240 integer at 10 × 13 px | in the integer-pitch list, 34,560 cells | **confirmed** |

The grid arithmetic deserves emphasis because it was load-bearing and had never met hardware: the
probe independently enumerated **24 integer-pitch grids** for this panel, and `144x240` — the
charter grid DEVICE-MATRIX assigns to this device — is among them, as is the square-cell
`120x260` (12 × 12 px) alternative. Both were derived by `tools/grid_fit.py` from a *press*
resolution; both survive contact with the real panel.

**QHD+ is available at 120 Hz.** All three advertised resolutions (1440×3120, 1080×2340,
720×1560) offer both 120 Hz and 60 Hz. Some Samsung generations restrict the top resolution to
60 Hz; this one does not, so densest panel geometry and highest refresh are not in conflict.
There is **no 144 Hz mode** — 120 and 60 only.

### The camera ceiling, and what it costs

| | Advertised |
|---|---|
| `INFO_SUPPORTED_HARDWARE_LEVEL` | **LEVEL_3** — the top tier |
| `MANUAL_SENSOR` | claimed |
| `SENSOR_INFO_TIMESTAMP_SOURCE` | **REALTIME** claimed |
| `SENSOR_ROLLING_SHUTTER_SKEW` | **absent** — independently confirms F25 |
| Fastest CPU-readable (`YUV_420_888`) | **60 fps** |
| High-speed modes | **2**, both **240 fps**: 1280×720 and 1920×1080 |

Two consequences follow directly, and they are the useful part.

**1. The CPU path caps `Fd` at 60 on this device.** The fastest CPU-readable mode is 60 fps, so
milestone 6 (≥120) is unreachable on the CPU path whatever the display does. That is ADR-0005's
dual-path split vindicated by the first device we looked at, and it is the receiver bounding
`Fd` — exactly as PERFORMANCE-PHILOSOPHY insists.

**2. The high-speed path caps capture at 1080p, which caps grid density.** `[HYP]` The only
≥120 fps modes are 720p and 1080p. Taking the 144×240 charter grid on this portrait 1440×3120
panel, captured at 1080×1920 with the screen filling ~80% of frame height:

- screen height ≈ 0.80 × 1920 ≈ 1536 px → **6.4 px/cell** down the columns
- screen width ≈ 1536 × (1440/3120) ≈ 709 px → **4.9 px/cell** across the rows

So the high-rate path lands at roughly **5 px/cell** — the very densest point the simulator grid
sweep reached (F16), and one the simulator is *known to be incapable of judging* because it models
neither sensor MTF nor moiré (F14 retraction, F16). Tagged `[HYP]`: the 80% fill fraction is an
assumption, not a measurement.

**This is a real tension, now concrete.** The panel supports grids up to 180×390, but on the
high-rate path the **receiver's 1080p ceiling, not the panel, bounds density**. Chasing display
density beyond what a 1080p capture can resolve buys nothing at 240 fps. It is precisely the
`N ↑ → Pc ↓` coupling PERFORMANCE-PHILOSOPHY lists, with numbers attached for the first time.

### What this does to OQ-035

OQ-035 asked whether Samsung restricts ≥60 fps capture for third-party apps, noting that if so
"the Pixel 8 is the only viable high-frame-rate receiver and milestone 6 rests entirely on it."

**The advertisement contradicts the pessimistic reading.** This third-party app is offered two
240 fps constrained high-speed modes and several 60 fps CPU-readable modes. The forum reports
behind OQ-035 concerned ≥60 fps *recording*, a different API surface, and may simply be stale.

**OQ-035 is not closed.** Being offered a mode is not being given frames, and the specific risk
the research notes flag — a high-speed session returning *duplicated* rather than distinct frames
— is invisible to enumeration by construction. Narrowed from "we may not even be offered the
modes" to "we are offered them; do they deliver?", which is EXP-007's verification half.

---

## F28 — The recorder was the bottleneck, not the camera, and only an A/B could tell `[FACT]`

**Found:** 2026-08-04, first run of the C05 CPU-path recorder on an SM-S948U1. Raw:
`data/experiments/EXP-007/raw/capture-cpu-path-SM-S948U1-20260804.md`.

Requested 60 fps at 1920×1440. Got **32.11 fps**. The obvious conclusion — that the device cannot
sustain 60 fps and F27's advertised figure is another vendor overstatement — would have been
**wrong**, and it would have been the third time this project measured its own bug and drew a
conclusion about the world from it (F14, F15).

### Two of my own defects came first

**The rate control was never set.** `CONTROL_AE_TARGET_FPS_RANGE` is an *auto-exposure* control.
With `CONTROL_AE_MODE_OFF` — which the recorder sets deliberately, so consecutive frames stay
photometrically comparable — the AE routine is not running and nothing consumes the range. Under
manual sensor control the period is set by **`SENSOR_FRAME_DURATION`**, which was absent. It does
not fail loudly; it silently produces the wrong rate, which looks exactly like a device that
cannot hit the rate. After setting it, `CaptureResult` reports 16,658,337 ns — a 60.03 fps ceiling
— so the camera was configured correctly and *still* delivered 32.

**The test harness reported a stale run.** The first re-run printed a byte-identical report,
timestamp included, because the script waited for the report file to *exist* and it already did
from the previous run. Fixed by deleting the report first and refusing to launch if the delete
fails. A measurement harness that can silently report the previous measurement is worse than no
harness.

### The A/B that settled it

Identical camera configuration; the only variable is whether frames are written to disk.

| Arm | Size | Y plane | Writes | Delivered | Worst gap |
|---|---|--:|:--|--:|--:|
| A | 1920×1440 | 2.76 MB | **ON** | **32.11 fps** | 5.00 periods |
| B | 1920×1440 | 2.76 MB | **OFF** | **59.04 fps** | 2.00 periods |
| C | 1280×720 | 0.92 MB | **ON** | **56.80 fps** | 2.00 periods |

**The camera delivers 59 of a requested 60 fps at 1920×1440.** Our own writer costs 46% of the
frames. At 2.76 MB per frame, 60 fps is ~166 MB/s to app-private storage; when that saturates,
unreturned `ImageReader` buffers throttle the camera, and the symptom is indistinguishable from a
slow sensor. Drop the frame to 0.92 MB and 95% of frames survive.

### The cadence, which names the mechanism precisely

An average rate cannot separate a slow sensor from a fast sensor whose frames are dropped. The
**modal** inter-frame interval can, because dropped frames add whole multiples of the true period
and cannot shift the most common value. For arm C (`tools/frame_cadence.py`):

- modal interval **16.66 ms → 60.02 fps** — the sensor is running at exactly the requested rate
- **17 gap events, every one exactly 2× modal**, never larger
- **17 frames dropped, 5.4% of expected**

Single misses of an otherwise metronomic cadence point at the buffer queue momentarily running dry,
not at a stall. Six `ImageReader` buffers may simply be too few; that is a testable next step
rather than a conclusion.

### What this bounds, and what it does not

**It bounds the capture harness, not the link.** The live receiver *decodes* frames; it does not
write them. So this constrains what can be *recorded* for offline analysis and says nothing about
achievable goodput. Conflating the two would be exactly the error PERFORMANCE-PHILOSOPHY's
six-rate discipline exists to prevent.

For OQ-023 — is there a lossless capture path for the harness? — the answer is **yes, bounded by
write throughput**. Roughly 50 MB/s sustained works on this device's app-private storage
(0.92 MB × 56.8 fps ≈ 52 MB/s). Consequences worth planning around:

- **1280×720 at 60 fps records with ~5% loss.** Usable.
- **1920×1080 at 60 fps is 124 MB/s** and will drop heavily. Untested, but the trend is clear.
- **The 240 fps arm cannot be recorded frame-for-frame at all** by this route. At 720p that is
  221 MB/s. Recording the high-rate path needs either a RAM ring buffer flushed afterwards (720p
  ×240 fps × 1 s ≈ 221 MB, so seconds not minutes), faster storage, or accepting a sampled
  recording. **Lossy compression remains excluded** — it destroys exactly the high-spatial-frequency
  cell structure the recordings exist to measure (C17).

**The generalisable lesson.** The instinct on seeing 32 fps was to distrust the vendor, and the
project's own risk register (RISK-011) supplies a ready-made story for why the device would be
lying. That made the wrong conclusion *more* attractive, not less. The A/B cost one extra run.

---

## F29 — The first real capture reached the harness, and the harness refused it `[FACT]`

**2026-08-04.** 300 frames captured on a physical phone, pulled to the desktop, and fed to
`ffreplay` — the production replay path, on real camera data, for the first time.

`ffreplay` parsed the bundle, found all 300 frames, and then **refused to decode**:

```
grid                     0x0
capture                  1280x720 @ 60.0 fps, 300 frames
⚠ INCOMPLETE METADATA — not usable as experimental evidence.
  missing: sender_model grid_cols/grid_rows distance_cm source_payload_sha256
layout: value_out_of_range
```

**That is the correct answer and the point of the exercise.** There was no transmitter, so no grid
was recorded, and `FrameLayout::Create(0, 0)` rejects it. C17 was built early specifically so the
first real capture would land on a path already known to be sound (F17), and its
incomplete-metadata guard fired on the first real capture rather than quietly producing a number
from a half-labelled dataset.

**What this establishes:** the bundle format round-trips from a real Android device — written by
`harness::CaptureWriter` through JNI, pulled, parsed, and enumerated by the same reader the
simulator uses. The path from phone to analysis tool exists and works.

**What it does not establish:** nothing has been decoded. These are pictures of a desk. Decoding
real optical frames needs a transmitter presenting known states (C03/C04), and that is the next
piece of work. `Fd` remains unmeasured (EXP-006), and no goodput figure of any kind has been
measured on hardware.

---

## F30 — The registries said almost nothing was built `[FACT]`

**Found:** 2026-08-04, auditing for skipped steps before starting the transmitter.

`FEATURE-REGISTRY.md` marked **~30 entries `Planned`** that had been implemented, tested, and
written up in findings F2–F19. A fresh agent reading it would have concluded the project had a
simulator and little else, and might reasonably have rebuilt something that already worked.

The list is not marginal: `MOD-01` (M0 modulation), `FEC-01`–`FEC-03` and `FEC-05` (intra-frame
FEC, interleaver, erasure signalling), `FTN-01`–`FTN-04` (the whole fountain layer), `PRO-01`
(header codec), `FIL-01`/`FIL-03`/`FIL-04` (manifest, hash gate, filename sanitisation),
`SEC-01`–`SEC-03`/`SEC-05` (bounds, overflow discipline, fuzzing), `CV-01`–`CV-04` (detection,
tracking, sampling, photometric field) and `SIM-01`/`SIM-04`/`SIM-05` (simulator, replay harness,
`CaptureSource`).

### Why it happened, and why it will happen again

CONTRIBUTING says to update the registry whenever a document or subsystem changes. That rule is
followed for *documents* — the findings file, ADRs and open questions all stayed current — and
skipped for *code*, because the registry is not where the work happens and nothing fails when it
goes stale. There is no test for it. RISK-020 tracks exactly this, and F10 was caught by it, and it
happened anyway across thirty entries.

**The recurrence rate is the real finding.** In this single day:

- The "no geometric pipeline / no payload FEC" claims in this file's own *not validated* list were
  corrected (2026-08-03) — both had been false for a day.
- The replacement text, *"No device code at all… the single most important gap"*, went false hours
  later when the probe ran (F27).
- Its replacement, saying delivery and distinctness were still unmeasured, went false **the same
  day** (F28).

So the same section was wrong three times in about thirty hours, each time in the direction of
understating progress. A periodic review is not optional bookkeeping; it is the only mechanism that
catches this, and `DOC-05` is re-scoped from a one-off task to a recurring one.

### The correction, and what it deliberately did not do

Every relabelled entry was checked against a source file plus its tests. Two distinctions were
introduced because the single `Planned`/`Done` axis could not express the truth:

- **Implemented** (code exists and is tested) is now kept separate from **acceptance met** (the
  experiment named on the entry's own acceptance line has run). Most acceptance criteria here need
  hardware or an unrun experiment, so implemented rarely implies accepted — `CV-03` has a working
  sampler and no swept interior margin; `MOD-06` has a working LLR path whose calibration test
  *cannot* pass at M0 because there are no errors to calibrate against.
- Entries that are **not built as specified** now say so rather than staying vaguely `Planned`:
  `FIL-02` reassembles in memory with no temp file and no streaming, which is fine at test sizes and
  not fine against the 4 GB bound the manifest already permits; `SIM-02`'s config files do not
  exist; `BEN-01` exists in the simulator only, with no C15 telemetry system behind it;
  `FEC-04` soft-input decoding is **deliberately** absent, because F18/F19 showed the value was in
  erasure *positions* rather than in LLR magnitudes.

**Under-claiming is not the safe direction.** It reads as modesty, and it is the failure mode that
makes someone rebuild a working subsystem or distrust a finding that was sound.

---

## F31 — An app does not get the panel's native resolution, and the charter grid dies without it `[FACT]`

**Found:** 2026-08-04, first run of the Android transmitter (C03/C04) on an SM-S948U1.

The transmitter requested the **1440×3120 @ 120 Hz** mode through `preferredDisplayModeId`. GL
received **1080×2340**. The request was ignored silently — Samsung gates panel resolution behind a
system *display resolution* setting, and an app cannot override it.

### What that does to the grid

The integer-pitch requirement is not a preference: a fractional pitch puts cell boundaries on
fractional pixels, which the panel cannot render crisply and which raises spatial crosstalk for no
gain (DEVICE-MATRIX). So the pitch has to be exact.

| Grid | At 1440×3120 (panel max) | At 1080×2340 (what an app gets) |
|---|---|---|
| **144×240** — the charter grid | **10 × 13 px, integer** | **7.5 × 9.75 px — FRACTIONAL** |
| **120×260** — the square-cell option | **12 × 12 px, integer** | **9 × 9 px, integer** |

**The charter grid for this device is unusable at the resolution the device actually hands out.**
The square-cell alternative is integer at *both*, because 1440/120 = 12 and 1080/120 = 9, while
3120/260 = 12 and 2340/260 = 9.

**This is an unplanned argument for the square-cell grid, and a strong one.** DEVICE-MATRIX already
recommended 120×260 for isotropic crosstalk and a simpler sampler. It turns out to also be the grid
that *survives a resolution change* — and a grid whose validity depends on a user-controlled display
setting is fragile in a way no amount of receiver work can fix.

**The deeper error is in how the grid was chosen.** DEVICE-MATRIX's table and the C02 probe's
integer-pitch list are both computed from the panel's **advertised maximum**. That is correct as a
*capability* statement and wrong as a basis for choosing a grid. The grid must be derived from the
**surface the app is actually given**, which is knowable only at runtime, after the surface exists.
The renderer now logs loudly and the report refuses the run when the pitch is fractional.

### Measured, on a verified-exact 120×260 grid

| | |
|---|--:|
| States submitted | **120.21 /s** over 19.99 s |
| Median submit interval | **8.333 ms → 120.01 /s cadence** |
| Intervals > 1.5× median | **0 of 2402** |
| Render errors | **0** |
| Cell pitch | 9 × 9 px, exact |
| Payload cells | 29,057 of 31,200 (`O` = 0.0687) |

**This is an upper bound on `Fd`, not `Fd`.** A transmitter cannot confirm presentation — which is
exactly why every frame header carries its own sequence number (`frame.h`) — so `Fd` is *distinct
presented states per second* as measured by the receiver. A state submitted and then dropped by the
compositor has been submitted, not presented.

Worth noting separately: the **entire encode chain runs at 120 Hz on-device with zero errors** —
fountain symbol, interleaved Reed–Solomon, and M0 rendering of 31,200 cells, per state. The
transmitter side is not the bottleneck at 120 Hz.

### Two process failures, both instructive

**"0 render errors" meant nothing.** The first run reported a beautiful 120.15 states/s — through a
**fractional** 7.5 × 9.75 px pitch. The error counter only tracked whether `nextFrame()` returned
success; it could not see that the pixels reaching the panel were wrong. **A clean cadence number
from a misconfigured renderer is worse than no number at all**, because it invites belief. The fix
was to check the pixels: `tools/verify_tx_screenshot.py` asserts integer pitch, a bright boundary
ring, a two-level interior (a mid-grey interior is the signature of a *filtered* upscale), and pixel
run lengths that are whole multiples of the pitch — which is what actually proves `GL_NEAREST` gave a
bit-exact upscale.

**Then the verifier failed for the wrong reason.** It sampled the outermost *pixel* row for the
boundary ring and found it dark, reporting "frame missing, offset or inverted" when the frame was
fine. Rounded display corners are black, and `screencap` composites system overlays such as the
gesture pill on top of the surface. Sampling the centre of the outermost **cell** fixed it. A
verifier that fails for the wrong reason costs as much time as the defect it was built to catch, and
it erodes trust in the one tool that was working.

---

## F32 — Rotation is nearly free for the decoder and expensive for framing `[HYP]`

**Measured:** 2026-08-04, after a hardware run failed with the two phones ~36° out of alignment and
the question arose of whether the app should tolerate rotation at all.

It already does. Sweeping in-plane roll with **framing held constant** (a square 2400×2400 render, so
a rotated screen never runs out of frame), 120×260 grid, `nsym` 32:

| Roll | Header `H` | Worst geometric error | Detection failures | Verified |
|--:|--:|--:|--:|:--|
| 0° | 1.0000 | 0.134 cells | 0 | yes |
| 10° | 1.0000 | 0.154 | 0 | yes |
| 20° | 1.0000 | 0.223 | 0 | yes |
| 30° | 1.0000 | 0.130 | 0 | yes |
| 35° | 1.0000 | 0.488 | 0 | yes |
| **40°** | **1.0000** | **0.523** | **0** | **yes** |
| 45° | 0.0000 | — | 3000 | **no** |

**Roll is tolerated to at least 40° with no measurable degradation.** F9's marker redesign and F13's
centroid-scaled annulus were built for exactly this and they work.

**45° is a specific geometric degeneracy, not a general limit.** The detector finds corners by
streaming the extremes of `x+y` and `x−y` (F12's O(1) accumulator). At 45° the rectangle's own edges
align with those diagonals, so the extremes stop being unique and the four corners cannot be
separated. Worth knowing precisely, because "rotation breaks at 45°" invites the wrong fix — the cure
is a different corner criterion, not more margin.

### The first version of this sweep was wrong, in the way this project keeps being wrong

The same sweep at a **portrait** render size reported failure from **20°**, and that number is an
artifact. The axis-aligned bounding box of a rotated rectangle is much larger than the rectangle:
at 20° the box exceeded the render's width, the boundary ring left the frame, and detection failed
for lack of four lines to fit. **It measured framing while claiming to measure rotation** — and the
script it ran in carried a comment warning about exactly that confusion.

### The consequence, which is a product decision rather than a code change

Rotation costs the decoder nothing and costs **framing** a great deal: at 35° the bounding box is
**2.24× the screen's area**. So the honest advice to a user is never "stop rotating" — it is "a
tilted screen needs more room in view". Two distinctions that are easy to conflate and were:

- The box is 2.24× the screen's **area** at 35°, but only **1.08×** its **long axis**. `px/cell` is a
  linear measure, so the correction to *density* is 8%, not 124%. The area figure governs whether the
  screen fits; the linear one governs whether it resolves.
- Tolerating rotation is not the same as tolerating **perspective**. This sweep varied roll only.
  Yaw and pitch were measured separately at up to 20°/12° (F10) and are a different envelope.

⚠ Simulator, uncalibrated (RISK-024). The *mechanism* is geometric and will hold; the exact angle at
which real optics give up may be lower.

---

## F33 — Five framing failures, one useless log line `[FACT]`

**2026-08-04.** Getting two real phones into a geometry that decodes took five iterations. Every one
of them reported the identical, useless thing:

```
frames in                20
frames decoded           0
geometry failures        20
```

The five actual causes, in the order they were hit:

1. **The camera was blocked.** `CAMERA_DISABLED: cannot open camera from background`. The receiver's
   screen was locked, so the activity never reached the foreground, and Android refuses camera access
   there. Fixed with `setShowWhenLocked` / `setTurnScreenOn` on both activities — the transmitter
   needs it too, since a locked screen displays nothing.
2. **Focus at infinity, against a screen 16 cm away.** `LENS_FOCUS_DISTANCE = 0` was hardcoded as a
   sensible default. It is sensible for a distant subject and catastrophic here. Now continuous AF by
   default, with an explicit diopter override; AF converged to 6.11 diopters (16.4 cm) and the lens's
   own minimum is 10.5 cm.
3. **Grossly overexposed.** ISO 400 and a quarter-frame exposure — reasonable for a room, wrong for
   staring at a full-brightness OLED. The interior came back **66% bright against 9% dark**, where a
   two-level frame should be near half and half. Overexposure destroys the *dark* level, and the
   photometric field needs both (F7). Default ISO is now 60, and both exposure and ISO are parameters
   because EXP-004/EXP-005 exist to choose them.
4. **A square sensor mode, chosen by my own selection rule.** Asked for a mode at or below 1920 wide,
   "largest by area" picked **1920×1920** — which wastes most of its pixels on a portrait screen and
   delivers no more px/cell than 1920×1080 for 78% more bandwidth. The rule now maximises
   `m·min(W, H/r) / rows`, i.e. resolvable px/cell for the target grid, weighing resolution and
   aspect together as area alone cannot.
5. **The screen ran off the edge of the frame.** Twice, in both directions. Localisation fits the four
   lines of the always-bright ring and intersects them (F10); with one edge outside the frame there
   are not four lines, and nothing downstream can recover. At one point the screen was **74% of the
   frame with a healthy 7.4 px/cell** and still undecodable, because it was clipped.

### The measurement that mattered was never in the log

Every one of these was found by measuring the **pixels** — bounding box, clipping, exposure balance,
inferred rotation, px/cell — and none was visible in the decode output. The decode chain reports
*that* geometry failed, which is correct and nearly useless: "screen not found" is the same line
whether the screen is absent, clipped, blurred, washed out, or 1.9 px/cell.

**The trap that cost the most was an ordering one.** A clipped screen has a perfectly healthy measured
px/cell *over the part that is visible*, so density-first reasoning says "move closer" when the fix is
"move back". I made that inversion myself before building the tool that catches it.

### What was built rather than worked around

`core/src/framing.cpp` (`AnalyseAim`, feature **UI-02**, 11 tests) answers "is this geometry workable,
and if not what should the user change" from one cheap pass over the luminance plane. In portable C++
per ADR-0014, because it is real logic with real edge cases and Kotlin could not be tested off-device.
Its verdicts are ordered by **what to fix first**, and clipping is checked before density precisely to
prevent the inversion above.

Two defects found in the analyser itself while testing it, both worth recording because both produced
*confident wrong advice* rather than an error:

- **Classifying levels against the threshold instead of the class means.** Otsu's threshold sits near
  the smaller class, so when a screen filled a quarter of the frame the threshold landed close to the
  bright level, a fixed band above it reached past 255, and genuinely bright pixels were counted as
  neither bright nor dark. A perfectly sharp synthetic frame reported **46% blur**. Now classified a
  quarter of the separation in from each class mean, which scales with whatever contrast exists.
- **No check that a split exists at all.** On a uniform frame Otsu returns threshold 0, every pixel
  counts as lit, and the analyser confidently reported a screen filling the whole view. Now a frame
  with less than 25 counts of separation is reported as *no screen*.

And the accompanying diagnostic tool made the same class of error as the code it was checking: its
boundary-ring test sampled the outermost **pixel** row, which rounded display corners and composited
system overlays make dark, so it reported "frame missing, offset or inverted" about a perfectly good
frame. Sampling the centre of the outermost **cell** fixed it. **A verifier that fails for the wrong
reason costs as much as the defect it was built to find, and it spends trust that the working tools
then have to earn back.**

---

## F34 — The screen's rounded corners clip a full-bleed frame, and localisation depends on corners `[FACT]`

**Found:** 2026-08-05. **This is the finding that produced the project's first real screen detection.**

After F33 cleared every rig problem, a capture had all of this true at once — and still failed
60 frames out of 60:

| Property | Measured | Verdict |
|---|--:|:--|
| Whole screen inside the frame | bbox at (449,356) in 2688×1512 | not clipped |
| Cell pitch | 7.30 px/cell | comfortable |
| In-image rotation | 1.0° | aligned |
| Interior levels | dark 0.339 / bright 0.539 / **mid 0.122** | *"cells ARE being resolved optically"* |
| Focus | AF converged, 15.8 cm | correct for the rig |
| Exposure | 16 ms at ISO 60 | balanced |

Every measurable property was healthy. The decode still reported `geometry failures 60`.

### The cause

**A phone's display has generously rounded corners, so a full-bleed optical frame has its four
corners physically cut away by the glass.** Measured on the capture with `tools/frame_corners.py`,
walking each bounding-box corner inward along its diagonal to the first lit pixel:

| Corner | Inset |
|---|--:|
| top-left | **77 px** |
| top-right | 23 px |
| bottom-left | 40 px |
| bottom-right | 20 px |

At 7.3 px/cell, **77 px is more than ten cells** — so the entire top-left corner marker was outside
the emitting area. And the insets are *asymmetric*, because perspective and rotation add to the
rounding.

That breaks localisation twice over. The quad is found from the streaming extremes of `x+y` and
`x−y` (F12's O(1) accumulator), and on a rounded rectangle those extremes lie somewhere on the arcs
rather than at the true corners — biased inward by a different amount at each corner, which skews
the homography. Then marker verification, which samples *through* that homography and demands both
an absolute score and a margin over the runner-up (F9), cannot pass because one marker is not
being displayed at all.

### The fix, which removes a second failure mode as a side effect

The renderer now computes the **largest integer cell pitch that fits inside a margin** and centres
the result, instead of stretching the grid across the whole surface.

**Measured result: `frames decoded 48 of 60`, geometry failures 12.** From zero. Reproduced on a
second run at 48/60.

The side effect is worth as much as the fix. Full-bleed rendering required the surface to be an
exact multiple of the grid, and when it was not — a 1080×2340 surface with the 144×240 charter grid
— the pitch came out at 7.5 × 9.75 px with every cell boundary on a fractional pixel (F31).
Choosing the pitch and letterboxing makes it integer for **any** grid and surface pair, so that
class of misconfiguration is now unrepresentable rather than merely warned about.

### Why the simulator could never have found this

`sim/render.cpp` draws a mathematically perfect rectangle. There are no rounded corners to clip,
so **this defect cannot occur in simulation at any setting**. It is the third instance of the same
pattern: a renderer that omits a physical mechanism reports the absence of the failure that
mechanism causes (F14's retraction, F16's missing density cliff, and now this).

Two consequences beyond the immediate fix:

- **The simulator's impairment set should grow a corner-occlusion arm** before SIM-03 calibration
  claims the model matches reality. Without it, calibration would be fitting a model that is
  structurally incapable of the failure.
- **A margin is not free.** It costs cells, and therefore payload per frame. How much margin is
  actually required is a function of the panel's corner radius, which is device-specific and not
  exposed by any API we have found — so it currently has to be measured per device or set
  conservatively. Raised as OQ-039.

---

## F35 — The tooling was built for the developer, not the operator `[FACT]`

**Found:** 2026-08-05, from operator feedback rather than from a test.

During the two-device runs the receiving phone showed, in the operator's words, *"a white screen
with text"* — no help lining up the camera. That was `CaptureActivity`'s report: 11sp monospace on
the default light theme, working exactly as written. The aiming UI existed by then, but as a
**separate activity that had to be launched by hand**, so during the runs that actually mattered
the phone offered nothing.

The deeper shape of it: every screen was reachable only through `adb shell am start`, and the
launcher icon opened the capability probe. All the diagnostic capability built in F33 was pointed
at the person reading a terminal, and none of it at the person holding the phone — who is the one
who has to move it.

Fixed by making the receiver **aim before it records**: live guidance, then an automatic start once
six consecutive `Ready` verdicts land. The streak matters and is not padding — a single `Ready`
between two bad frames is precisely what the `Ready`/`TooDark` oscillation looks like when the
exposure window falls in the panel's blanking interval, and starting on it would record the bad
half. Verified end to end: lined up, started itself, recorded 60 frames, decoded 48, with no `adb`
beyond launching the activity.

### A correction, recorded because the error was mine and instructive

An earlier comment in this repository stated that a `CAMERA_DISABLED` failure was caused by a
**locked screen**. The error itself was real and specific — *"cannot open camera from background",
proc state 20* — but that only establishes the app was **not foreground**. I never verified the
cause, and the operator reports both phones were unlocked at the time.

Worse for attribution: two fixes were applied in the same change — the
`setShowWhenLocked`/`setTurnScreenOn` flags *and* a wake/dismiss-keyguard step in the run script —
so **which of them made it work is unknown**, and possibly neither did. The flags are kept as cheap
insurance and the comment now says exactly that rather than asserting a diagnosis.

This is the F15 pattern for the third time in this project: a plausible mechanism written down as
fact, with the real cause unexamined because the symptom went away. The specific habit worth
naming is **changing two things at once while chasing one failure** — it converts a diagnosis into
a guess, and it did so here.

---

## F36 — First real data read off a screen: `H` = 0.3784, and the erasure is a GEOMETRY problem `[FACT]`

**Measured:** 2026-08-06, Galaxy S26 Ultra → Pixel 8, 120×260 grid at 9×9 px, 15 states/s,
2688×1512 capture, ~19.8 cm, exposure 15.0 ms at ISO 60.

**14 optical frame headers decoded from real optics.** This is the first time this project has read
any data off a phone screen through a camera.

The number is not soft. A header decode requires the Reed–Solomon layer to correct it *and* its
CRC-32 to validate (F2). Fourteen successes therefore means fourteen frames of genuinely correct
bytes — not a plausible-looking figure that might be noise.

| | |
|---|--:|
| frames in | 60 |
| frames localised | 37 |
| **header success `H`** | **0.3784** (14 ok, 23 failed) |
| cell erasure rate | 0.5456 |

`H` appears directly in the goodput model and had never been measured on real hardware. It is now
measured, on one rig, at one geometry: **0.3784**. The model's scenarios assume far higher.

### What the erasure attribution says, and it is not what it looked like

The erasure rate of 0.5456 looked like a photometric problem. The per-node telemetry says it is not:

| | |
|---|--:|
| nodes below separation | **0.0163** |
| nodes over residual | **0.2619** |
| mean local separation | 142.0 (erase below 12.0) |
| bright nonuniformity | 1.34 |

Only 1.6% of lattice nodes have collapsed levels — separation is *healthy*, at 142 against a
threshold of 12. But **26% of nodes fail the pilot-fit residual test**, and since a failing node
erases its whole region, that is enough to erase over half the payload.

**The pilot-fit residual is a "my model does not fit here" signal (F8), and the reason it does not
fit is one layer upstream.** Two numbers in the same report point at geometry:

- **`refined frames 0`, `full acquisitions 60`.** The tracker never once refined an existing lock —
  every frame paid a fresh full acquisition. So there is no temporal consistency in the homography
  at all.
- **23 of 60 frames failed geometry outright.** Localisation is marginal, not solid.

A homography that is slightly wrong samples each pilot slightly off its cell centre, picking up a
blend of its neighbours. The pilot then disagrees with the level the field fitted for it, the
residual rises, and the region is erased — correctly, because decoding against a
confidently-wrong reference is exactly what F8 exists to prevent. **The erasures are the receiver
protecting itself from a geometry problem, not a photometry problem.**

That reframes the next work. Chasing exposure, brightness or pilot density would have been the
obvious response to a 0.55 erasure rate and would have been aimed at the wrong layer. F10 measured
worst-case cell-centre error **< 0.25 cells** — in simulation, against a mathematically perfect
rectangle. On real optics, with rounded corners biasing the quad extremes inward by an asymmetric
20–77 px (F34), that accuracy is clearly not being achieved.

### No payload yet, and why

Fourteen readable headers do not make a transfer. At a 0.5456 cell erasure rate the intra-frame FEC
is far outside its budget — `nsym = 32` over 255-byte codewords corrects roughly 12% of bytes as
erasures, and one unreadable cell condemns its whole byte (F18's recorded limitation). So the
payload is unrecoverable at this erasure rate no matter what the fountain layer does, and **no
goodput figure exists.**

### What this took, recorded because the count is the point

Six distinct causes stood between "the app runs" and "14 headers decoded", and **none of them was
visible in the decode log**, which said only `geometry failures: N of N` every time: camera blocked
from background; focus at infinity against a 16 cm target; ISO 400 overexposing an OLED; a capture
mode chosen by pixel area picking a square sensor mode; rotation inflating the bounding box; and
rounded corners eating a marker (F32–F35). Then a seventh — exposure derived from `fps`, quartering
it to 4.16 ms and driving separation *negative* — which the new photometric telemetry caught
immediately and by name.

**Every one was found by measuring the pixels rather than reading the log.** The telemetry added
along the way is why this last one took minutes instead of a day, and why the erasure is now
attributed to geometry rather than guessed at.

---

## F37 — A verdict is not a viewfinder `[FACT]`

The aiming screen (UI-02) drew the *analysis* — where the screen sat in the frame, which edge
was clipping — and deliberately not the camera image. The reasoning was sound on its own terms
and is recorded in `AimActivity`: a schematic shows the thing that decides success, costs no
YUV conversion, and cannot mislead by looking plausible.

It was still unusable. It shows what is **wrong** but not where the camera is **pointing**, so
lining two phones up meant moving them blind and waiting for a verdict to change 150 ms later.
The operator has to close a visual feedback loop, and a loop needs a continuous signal, not a
classification.

**Fix:** a live preview *behind* the overlay, as a second target on the same capture session, so
the compositor does the scaling and colour conversion and none of the analysis budget goes on it.

**Generalisation.** "This display shows the decision-relevant quantity" is an argument about
*information*. Aiming is a *control* task, and a controller needs an error signal it can act on
continuously. The two are not the same requirement, and optimising a control surface for
information content is how you build something correct and unusable.

---

## F38 — The overlay rotated, the image did not, and moving left moved the box up `[FACT]`

The schematic was drawn in **sensor** coordinates while the operator was moving the phone in
**display** coordinates. With a rear camera mounted at `SENSOR_ORIENTATION = 90`, moving the
phone left moved the box up. Reported, accurately, as impossible to line up with.

The overlay was fixed by rotating it into display space. The preview underneath it was not, and
that half of the defect survived three further attempts — see F41 and F42.

**Generalisation.** Any two-layer display where one layer is derived from sensor data and the
other from the same data through a different transform has a registration bug waiting in it. The
symptom is not "it looks wrong" — both layers look plausible alone. It is that *the control
inputs map to the wrong axis*, which is only visible to someone holding the device.

---

## F39 — Lit fraction is payload-dependent and must never gate a verdict `[FACT]`

The aim analyser judged brightness partly on the share of interior pixels at the bright level. On
a fountain-coded stream that share is a property of the **payload**, not of the optics: a block
padded with 22 zero symbols out of 64 renders mostly dark, and the analyser called a
correctly-exposed screen too dark.

This also produced a wrong diagnosis that cost real time. A dim, flickering transmitter was
attributed to OLED PWM, and the operator was told to raise the screen brightness by hand — which
they could not do, because the app was holding the brightness. The panel was fine. The frame was
mostly zeros.

**Fix:** judge brightness on **levels and separation** — quantities the channel determines —
never on how many cells happen to be lit. `lit_fraction` is retained as diagnostic output and
documented as payload-dependent in `framing.h`.

**Generalisation.** A channel-quality metric must be a function of the channel. If a metric moves
when the payload changes, it is measuring the message.

---

## F40 — Autofocus optimises the wrong objective, and so did my first focus metric `[FACT]`

Continuous AF converged on a value that looked plausible in a photograph and gave a **level
separation of 5.1** against a decode threshold of 12 — unreadable. AF maximises general-scene
contrast; this channel needs the two luminance levels far apart. Those are different objectives
and they have different optima.

The first sweep built to find a better focus was **also** wrong: it scored each focus distance by
mid-level fraction, and picked 40 cm for a subject at 20 cm. Scoring on **level separation**
picked the right distance.

Worse, an intermediate version *locked* the bad AF value, making a transient problem permanent —
a regression introduced while fixing the thing it made worse.

**Generalisation.** When overriding an adaptive system, the override needs the objective the task
cares about, stated explicitly. Two consecutive versions here optimised proxies, and both proxies
were defensible until measured against the quantity that decides decoding.

---

## F41 — Letterboxing the container did not stop the stretch `[FACT]`

A landscape camera stream in a portrait view was squashed vertically. The fix was a self-sizing
container that held the correct aspect ratio. The operator reported the same stretch.

Sizing a box does not rotate what is drawn in it. A `SurfaceView` stretches whatever buffer it
holds to fill its bounds, so a landscape buffer in a portrait-shaped box is still landscape
content, now squashed into a differently-shaped hole.

---

## F42 — Pinning the buffer size did not either, and the sign was wrong `[FACT]`

Second attempt: pin the surface's buffer size. Same outcome, same reason — the pixels were never
turned. Only then did the preview move to a `TextureView`, which renders through a transform
matrix and can therefore rotate and scale together.

The transform's **sign** then cost another round: rotating by *minus* `SENSOR_ORIENTATION` leaves
the image lying on its side. `SENSOR_ORIENTATION` is how far the sensor is mounted clockwise, so
rotating by *plus* that much undoes it.

**Three wrong fixes in a row, each verified by reasoning and none by measurement**, against an
operator who reported the same symptom every time and was right every time.

---

## F43 — Two implementations of the preview, and the fixes all landed on the one nobody used `[FACT]`

**Severity: high — it invalidated four consecutive "verified" claims.**

After F42, the preview was correct. The operator still reported a stretched image.

`AimActivity` and `CaptureActivity` each had their **own** preview. Every fix, and every
verification, went to `AimActivity`, because that is what `adb shell am start` launches. The
**Receive** button opens `CaptureActivity`, which still used the `SurfaceView`-in-a-sizing-box
arrangement that F41 had already established cannot work. The operator was looking at the
untouched copy the entire time.

The operator's own report is what separated them, and it named both defects precisely:

> "every time you launch the app on the pixel it is sideways but if I launch it fresh and click
> receive its upright, just stretched up and down still"

Two launch routes, two different screens, two different symptoms — in one sentence.

**Fix:** `CaptureActivity` uses `CameraPreviewView`; `AspectFrameLayout` is **deleted** rather
than left where it could be reached for again. The device-rotation term and the tap-to-rotate
override went too: every activity is `screenOrientation="portrait"`, so the term is zero by
construction, and `AimView` rotates its overlay by `sensorRotation` alone — any term present in
one and absent in the other slides the guidance off the image it annotates.

**Two methodological failures, both mine, and the second is the worse one:**

1. **I verified on the path I could script, not the path the operator uses.** `am start` with
   explicit extras was convenient; the Receive button sends different extras (`maxWidth` 2688,
   not 1920) *and* opens a different activity. A test that does not traverse the user's entry
   point is not a test of the user's experience. The verification run now drives the actual
   button via `uiautomator` and a tap.
2. **My screenshot check was measuring the overlay, not the camera image.** It took the bounding
   box of every bright pixel, so the schematic's thin lines and text set the extremes: it
   returned an *identical* `986x1830` box for two runs whose scenes were entirely different —
   one of which had the transmitter dark. It reported UPRIGHT both times and would have reported
   UPRIGHT for a sideways image. Replaced with a largest-connected-component measurement, which
   isolates the filled blob of the transmitting screen from thin UI strokes.

A check that cannot fail is not evidence. Both of these returned green while the operator was
looking at a broken screen, which is the specific way a verification habit can be worse than no
verification at all — it converts "I have not checked" into "I have confirmed".

**Verified after the fix**, through the Receive button, transmitter live:
`camera 2688x1512, sensor 90 -> rotate 90, uniform scale 0.714, view 1080x2400`, and the
largest bright blob measures taller than wide. Uniform scale is the load-bearing part: one factor
on both axes cannot stretch.

**Generalisation.** When a user reports a defect that measurement says is fixed, the first
hypothesis should be that the measurement and the user are looking at different things — not
that the user is mistaken. Four rounds were spent on the second hypothesis.

---

## F44 — The TextureView held the matrix and did not honour it `[FACT]`

**The measurement that ended five rounds of guessing, and it was a contradiction, not a value.**

After F43 unified the two previews, the operator still reported the image sideways and stretched
to fill the portrait screen. So the transform was read back **out of the view** rather than logged
on the way in:

```
CONTENT LANDS AT (0,240)-(1080,2160)  1080x1920  bars top 240 bottom 240  identity=false
```

`getTransform()` returned the correct matrix. `mapRect` put the content in the correct place, with
240 px letterbox bars top and bottom. Meanwhile, with the overlay hidden so that nothing but the
camera image and a black background could appear, the composited screen carried image signal in
**both** bar regions — rows 2160–2400 at mean luminance 113 where the view's own transform demands
zero.

Both facts are solid and they cannot both describe what is on the glass. **The matrix was held and
not composited.** The root cause was not pursued further, because it stopped mattering:

**Fix: stop asking the platform to rotate anything.** The receiver already reads every frame's Y
plane on the CPU for aim analysis, so the pixels are in hand. `LumaPreviewView` copies them into a
`Bitmap` — subsampled by an integer stride to ~720 px on the long edge — and draws it through a
`Canvas` matrix. There is no `SurfaceTexture`, no compositor transform hint, and no producer queue
between the rotation and the pixels. `CameraPreviewView` and its `TextureView` are deleted.

Grayscale is not a compromise here: the link is luminance-only, so the preview now shows exactly
what the demodulator sees, which is strictly more informative than an ISP-prettified image that
can look fine while its levels are unusable.

**Generalisation, and it is the expensive lesson of this whole sequence.** Logging a value you
computed proves only that your arithmetic ran. Five rounds of "verified" rested on exactly that,
and every one of them was contradicted by the operator. The moment the *same quantity* was read
back out of the component that owned it and compared against an independent observation of the
result, the defect was located in one attempt. **Verify by reading back from the system under
test, never by echoing your own input** — and when a component's self-report and the observable
result disagree, stop debugging the value and replace the dependency.

---

## F45 — The sampling grid sat off the cells `[FACT]`

*Recorded 2026-10-23 from commit 90167ca (2026-08-07), where F45–F48 were written up in the commit
message and in code comments but never reached this document — the same gap ce5ea1c closed for
F37–F43. Measured on the first above-cliff capture, 226 real frames, Galaxy S26 Ultra → Pixel 8.*

Corner markers pin the homography at the corners, but a projective map's error is not uniform,
and at 8.85 px/cell a half-cell slip is 4.4 px. A sub-cell search scored on the **pilots** — whose
levels are known by construction, so it needs no payload knowledge (F15) — moved local separation
**40.9 → 54.9**. Together with F46–F48: cell erasure **0.7577 → 0.4420** and header yield `H`
**0.0258 → 0.1289** on the same frames.

## F46 — Erased cells were voting in the header's majority vote `[FACT]`

NaN is the erasure convention and `NaN > threshold` is false, so every unreadable cell cast a
confident DARK vote. Erasures are not neutral noise; they all land on the same side. A real bug,
but not the binding constraint: fixing it alone moved nothing, which is how F47 was found. (F50
takes the same idea further.)

## F47 — The spatial map, not the aggregates, found it `[FACT]`

Summed over a frame, a grid drifting against the cells, glare and defocus all read as "high
residual, high erasure". They look nothing alike in space. `ffreplay --map` showed a healthy bottom
two-thirds at 140–190 separation against a **dead band across the upper third** — out of focus,
because the two phones were not parallel and the depth of field is shallow. Physical, not
algorithmic. The live coverage map (commit 4e0208e) moves the same measurement to the operator.

## F48 — The residual gate was miscalibrated by a wide margin `[FACT]`

0.25 was always a hypothesis. It erased 70% of the lattice, and relaxing it walked header yield
monotonically up 6.8×: the residual is measured about a locally fitted plane, the real field is not
planar, so the gate reported "not a plane", which is true everywhere. Shipped at 1.2, the loosest
value that still erases an occluded region (bisected: fails at 2.0, passes at 1.2). A tighter gate
needs a better field model, not a smaller number (EXP-026). Interior margin swept 0.20–0.44 on the
same frames moved erasure by 0.006: **cell crosstalk is not the limiter.**

---

## F49 — The fountain code, not the channel, was costing 20–35% of goodput `[FACT]` / `[HYP]`

**The defect** `[FACT]` — a property of the code, independent of the channel model. Protocol v1
sent a systematic prefix followed by robust-soliton LT repair symbols, decoded by peeling. On
`ffsim` at 5–10% frame loss the fountain overhead was **0.20–0.42**; the performance model assumes
`Rfountain` 0.90–0.97, and v1 delivered ~0.75–0.83.

**The obvious fix was wrong.** The natural suspect was the peeling decoder, so the first experiment
replaced it with Gaussian elimination on the identical symbol stream. It recovered almost nothing:

| frame loss | peeling overhead | GE overhead (same LT rows) | GE, **dense** rows |
|--:|--:|--:|--:|
| 5% | 0.202 | 0.202 | **0.022** |
| 10% | 0.266 | 0.260 | **0.025** |
| 30% | 0.328 | 0.285 | **0.025** |

*k = 64, 400–2000 trials per cell, robust soliton c = 0.05, δ = 0.05 as in v1.*

The waste is in the code. After the systematic prefix the receiver is missing only the few source
symbols that were lost, and a low-degree repair symbol almost never touches one of them — at 5%
loss a degree-3 symbol touches a missing source symbol with probability ~0.14, so ~86% of repair
symbols carry nothing new **whatever the decoder does**. Robust soliton is designed for a
non-systematic code; bolted onto a systematic prefix it is the worst of both. A dense row (each
source symbol included with probability ½) touches each missing symbol with probability ½, and GE
over GF(2) completes within ~1.6 symbols of k at any loss rate. That is the textbook random linear
fountain and carries no patent question (RISK-016).

**Two further losses**, both structural and both fixed in ADR-0015:

- **Block tail.** With no back channel the transmitter cycles every block until the receiver stops
  looking, so a transfer ends when its *unluckiest* block completes. The spread per block shrinks
  as 1/√k, so v2 uses as few, as large, blocks as the decoder bound allows (`AutoBlockSymbols`).
- **Padding (F4).** v1 transmitted the zero padding past the end of the file every cycle. On a
  64 KB file at 120×200 that was 34 of 64 frames — **half the transfer**. v2 never sends it; the
  receiver primes it from the manifest.

**Effect** `[HYP]` — simulated, same flags and seeds, HEAD (v1) against v2, 256 KB, `Fd` = 60
assumed:

| channel | v1 goodput | v2 goodput | v1 overhead | v2 overhead |
|---|--:|--:|--:|--:|
| clean | 120.0 | 126.9 | 0.00 | 0.00 |
| 5% drop | 90.4–94.8 | 117.3–123.9 | 0.20–0.26 | 0.008–0.017 |
| 10% drop | 80.0–88.8 | 113.8–115.5 | 0.22–0.38 | 0.000–0.017 |
| 30% drop | 54.1–60.7 | 88.3–97.8 | 0.39–0.67 | 0.000–0.017 |
| F18-impaired | 65.9–78.0 | 103.1–115.5 | 0.36–0.59 | 0.008–0.017 |

KB/s, 3 seeds. 64 KB on the image path: **60.0 → 120.0 KB/s**, entirely the padding.

**Generalisation.** When a component underperforms, the component you suspect is the one you
already understand. Swapping the decoder was the cheap, local, obvious change, and running it
first is what showed the loss lived one layer up, in a design decision nobody was looking at.

---

## F50 — The header decoder was not told which bytes it could not read `[FACT]` / `[HYP]`

**Defect** `[FACT]`. `DemodulateHeader` majority-voted the replicated copies and handed bytes to
`HeaderCodec::Decode`, which took no erasure positions. A bit whose every copy was erased became 0
(F46 removed the bias; it did not flag the bit), so each unreadable byte cost the RS layer two
units of budget as an error instead of one as an erasure — on the layer whose success `H` is the
measured hardware bottleneck (F36: 0.13–0.38).

**Fix.** Copies are combined by confidence — distance from threshold in half-separations, clipped
to ±1 — and a bit whose tally is under half a clean copy marks its byte erased. `HeaderCodec`
decodes with the erasures and, if that fails, again without: a flag on a byte that was in fact
right spends budget for nothing, and the CRC guards both attempts, so the second try can only add
successes. The receiver counts **headers rescued by erasures** per run, so the gain is measured on
every real capture rather than assumed.

**Effect** `[HYP]`. F18-impaired channel, `H` 0.951–0.956 → 0.961–0.969. A dead stripe across 22
bytes of every copy — past the 16 blind errors nsym = 32 can locate — now decodes (test
`Session.HeaderErasuresRescueFramesABlindDecodeLoses`).

---

## F51 — A global shift is the wrong correction for a non-uniform error, and the objective was blind `[HYP]`

**Corner refinement.** F45's search moves the whole grid by one translation. That is right only
when the error is the same everywhere, and neither error measured on real optics is: F34's
rounded corners bias the quad's extremes inward by an asymmetric 20–77 px, and a slightly wrong
perspective is exact at two corners and wrong at the others. The search then finds a compromise.
`FramePipeline::Align` now follows the shift with a bounded coordinate descent over all four
corners (8 degrees of freedom — a full homography), on the same pilot-contrast objective.

**The objective was flat where it mattered.** Scored through the decode sampler (margin 0.3), the
refinement stalled at **0.22 cells**: every sub-sample of a pilot stays inside its own cell until
the error exceeds the margin, so contrast has no gradient below it. A wide window (0.05) had the
gradient but **drifted a correct grid by 0.13 cells**, because reading a pilot's edge lets a
same-level neighbour raise the score — a payload-dependent bias. And on pilots alone the worst case
was **0.34–0.55 cells**: ~80 interior pilots barely constrain the corners, which are what this step
moves.

**Resolved by measurement**, not argument — `Alignment.AlignWindowSweep`, 4 scenes × 3 corner
perturbations, worst case:

| objective | worst error left | worst drift of a correct grid | cost |
|---|--:|--:|--:|
| pilots only, decode window | 0.22 | — | — |
| pilots only, margin 0.05 | 0.34–0.55 | 0.13 | — |
| **all structural cells, margin 0.15, 4×4, 512 anchors** | **0.110** | **0.095** | 4.9 ms |
| all structural cells, margin 0.15, 4×4, 256 anchors | 0.135 | 0.122 | 2.5 ms |
| all structural cells, decode window | 0.324 | 0.082 | 1.4 ms |

Markers, boundary ring, timing tracks and guards are known by construction exactly as pilots are,
so scoring on them keeps F15's rule — and they sit at the edges, where the corners are decided.

**Effect** `[HYP]`. The simulator renders only projective geometry, so a single shift was always
sufficient there and it could never have shown a reason for this. A radial lens term (`--lens-k1`)
is the first image-path impairment a homography cannot represent:

| lens k1 | shift + corners | shift only | no alignment |
|--:|---|---|---|
| 0 | 0.086 cells, 120 KB/s | 0.42 cells, 120 KB/s | 0.33 cells, 120 KB/s |
| −0.01 | **0.25 cells, 120 KB/s** | 0.57 cells, **does not verify** | 0.64 cells, **does not verify** |
| −0.02 | 0.49 cells, 91 KB/s | does not verify | does not verify |
| −0.03 | `H` 0.72, does not verify | `H` 0 | `H` 0 |

64 KB, 900×1500, yaw 6°, pitch −4°, blur 1, noise 6; "does not verify" = not within 300–400 states.
Note row 0: **shift-only is worse than no alignment in the worst case** — the compromise effect,
measured. And at k1 = −0.01 every header decodes on the failing arms; the payload dies on confident
*errors*, not erasures (F23's failure mode).

**What it does not fix.** Beyond |k1| ≈ 0.02 no single homography fits. The right tool there is
the camera's own calibration: Camera2 publishes `LENS_DISTORTION` and
`LENS_INTRINSIC_CALIBRATION` (API 28+), which the sampler could apply before the homography. Not
built; tracked as OQ-038.

---

## F52 — Four copies of the protocol, and the receiver was handed the manifest `[FACT]`

The frame → header → FEC → fountain chain was written out by hand in `ffsim`, in `ffreplay` (which
stopped at the header), in the JNI transmitter, and in the end-to-end tests. And every receiver
was built from `tx.manifest()` — the transmitter's own object — which is the one thing a real
receiver can never have. **No real capture could ever have completed a transfer**, however good
the optics, and nothing would have said why. `FileManifest::Decode` was called only by tests, and
the flag reserved for an in-band manifest was never set.

Separately, `ffsim --image-path` wired the tracker, sampler and photometry together itself instead
of running `FramePipeline`, so F45's alignment — the single largest real-capture improvement — was
invisible to the simulator. HANDOFF already said not to duplicate `FramePipeline`; it had been.

**Fix.** The session layer (C13a): one `TransmitSession`, one `ReceiveSession`, used by `ffsim`,
`ffreplay`, the JNI transmitter and the tests. The receiver is told the grid and nothing else; the
code rate is in every header and the manifest arrives in-band (ADR-0015). Symbols read before the
manifest are held and replayed, so a receiver that misses it loses nothing. `ffreplay` now runs a
verified transfer and reports goodput, and reports the three display-state rates EXP-006 needs from
header sequence numbers.

**Generalisation.** A test harness that supplies something the real system has to *earn* is not
testing the real system. The end-to-end tests passed for months against a receiver that could not
exist.

---

## F53 — A moving phone loses frames to blur, not to the tracker `[HYP]`

**Question.** Does the receiver survive being held by hand, and if not, which layer gives way?
The simulator could not say: every image-path impairment was static per frame, and the only motion
(`RenderedSource`'s jitter) was i.i.d., which no hand produces.

**Built to answer it** — `sim::MotionConfig` / `RenderMoving`, `ffsim --shake S --pan X Y`:
smooth sway (~1 Hz) plus physiological tremor (~9 Hz) plus roll and distance sway, scaled by one
intensity; sweeping pans that reverse so the screen stays in view; motion blur by integrating the
exposure; rolling-shutter skew by rendering row bands at their own readout times.

**Hypothesis tested and rejected.** The tracker searched around where the screen *was*, so a
constant-velocity prediction — search where it is *going*, widen the window with speed — looked
like the obvious fix. It made no difference. Every row of the A/B was identical at 30 fps (shake up
to 4, pans up to 1 image width/s), and identical at 10 and 6 processed frames/s, except one arm
(pan 0.5 at 6 fps) where prediction lost the lock once per seed and the plain search never did.
The 25% annulus already covers hand motion. **Prediction ships off** (`predict_motion = false`),
kept switchable for real rolling-shutter captures.

**What actually fails** is reading, not tracking. With the lock held throughout:

| motion (30 fps, 15 ms exposure) | header success | verified goodput |
|---|--:|--:|
| shake 1–2 | 1.00 | full |
| shake 3 | 0.87–0.91 | ~60% |
| shake 4 | 0.63–0.64 | ~25% |
| pan 0.3 image/s | 0.81–0.86 | ~70% |
| pan 1.0 image/s | 0.12–0.13 | none within 300 frames |

Motion during the exposure smears cells into their neighbours — at 6 px/cell, a 1 image-width/s
pan is ~2 cells of blur in 15 ms. No receiver algorithm recovers that; a shorter exposure or a
steadier hand does. Hence F54: the remedy for motion is to **tell the person**, at the moment it
matters, and to make sure nothing they have already received is lost meanwhile.

**Kept from the work:** the tracker's velocity estimate, now per *second* from capture timestamps
with a 0.1 s time constant. A per-frame weight came out ~15% noisier at 30 fps than at 10 because
corners are pixel-quantised and more frames meant less smoothing per second; a receiver that keeps
up with only some frames must report the same speed regardless.

---

## F54 — The receiver could not tell the person anything, and could not deliver a file `[FACT]` / `[HYP]`

**Defect** `[FACT]`. The Receive button recorded 120 frames — four seconds — for desktop analysis,
then showed a capture report. It could not deliver a file, could not say whether anything had
worked, and gave no signal that the phone had drifted until the recording was over. For a transfer
lasting a minute, the person holding the phone had nothing to act on.

**Fix — the live receiver (C13b).** `LiveReceiver` runs `FramePipeline` + `ReceiveSession` on each
camera frame and returns a phase, one hint and one plain sentence; JNI and Kotlin only display it
(ADR-0014). The Receive button now decodes on the phone, frames dropped whenever the decoder is
busy (fountain coding makes that free), until the file verifies and is saved. Scripted
experiments keep the recording path.

The behaviour that matters most under motion: **progress never goes backwards**, and when the
screen leaves view the message is *"Paused at 42% — point the camera back at the sending screen.
Nothing received so far is lost."* — because it is true.

**Caught by the tests, before any phone ran it:** the time-remaining estimate kept showing
"about 3 s left" while the camera pointed at the floor, extrapolating from the seconds before the
pause. It is now withheld whenever nothing is being read.

**The "hold steadier" threshold, calibrated** `[HYP]` — `LiveReceiver.DISABLED_BlurCalibrationTable`:

| motion | estimated blur mean / peak (cells) | header success |
|---|--:|--:|
| shake 1 | 0.29 / 0.49 | 1.00 |
| shake 2 | 0.57 / 0.95 | 1.00 |
| shake 3 | 0.85 / 1.40 | 0.89 |
| shake 4 | 0.89 / 1.64 | 0.68 |
| pan 0.6 | 0.93 / 1.34 | 1.00 |
| pan 1.0 | 0.98 / 1.92 | 0.02 |

The first threshold (0.35 cells) would have nagged a careful hold whose every frame decoded. Blur
alone cannot separate pan 0.6 (fine) from pan 1.0 (dead), so the hint now needs blur over 0.75
cells **and** under 80% of recent frames read, or blur over 1.5 cells outright. A warning that
fires while everything works is one people learn to ignore.

---

## F55 — The aiming text block did not measure its own banner `[FACT]`

`AimView.drawTextBlock` drew the banner line ("Recording 40/120 — …") and advanced past it, but
computed the scrim height without it, so with a banner showing the last line of guidance was drawn
46 px below the bottom of the screen — the line most likely to be the instruction. Found by
reading the code while adding the live status card, not by a report; fixed by measuring it.

---

## F56 — One lamp in view defeated both the detector and the aiming advice `[FACT]` / `[HYP]`

**Defect** `[FACT]`, by reading the code after the first hand-held test (F57). `ScreenDetector`
took the corner extremes of **every pixel above threshold anywhere in the frame**, and
`AnalyseAim` took its bounding box the same way. A window, a desk lamp or a ceiling light in view
therefore stretched the "screen" to itself: detection failed outright, and the aiming analyser
reported the screen **clipped** at an edge it was nowhere near — "move back", indefinitely,
however well the phone was held. A rig in a dark room never shows this; a person holding a phone
in a lit room shows it constantly, and with the screen filling only half the view (F57) there is
more room around it for a light source to appear.

**Fix.** `LargestLitRegion`: lit samples per block on a coarse map (at most 128 blocks a side —
bounded memory, F12), 8-connected components, and the component that **spans the most of the
image** is the screen. The boundary ring is always lit and one closed loop, so it is one component
on any payload, a frame of zeros included (F15). Ranked by extent, not by lit count: a solid
window outweighs a mostly dark frame by lit count. Padded one block, because the tip of a rotated
screen's corner is a sliver that can fall below the noise floor of its block — two tracker tests
caught exactly that on the first version.

Tests: `ScreenDetector.LocatesAScreenWithALampElsewhereInTheView` (two distractors, corners within
2 px), `AimAnalysis.ALampElsewhereInTheViewIsNotTheScreen`, `LargestLitRegion.*`. A light source
*touching* the screen (glare on the glass) still merges with it; marker verification remains the
backstop there `[HYP]`.

---

## F57 — The first hand-held test: no focus, no feedback, and an unoptimised decoder `[FACT]`

**What happened** `[FACT]`. Galaxy S26 Ultra and Pixel 8, both hand-held, both on 120x260. Each
phone in turn sat on the aiming screen — the Pixel for 2 min 22 s, the S26 for 51 s — and **never
started receiving**. The operator reported that the camera would not focus, and that when it did
show a good view nothing happened. The logs agree: no live-receiver line at all, only the preview
being set up.

**Causes, in the order they bite:**

1. **The geometry asked for a distance the lens cannot focus at.** Receive captured at 2688x1512.
   At the 7.5 px/cell floor, 260 rows need 1950 px, so the sender's screen (124 mm of drawn frame
   on the S26) had to fill 72% of the frame's long axis: about **12 cm** from the Pixel 8's main
   camera (6.9 mm lens, 9.79 mm sensor width). That lens focuses no closer than **10.5 cm**
   (`LENS_INFO_MINIMUM_FOCUS_DISTANCE` 9.52 dpt; the S26's main camera, 10.0 dpt). The workable
   band was ~1.5 cm deep — and the depth of field at 15 cm, from the reported hyperfocal distance,
   is about ±4 mm. Hand-held, that is "autofocus not working".
2. **A gate in front of the decoder.** Receiving began only after six consecutive *Ready* verdicts
   from the aiming heuristic. Every condition above, and F56's lamp, kept it from ever saying
   Ready, so the decoder — the only judge that matters — never saw a frame.
3. **Focus was locked at the hand-over.** Correct for a dataset, wrong for a hand: a lock taken at
   one distance is blurred a few millimetres later.
4. **The native decoder was built `-O0`.** AGP builds the debug APK's CMake with
   `CMAKE_BUILD_TYPE=Debug`, which on this toolchain adds no `-O` flag. Measured on the Pixel 8 at
   4080x3072 with no screen in view: **139.7 ms/frame at `-O0`, 44–55 ms at `-O2`**; the S26,
   31 ms. Every hardware run so far used the `-O0` build.

**Fixes.**
- Receive captures at the highest-resolution 30 fps YUV mode (4080x3072 on the Pixel, 4080x3060 on
  the S26; `maxWidth` 4096). The screen then needs 48% of the frame: a hold from ~10.5 to ~18 cm
  on the Pixel, ~10 to ~15 cm on the S26 receiving from the Pixel `[HYP]` (geometry, not yet a
  measured hold).
- No aim gate for live receiving. The aim analysis now runs beside the decoder and supplies the
  explanation only while nothing is being read.
- Continuous AF throughout, weighted on the middle 40% of the view; a full AF scan (`AUTO` +
  trigger, not a trigger in continuous mode, which merely confirms wherever passive focus already
  sits) when the screen is in view but nothing has read for 2.5 s; tap-to-focus; and when the lens
  reports it is at its closest distance, *"Too close to focus — move back a little. This camera
  focuses from about 11 cm."* — the one case where waiting never helps.
- OIS on, EIS off: OIS acts during the exposure, which is when blur happens (F53); EIS warps each
  frame after capture, bending the edges the detector fits.
- Exposure control on the screen's own interior levels (bright cells 150–235, dark below 100):
  ISO first, then a shorter exposure once ISO is at its floor. Steered **only** while the decoder
  has verified the screen — the first device run of it tuned ISO to the room while pointed at a
  ceiling — and relaxed to the start values when the screen is gone.
- `-O2` for the native library in every variant.

**What is still not known** `[HYP]`: whether a hand-held transfer now completes. Nothing above has
been tested with two phones yet; it is what the next run answers.

---

## F58 — A decoded header does not prove the grid; a decoded payload does `[FACT]`

The receiver now works out the sender's grid itself (one native receiver per candidate, frames
dealt to them in turn), so the two phones no longer have to be set to the same grid. It first
committed to a grid on the first decoded **header**, on the reasoning that a header carries RS
parity and a CRC. `LiveReceiver.OnlyTheSendersGridDecodesAPayloadSoTheGridCanBeDetected` refuted
that: a 120x208 receiver read a 120x200 sender's header on **60 frames of 60**. The header band
is a few rows at the top of the frame, and a near-miss grid puts those rows almost exactly where
the true grid has them.

It now commits on the first decoded **payload** — intra-frame FEC passed and the CRC matched —
which spans every cell. Across the same 60 frames no wrong grid decoded one. The three grids the
app offers (120x260, 144x240, 180x390) are far enough apart that none of them reads even a
header of another `[FACT, simulated frames]`.

---

## F59 — The first verified transfer on real phones, and why it took 217 s `[FACT]` / `[HYP]`

**Result** `[FACT]`, 2026-10-23 17:26:48. Pixel 8 sending, Galaxy S26 Ultra receiving, both
hand-held: the 195.1 KB test file **received and SHA-256 verified**, saved to Downloads/FileFlow.
216.6 s from pressing Receive, 2284 frames read: **0.9 KB/s wall-clock goodput**. The first
completed transfer in this project on real optics.

**Where the time went** (S26 log):

| span | distance (lens focus) | px/cell | headers read | progress |
|---|---|--:|--:|--:|
| 17:23:12–17:24:11, 59 s | ~12 cm | 13–14 | some | finding the grid (F58) |
| 17:24:11–17:26:44, 153 s | ~12 cm | 12–15 | **~100%** | **0%** — no manifest |
| 17:26:44–17:26:48, 4 s | 17 → 25 cm | 7.6 → 4.6 | 100% | **0 → 98 → 100%** |

The receiver read essentially every header at 12 cm, and buffered symbols, but no MANIFEST frame
decoded for two and a half minutes. Moved back, a manifest decoded within four seconds; the
buffered symbols replayed to 98% at once, and it verified. So most of the file had been read up
close, and the person saw 0% the whole time.

**Interpretation** `[HYP]`: at 12 cm payload decoding was rare — enough over 150 s to hold 98% of
the file, not enough to catch one of the manifests (then one state in 32, at 7.5 states/s). The
header is a few rows at one end of the frame; the payload spans all of it. Depth of field at 12 cm
is a few millimetres (F57) and scales with distance squared, so at 25 cm it is ~4x deeper: a
slight tilt between two hand-held phones defocuses part of the payload close up, and none of it
farther out (the same mechanism as F47). Not yet isolated: a recording at both distances would.

**It also decoded at 4.6 px/cell**, under the 7.5 the aiming analyser treats as the floor (from
EXP-001, which was measured with focus and framing problems this run did not have). One
observation, not a new floor.

**Also found on this run:**
- The Pixel sent at **7.5 states/s, not 15**: its panel ran at 60 Hz although 120 was requested,
  and the divisor was computed from the request. The renderer now measures the vsync interval and
  re-derives the divisor every 60 frames — measured on the Pixel afterwards: 59.5 Hz, divisor 4,
  14.9 states/s.
- The status card changed **several times a second** (reading / not readable / point the camera
  / too close): the "goes in and out" the operator reported. A message now holds for 1.5 s before
  one of a different kind replaces it.
- Exposure control treated a lifted dark level as overexposure and drove ISO to the sensor's floor
  (9). The dark cells were lifted by blur, not light; it now steers on the bright level only.

**Changes:** guidance is now "about 20–25 cm", "move back" when the lens reports < 15 cm and no
payload is arriving, "move closer" only below 4 px/cell; before the manifest the card counts the
pieces already held; manifests every eighth of the symbols needed, clamped to [8, 32] states
(was half, [16, 128]).

---

## F60 — The camera and the display share no clock: the speed limit nobody had modelled `[HYP]`

**The gap.** Every simulated goodput in this repository assumed the camera captures each display
state exactly once, whole. A phone camera free-runs at its own rate, and its rolling shutter reads
rows over `readout`, each row exposing for `exposure`. Any row whose exposure window straddles a
state change records a time-weighted mix of two states. This is the `Pc` term of the performance
model, never measured and never simulated.

**Model.** `sim::RenderUnsynced` renders a capture band by band, each band integrating every display
state its exposure window overlaps, weighted by time. `LiveReceiver.DISABLED_UnsynchronisedTimingSweep`
runs the real live receiver on such captures: 200 KB file, 120x200 grid, static geometry, the
camera detuned 0.13% from nominal (at exactly equal or harmonic rates the tear sits on the same
row forever, which made the first version of the sweep say whatever its random phase said), three
phases per arm, decoder assumed to keep up:

| camera | display | exposure | readout | captures torn | payload decoded | goodput |
|--:|--:|--:|--:|--:|--:|--:|
| 30/s | 15/s | 16 ms | 30 ms | 70% | 73% | 29.8 KB/s |
| 30/s | 10/s | 8 ms | 30 ms | 38% | 86% | 19.9 KB/s |
| 30/s | 15/s | 8 ms | 30 ms | 57% | 77% | 29.8 KB/s |
| 30/s | **20/s** | 8 ms | 30 ms | 69% | 59% | **34.7 KB/s** |
| 30/s | 24/s | 8 ms | 30 ms | 90% | 53% | 31.3 KB/s |
| 60/s | 15/s | 8 ms | 15 ms | 37% | 88% | 29.9 KB/s |
| 60/s | 20/s | 8 ms | 15 ms | 47% | 83% | 39.8 KB/s |
| 60/s | 24/s | 8 ms | 15 ms | 52% | 75% | 47.7 KB/s |
| 60/s | **30/s** | 8 ms | 15 ms | 70% | 73% | **59.6 KB/s** |
| 60/s | 40/s | 8 ms | 15 ms | 94% | 63% | 74.1 KB/s |

**What it says** `[HYP]`:
1. The clean-capture bound `Fd <= 1 / (exposure + readout + 1/Fc)` is too pessimistic. RS repairs a
   capture torn near one edge, so goodput keeps rising past it; "torn" counts any capture touching
   two states, however slightly.
2. With a **30 fps** camera goodput peaks near **20 states/s** and falls beyond. The 15 used until
   now left ~15% on the table.
3. A **60 fps** camera roughly doubles the achievable rate — still rising at 40 states/s — provided
   the decoder keeps up, which one core does not (F57).

**Changes.**
- **Decode lanes.** `LiveReceiver` runs `cfg.lanes` independent pipelines that may run on separate
  threads at once, sharing one session behind a lock; only the session and the status are
  serialised. `LiveDecoder` gives each lane a worker thread and its own frame buffer, and a frame
  goes to whichever lane is idle (`LiveReceiver.LanesOnSeparateThreadsCompleteTheSameTransfer`:
  three threads, one verified file). Lanes default to cores − 3, at most 4.
- **The fastest capture rate that keeps the resolution**: the highest of 60/50/30 fps the camera
  offers at ≥ 90% of its 30 fps mode's long side. The Pixel 8: 4032x2268 at 60 against 4080x3072 at
  30. The S26 Ultra offers 60 fps only up to 2336x1080, but **50 fps at 4000x1848** — a first
  version that chose only between 60 and 30 left it at 30.

**Measured** `[FACT]`, no screen in view, 4 lanes: Pixel 8 at 60 fps, ~60 frames decoded/s,
24-28 ms each; S26 Ultra at 50 fps, ~50 decoded/s, 7-9 ms each. Every delivered frame decoded on
both. Rolling-shutter skew reported per frame: **9.7 ms** (Pixel, 4032x2268) and **15.7 ms** (S26,
4000x1848) — inside the 15 ms the sweep assumed for 60 fps, give or take.
- **8 ms exposure** by default (was 16): half the blur, half the mixing; exposure control buys the
  light back with ISO.
- **Speed presets** on the sender: Steady = 120x260 at 20/s (the 30 fps optimum, so it suits any
  camera), Fast = 120x260 at 30/s and Fastest = 180x390 at 30/s (for 60 fps receivers).
- The camera's `SENSOR_ROLLING_SHUTTER_SKEW` is now logged per frame, so the readout term can be
  measured on each phone rather than assumed.

**Not modelled, and next:** a capture whose tear crosses mid-frame is lost whole, because every RS
codeword is interleaved across the entire frame. LightSync (Hu et al., MobiCom 2013) roughly
doubled throughput by decoding such imperfect frames rather than discarding them. The equivalent
here is localised codewords — tiles, each its own fountain symbol with its own check — so a torn
capture still yields the tiles on either side of the tear. That is a protocol change (v3), and it
should wait for a real capture that measures how often mid-frame tears actually happen.

---

## F61 — Fill the picture: 34 KB/s on a 2.2 MB photo, and why the Pixel needed distance `[FACT]` / `[HYP]`

**Measured** `[FACT]` (2026-10-23, Pixel 8 sending at Steady, 120x260 at 20 states/s → S26 Ultra
receiving at 50 fps, hand-held both): a 2.2 MB photo, SHA-256 verified, in **67.0 s from pressing
Receive — 33.9 KB/s wall-clock goodput**, 37 times F59's. From the receiver's log, with the
pattern's size in the picture as camera px/cell:

| px/cell | focus | payload frames/s | note |
| --- | --- | --- | --- |
| 3.4–3.9 | 30–35 cm | ~3 | the screen a third of the picture, as the coach then asked |
| 4.7–6.6 | 22–27 cm | ~25 | |
| 7.1–11.8 | 11–22 cm | **~40** of 50 | the screen filling the picture |

So the coach's target -- "fill about a third of the picture" -- was a tenth of the rate on offer.
F59's failure at 12 cm was the exposure servo driving ISO to its floor, not the distance; this
transfer ran fastest at 11 cm. The guidance is now **fill the picture**, with corner marks in the
pattern's shape over the clear part of the viewfinder; "too close" fires only when the lens is
pinned at its closest focus, and "too far" below 5 px/cell.

**The Pixel read best from far away** (operator report, no log: the Pixel's buffer had rolled
over). Not optics: the two main cameras are nearly the same lens (f 6.9 / 6.5 mm on a 9.79 mm
sensor, closest focus 10.5 / 10 cm). The likely cause `[HYP]` is **lens distortion**. The decoder
maps the screen with one homography, exact through an ideal lens, and a radial bow cannot be
absorbed by it. From each camera's own `LENS_DISTORTION`, for a pattern filling ~85% of the
frame, the long edges sit off the corner-fitted homography by about **10 px ≈ 0.7 cell on the
Pixel** (k = 0.089, −0.162, 0.119) against **~0.35 cell on the S26** (0.130, −0.347, 0.257, whose
terms cancel better at that radius) -- and by a quarter of that, in cells, with the screen a third
of the picture. The Pixel offers `DISTORTION_CORRECTION_MODE` FAST and was never asked for it; it
now is, wherever offered, and the applied mode is logged ("undistort on/off"). The S26 does not
list the control. Confirm with a Pixel receive at fill: payload rate against F61's table.

**Speed and time left.** The time left was the raw 4 s windowed rate, and on this transfer it read
2 min, 4, 6, 12, 9, 12, 9, 6, 2 min, 81 s in the first sixteen seconds. Now `TransferRate`
(core/live): the window stretches to hold ≥ 2% of the file (up to 20 s), the rate is filtered
(τ 2 s), and the time left counts down by the clock and glides to each estimate, rising three
times slower than it falls. Replayed on this transfer's own progress trace, it wanders from a
steady countdown by 270 s against 1293 s, and its worded value ("4 min", "42 s") rises in 1 second
against 12. The card shows that rate as "2.2 MB · 34 KB/s · 0:41", whole KB/s above 10 so the last
digit does not churn, redrawn four times a second.

**Session logs** now also go to `files/logs/` in app storage (newest 20), because installing a
build clears logcat and the Pixel's side of this test was lost that way.

---

## What has NOT been validated

To be explicit, since a working simulator invites over-confidence.

> **Corrected 2026-08-03.** The first two entries in this list had gone stale and were
> understating what exists — the geometric pipeline landed with F9/F10/F13/F17 and payload FEC
> with F18, but this section still said neither existed. That is exactly the
> documentation-versus-implementation divergence RISK-020 tracks and F10 was caught by, so it is
> corrected here rather than quietly rewritten. The superseded text, quoted in full because
> **this working tree is not under version control** and there is no history to recover it from:
>
> > - No geometric pipeline exists yet — no marker detection, homography, rectification or
> >   subpixel sampling. `sim` hands cell samples straight to the demodulator. **The hardest
> >   computer-vision problems are untouched.**
> > - No payload FEC. `Rfec` = 1.0 throughout; EXP-011 has not run.
>
> Both were true when written and both were false by 2026-08-02 (F9/F10/F17, F18).

**What now exists** (so the list below is not read as broader than it is): the full CV path —
detection, homography, tracking, subpixel sampling, photometric field (C06/C06a/C08/C09);
intra-frame FEC with errors-and-erasures decoding (C11, F18); the capture/replay harness proven
bit-identical to live decode (C17, F17); and the C14 estimator and code-rate policy (F20–F23).

**Also now exists (2026-10-23):** protocol v2 and the session layer C13a (ADR-0015, F49–F52) — a
receiver that needs only the grid, an in-band manifest, and `ffreplay` running verified transfers
with goodput and display-state rates. **None of it has met a real capture yet.**

> **Corrected 2026-10-23.** The first bullet below still said *"no transmitter exists, so no
> display state has ever been presented or counted"*. A transmitter has existed since F36 and
> presented the frames whose headers were read. What remained true is that `Fd` was never
> *counted*: nothing tallied distinct sequence numbers per second. `ffreplay` now does, so the next
> capture answers EXP-006. The original wording is kept below, struck, rather than overwritten.

**What still is not validated:**

- **The receiver's camera path is measured; nothing else about the hardware is.**
  *Corrected twice on 2026-08-04*, which is itself the point — this entry has gone stale within
  hours, twice, and the superseded text is kept rather than overwritten:
  - It first read *"No device code at all… the single most important gap"*, false once the C02
    probe ran (F27).
  - It was then rewritten to say frame delivery, distinctness and manual-control obedience were
    *still unmeasured*, and that became false the same day (F28).

  **What is now measured on the S26 Ultra:** CPU-path delivery of **59.04 fps** against a
  requested 60 at 1920×1440, **0 duplicate frames in 600**, and manual exposure / ISO / AE /
  `EDGE_MODE` / `NOISE_REDUCTION_MODE` all reported as requested. A real capture bundle reached
  `ffreplay` (F29).

  **What is still unmeasured, and it is most of what matters:** `Fd` — ~~no transmitter exists, so
  no display state has ever been presented or counted~~ never counted, though now countable
  (EXP-006). The **240 fps high-speed path**,
  which is the one that gates milestone 6 and the one where duplication risk actually sits
  (OQ-002). Whether `EDGE_MODE` OFF *behaves* as off, which needs a known
  high-spatial-frequency target. Rolling-shutter skew (F25). `Pc`, and every optical property of
  the channel. **No goodput figure of any kind has been measured on hardware**, and the probe
  still correctly returns UNSUPPORTED.
- **The channel model is uncalibrated** (RISK-024). Its impairments are plausible, not
  validated. Every simulator number in this document is `[HYP]` for that reason.
- `Pc`, `Fd` and the density cliff — the model's highest-leverage unknowns — remain entirely
  unmeasured. F16 established that the cliff **cannot** be found with the current renderer.
- **No payload code family selected.** Intra-frame FEC exists, but EXP-011 has not run, so RS
  is a working default rather than a justified choice (OQ-009).
- **The GPU sampling path does not exist**, nor does the mandatory CPU/GPU equivalence test
  (C08, EXP-016).
- **C14 cannot actuate** (F20). The decision function is validated; the delivery path is
  blocked by the protocol, and on a one-way link by physics (OQ-013, OQ-037).
- **No thermal policy** (ADP-05, OQ-036): the decode-cost measurements that would justify one
  come from EXP-011 and EXP-020, and neither has run.
