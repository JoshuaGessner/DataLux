# Architecture decision records

> **Status:** Draft
> **Owner:** Architecture
> **Last reviewed:** 2026-08-02

Every decision that shapes the system lives here, not in chat logs or commit messages.

## Format

Each ADR contains: **Context, Decision, Alternatives, Consequences, Evidence, Risks,
Validation plan, Status.**

## Status values

| Status | Meaning |
|---|---|
| **Proposed** | Decided provisionally; evidence is insufficient. Most Phase 0 ADRs are here. |
| **Accepted** | Supported by evidence recorded in the ADR. |
| **Superseded** | Replaced; the superseding ADR is named. Never deleted. |
| **Rejected** | Considered and declined; kept so the reasoning is not relitigated. |

An ADR at **Proposed** is a decision we are acting on while acknowledging we might be
wrong. Each one names the experiment that would confirm or overturn it.

## Index

| ADR | Title | Status | Confidence | Validated by |
|---|---|---|---|---|
| [0001](ADR-0001-android-first.md) | Android-first implementation | Accepted | High | — (scope decision) |
| [0002](ADR-0002-native-app-not-browser.md) | Native installed app, not browser | Accepted | High | Platform capability analysis |
| [0003](ADR-0003-kotlin-shell-cpp-core.md) | Kotlin shell + C++ NDK core | Accepted | High | — (structural) |
| [0004](ADR-0004-gpu-rendered-transmitter.md) | GPU-rendered transmitter | Accepted | Medium-High | EXP-006 |
| [0005](ADR-0005-camera-receive-paths.md) | Dual CPU/GPU camera receive paths | **Proposed** | Medium | EXP-007 |
| [0006](ADR-0006-full-screen-grid-not-qr.md) | Full-screen spatial grid, not QR | **Proposed** | Medium | EXP-001, EXP-002, EXP-017 |
| [0007](ADR-0007-binary-luminance-baseline.md) | Binary luminance as baseline | Accepted | High | EXP-003 |
| [0008](ADR-0008-differential-modulation-path.md) | Differential modulation as primary optimisation path | **Proposed** | Low-Medium | EXP-010 |
| [0009](ADR-0009-intraframe-fec-plus-fountain.md) | Intra-frame FEC + cross-frame fountain | **Proposed** | Medium-High | EXP-011, EXP-012 |
| [0010](ADR-0010-simulator-before-optimization.md) | Offline simulator before optimisation | Accepted | High | — (process) |
| [0011](ADR-0011-reference-device-first.md) | Reference-device-first development | Accepted | High | — (scope decision) |
| [0012](ADR-0012-goodput-primary-metric.md) | Goodput as the primary metric | Accepted | High | [CHROMACODE] evidence |
| [0013](ADR-0013-toolchain-and-build-system.md) | Toolchain, build system and repo layout | **Accepted** | High | Validated 2026-08-03: `core/` cross-compiles for arm64 under NDK r29 unchanged, one `.so`, no warnings |
| [0014](ADR-0014-thin-android-adapters.md) | Thin Android adapters; all judgement in portable C++ | **Accepted** | Medium-High | C02 probe logic tested off-device; F25 found a platform error on the way |
| [0015](ADR-0015-protocol-v2-dense-fountain-inband-manifest.md) | Protocol v2: dense fountain, in-band manifest, self-describing code rate | **Accepted** | High (code) / Medium (gain) | F49 measured the code property directly; goodput gains are `[HYP]` until a real capture completes |

## Note on confidence

ADR-0008 has the lowest confidence of any decision we are acting on. Differential
modulation is a central plank of the prescribed architecture, but we found no primary
evidence for it during Phase 0 research and the theoretical argument cuts both ways.
It is worth reading that ADR before relying on the optimisation path it describes.
