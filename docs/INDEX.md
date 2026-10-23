# Documentation index

> **Status:** Current
> **Owner:** C20 / documentation
> **Last reviewed:** 2026-08-03

Every document in this repository. Keep this current whenever files are added — see
[CONTRIBUTING.md](../CONTRIBUTING.md).

For purpose, ownership, status and relationships, see
[DOCUMENT-MAP.md](DOCUMENT-MAP.md).

**Handing this project to a fresh agent session?** [HANDOFF.md](HANDOFF.md) is a
self-contained prompt covering the operating rules, build commands, current state, and the
findings that would otherwise have to be rediscovered.

---

## Start here

| Order | Document | Why |
|---|---|---|
| 1 | [vision/PROJECT-VISION.md](vision/PROJECT-VISION.md) | What this is and why it might work |
| 2 | [vision/TERMINOLOGY.md](vision/TERMINOLOGY.md) | Read early — the vocabulary is load-bearing |
| 3 | [vision/PERFORMANCE-PHILOSOPHY.md](vision/PERFORMANCE-PHILOSOPHY.md) | The one metric, and why we are strict about it |
| 4 | [architecture/ARCHITECTURE-OVERVIEW.md](architecture/ARCHITECTURE-OVERVIEW.md) | System shape |
| 5 | [planning/ROADMAP.md](planning/ROADMAP.md) | What happens in what order |
| 6 | [experiments/FIRST-THREE-EXPERIMENTS.md](experiments/FIRST-THREE-EXPERIMENTS.md) | The immediate work |

---

## Root

| Document | Purpose |
|---|---|
| [../README.md](../README.md) | Project overview and entry point |
| [../CONTRIBUTING.md](../CONTRIBUTING.md) | Working rules, evidence tags, experiment workflow |

## Vision

| Document | Purpose |
|---|---|
| [vision/PROJECT-VISION.md](vision/PROJECT-VISION.md) | Problem, thesis, success criteria, where the target is strained |
| [vision/GOALS-AND-NON-GOALS.md](vision/GOALS-AND-NON-GOALS.md) | G1–G7, NG1–NG10, deferred decisions |
| [vision/TERMINOLOGY.md](vision/TERMINOLOGY.md) | Rates, optical structure, channel, coding, model variables |
| [vision/PERFORMANCE-PHILOSOPHY.md](vision/PERFORMANCE-PHILOSOPHY.md) | Goodput definition, six-rate discipline, reporting rules |

## Research

| Document | Purpose |
|---|---|
| [research/RESEARCH-PLAN.md](research/RESEARCH-PLAN.md) | Method, area status, priorities, research tasks RT-01…RT-08 |
| [research/BIBLIOGRAPHY.md](research/BIBLIOGRAPHY.md) | Annotated sources with mandatory access-level tags |
| [research/PRIOR-ART-MATRIX.md](research/PRIOR-ART-MATRIX.md) | System-by-system comparison; gaps explicitly marked |
| [research/android-display-pipeline.md](research/android-display-pipeline.md) | Refresh, Choreographer, FrameTimeline, VRR, presentation verification |
| [research/android-camera-pipeline.md](research/android-camera-pipeline.md) | **Contains the high-speed-session constraint finding** |
| [research/coding-theory.md](research/coding-theory.md) | Channel framing, constellations, soft decisions, code families, fountain |
| [research/fec-library-evaluation.md](research/fec-library-evaluation.md) | Candidate libraries, licensing, selection criteria |
| [research/computer-vision.md](research/computer-vision.md) | Detection, homography, tracking, sampling, photometric normalisation |
| [research/PAPER-ACCESS.md](research/PAPER-ACCESS.md) | How to obtain primary sources legally; what ACM/IEEE are |

## Architecture

| Document | Purpose |
|---|---|
| [architecture/ARCHITECTURE-OVERVIEW.md](architecture/ARCHITECTURE-OVERVIEW.md) | Layering, threading, memory, key interfaces |
| [architecture/DATA-FLOW.md](architecture/DATA-FLOW.md) | End-to-end flow, erasure propagation, simulation paths |
| [architecture/COMPONENT-REGISTRY.md](architecture/COMPONENT-REGISTRY.md) | C01–C20 with full specifications |
| [architecture/ENGINEERING-PRACTICES.md](architecture/ENGINEERING-PRACTICES.md) | Toolchain, repo layout, testing, memory safety, CI, open decisions D1–D5 |

## Architecture decision records

| Document | Purpose |
|---|---|
| [adr/README.md](adr/README.md) | ADR index, statuses and confidence |
| [adr/ADR-0001-android-first.md](adr/ADR-0001-android-first.md) | Android-first |
| [adr/ADR-0002-native-app-not-browser.md](adr/ADR-0002-native-app-not-browser.md) | Native, not browser |
| [adr/ADR-0003-kotlin-shell-cpp-core.md](adr/ADR-0003-kotlin-shell-cpp-core.md) | Kotlin shell + C++ core |
| [adr/ADR-0004-gpu-rendered-transmitter.md](adr/ADR-0004-gpu-rendered-transmitter.md) | GPU transmitter |
| [adr/ADR-0005-camera-receive-paths.md](adr/ADR-0005-camera-receive-paths.md) | Dual CPU/GPU receive paths |
| [adr/ADR-0006-full-screen-grid-not-qr.md](adr/ADR-0006-full-screen-grid-not-qr.md) | Full-screen grid, not QR |
| [adr/ADR-0007-binary-luminance-baseline.md](adr/ADR-0007-binary-luminance-baseline.md) | Binary luminance baseline |
| [adr/ADR-0008-differential-modulation-path.md](adr/ADR-0008-differential-modulation-path.md) | Differential modulation — **lowest confidence** |
| [adr/ADR-0009-intraframe-fec-plus-fountain.md](adr/ADR-0009-intraframe-fec-plus-fountain.md) | Intra-frame FEC + fountain |
| [adr/ADR-0010-simulator-before-optimization.md](adr/ADR-0010-simulator-before-optimization.md) | Simulator before optimisation |
| [adr/ADR-0011-reference-device-first.md](adr/ADR-0011-reference-device-first.md) | Reference-device-first |
| [adr/ADR-0012-goodput-primary-metric.md](adr/ADR-0012-goodput-primary-metric.md) | Goodput as the metric |
| [adr/ADR-0013-toolchain-and-build-system.md](adr/ADR-0013-toolchain-and-build-system.md) | Toolchain, build system, repo layout |
| [adr/ADR-0014-thin-android-adapters.md](adr/ADR-0014-thin-android-adapters.md) | Thin Android adapters; all judgement in portable C++ |
| [adr/ADR-0015-protocol-v2-dense-fountain-inband-manifest.md](adr/ADR-0015-protocol-v2-dense-fountain-inband-manifest.md) | Protocol v2: dense fountain, in-band manifest, self-describing code rate |

## Specifications

| Document | Purpose |
|---|---|
| [specifications/OPTICAL-FRAME-CANDIDATES.md](specifications/OPTICAL-FRAME-CANDIDATES.md) | Three candidate layouts, comparison, sweep plan, header format |
| [specifications/MODULATION-SPEC.md](specifications/MODULATION-SPEC.md) | M0–M4 staged modes with enable/fallback conditions |
| [specifications/PROTOCOL-SPEC.md](specifications/PROTOCOL-SPEC.md) | Six-layer stack, session header, versioning, bounds |
| [specifications/PERFORMANCE-MODEL.md](specifications/PERFORMANCE-MODEL.md) | First-order model, scenarios, sensitivity, provenance |

## Experiments

| Document | Purpose |
|---|---|
| [experiments/EXPERIMENT-REGISTRY.md](experiments/EXPERIMENT-REGISTRY.md) | EXP-001…EXP-023 with hypotheses and thresholds |
| [experiments/OPEN-QUESTIONS.md](experiments/OPEN-QUESTIONS.md) | OQ-001…OQ-033 with severity and resolution path |
| [experiments/FIRST-THREE-EXPERIMENTS.md](experiments/FIRST-THREE-EXPERIMENTS.md) | Recommended immediate work with rationale |
| [experiments/PHASE1-FINDINGS.md](experiments/PHASE1-FINDINGS.md) | **F1–F25 — the project's memory. Design defects, one retraction (F14), every simulated rate, and what the first real CI run and the first real SDK found** |

## Testing

| Document | Purpose |
|---|---|
| [testing/SIMULATOR-PLAN.md](testing/SIMULATOR-PLAN.md) | Impairment pipeline, run configs, outputs, self-validation |
| [testing/CAPTURE-HARNESS.md](testing/CAPTURE-HARNESS.md) | Recording format, required metadata, regression use |
| [testing/BENCHMARK-METHODOLOGY.md](testing/BENCHMARK-METHODOLOGY.md) | Metrics, categories, test files, milestone acceptance |

## Planning

| Document | Purpose |
|---|---|
| [planning/ROADMAP.md](planning/ROADMAP.md) | Phases 0–10 with entry/exit criteria and milestone confidence |
| [planning/FEATURE-REGISTRY.md](planning/FEATURE-REGISTRY.md) | Feature registry across 16 ID groups |
| [planning/RISK-REGISTER.md](planning/RISK-REGISTER.md) | RISK-001…RISK-025 with triggers and owners |
| [planning/DEVICE-MATRIX.md](planning/DEVICE-MATRIX.md) | **Pixel 8 + Galaxy S26 Ultra: specs, per-device grids, capabilities to verify** |

## Security

| Document | Purpose |
|---|---|
| [security/THREAT-MODEL.md](security/THREAT-MODEL.md) | Trust boundaries, actors, T1–T10, security requirements |
| [security/INPUT-VALIDATION.md](security/INPUT-VALIDATION.md) | Bounds table, integer discipline, parser rules, review checklist |

## Tools and data

| Path | Purpose |
|---|---|
| `tools/perf_model/perf_model.py` | Reproducible goodput model — generates the tables in PERFORMANCE-MODEL.md |
| `tools/grid_sweep.sh` | Grid-density sweep through the image path — simulator dry run of EXP-001 (F16) |
| `tools/grid_fit.py` | Fits the grid-sweep response surface |
| `tools/adapt_sweep.sh` | Code-rate ladder sweep plus the C14 controller's pick — reproduces EXP-023 (F20–F23) |
| `tools/android_capture.sh` | Runs one on-device capture and reports what the camera **delivered** — EXP-007's verification half (F28). The `write` arm is the A/B that separates camera rate from writer I/O |
| `tools/frame_cadence.py` | Modal-interval analysis of a capture's per-frame timestamps. Distinguishes a slow sensor from a fast one whose frames are dropped (F28) |
| `tools/android_transmit.sh` | Runs one on-device transmit and reports states **submitted** (an upper bound on `Fd`, never `Fd` itself) |
| `tools/android_probe.sh` · `tools/android_link.sh` · `tools/android_aim.sh` | Probe one device; run a two-device link; fast (~20 s) aiming iteration |
| `tools/android_pull_bundle.sh` | Pulls a capture bundle off a device and decodes it through `ffreplay` |
| `tools/verify_tx_screenshot.py` | Asserts a transmitter screenshot really contains an optical frame — integer pitch, bright ring, two-level interior, exact `GL_NEAREST` upscale (F31) |
| `tools/frame_framing.py` | Rig diagnostic: clipping, inferred rotation, exposure balance and px/cell from one captured frame (F33) |
| `tools/check_links.py` | Link checker for every relative link in the docs tree |
| `data/experiments/` | Raw and processed experimental results. **Append-only** |
| [`data/experiments/EXP-023/RESULTS.md`](../data/experiments/EXP-023/RESULTS.md) | EXP-023 conclusions: C14 code-rate selection, and the cost of guessing wrong |

---

## Registry quick reference

| Registry | Location | Count |
|---|---|---|
| Components | [architecture/COMPONENT-REGISTRY.md](architecture/COMPONENT-REGISTRY.md) | C01–C20 |
| Features | [planning/FEATURE-REGISTRY.md](planning/FEATURE-REGISTRY.md) | 16 ID groups |
| Experiments | [experiments/EXPERIMENT-REGISTRY.md](experiments/EXPERIMENT-REGISTRY.md) | EXP-001–023 |
| Open questions | [experiments/OPEN-QUESTIONS.md](experiments/OPEN-QUESTIONS.md) | OQ-001–037 |
| Risks | [planning/RISK-REGISTER.md](planning/RISK-REGISTER.md) | RISK-001–025 |
| ADRs | [adr/README.md](adr/README.md) | ADR-0001–0014 |
| Findings | [experiments/PHASE1-FINDINGS.md](experiments/PHASE1-FINDINGS.md) | F1–F25 |
| Research tasks | [research/RESEARCH-PLAN.md](research/RESEARCH-PLAN.md) | RT-01–RT-08 |
