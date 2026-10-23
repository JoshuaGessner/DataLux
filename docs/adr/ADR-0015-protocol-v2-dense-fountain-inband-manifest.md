# ADR-0015 — Protocol v2: dense fountain, in-band manifest, self-describing code rate

> **Status:** Accepted
> **Date:** 2026-10-23
> **Owner:** Architecture
> **Related:** ADR-0009, ADR-0012, C11, C12, C13, C13a, F4, F20, F49, F50, F52, OQ-010, OQ-037,
> RISK-016

## Context

Three defects, found together in one review, each made the v1 protocol slower or unusable:

1. **The fountain code wasted 20–35% of goodput under any frame loss** (F49). The cause was the
   code — low-degree LT repair rows after a systematic prefix — not the peeling decoder: Gaussian
   elimination on the same rows recovered almost none of it.
2. **No receiver could learn the session.** The manifest (file size, hash, block layout) was never
   transmitted, and the intra-frame code rate was a convention both ends had to share out of
   band. Every receiver in the repository was handed the transmitter's manifest object (F52), so no
   real capture could ever have completed a transfer.
3. **Padding was transmitted** (F4). The zero tail of the last block was sent every cycle: half the
   frames of a 64 KB transfer at 120×200.

None of these can be fixed without changing what is on the wire, so they are fixed together under
one version bump rather than three.

## Decision

**`kProtocolVersion` = 2.** A v2 receiver refuses v1 frames and a v1 receiver refuses v2 frames,
both cleanly, by version. v1 receivers would otherwise decode v2 repair symbols against LT rows and
fail only at the final hash, after the whole transfer.

### Fountain (C12)

- **Systematic random linear fountain over GF(2).** ESI < k is source symbol ESI; every ESI ≥ k is
  a dense row, each source symbol included with probability ½, regenerated from
  `(session_seed << 32) ^ (ESI × 0x9E3779B97F4A7C15)` by `SplitMix64`, 64 bits per word, the
  final word masked to k bits, and an all-zero row replaced by the unit row `ESI mod k`.
- **Decoded by incremental Gaussian elimination**, each symbol reduced on arrival; back-substitution
  once rank reaches k.
- **`kMaxSourceSymbols` = 1024** (was 2²⁰). Decoding is O(k) symbol XORs per repair symbol and a
  k×k bit matrix, so k is now a cost bound as well as a memory bound against a hostile manifest.
- **Blocks: as few and as balanced as the bound allows** (`AutoBlockSymbols`). With no back channel
  a transfer ends when its unluckiest block completes, and that tail shrinks as 1/√k.

### Padding

Source symbols lying entirely past the end of the file (`FirstPaddingSymbol`) are **never
transmitted**. The receiver derives them from the manifest and primes its decoder with them as
known zeros; they count neither as received nor as progress.

### Frame header

- **`fec_nsym` (u8) added after `flags`**: RS parity bytes per intra-frame codeword, 0 = none,
  255 rejected. `kPlainSize` 33 → 34 bytes. The receiver reads the code rate before it reads
  anything else in the frame — including the manifest, whose payload is FEC-protected like any
  other.
- **Flag `0x02` is `kFlagManifest`** (was `kFlagSessionHeaderFollows`, reserved and never set).
- **`payload_crc` covers the first `payload_bytes` bytes** of the frame message. For a symbol that
  is the whole symbol, as before.

### Manifest frames

- One every `manifest_interval` display states, the first at sequence 0. Default
  `clamp(symbols_needed / 2, 16, 128)`: 0.8–6% of display states.
- Payload: `FileManifest::Encode()` in the first `payload_bytes` bytes; the rest of the symbol is
  pseudorandom filler keyed to the sequence number, never zeros (F15, F39).
- A manifest must fit one symbol. `FileTransmitter::Create` refuses a symbol size that cannot hold
  it.
- **The first valid manifest owns the session.** Frames from any other `session_id` are refused,
  including those read before the manifest (THREAT-MODEL T8).
- **Symbols read before the manifest are held and replayed** once it arrives (bounded, 4096 frames
  by default), so a receiver that misses the manifest loses no data — only the wait for the next
  manifest frame, and only if it would otherwise already have finished.

### Session layer (C13a)

`TransmitSession` and `ReceiveSession` are the only implementation of the above, used by `ffsim`,
`ffreplay`, the JNI transmitter and the tests. The receiver is configured with the grid and nothing
else.

## Consequences

- **Faster, by simulation** `[HYP]`: 256 KB at `Fd` = 60, 5–30% frame loss: +29% to +60% verified
  goodput; F18-impaired channel +52%; clean +6%. 64 KB, image path: 2.0× (padding). F49 has the
  tables.
- **A real capture can now complete a transfer**, and `ffreplay` says so with a verified goodput —
  the first measurement milestone 2 has ever been able to produce.
- **The RaptorQ question loses most of its urgency** (OQ-010, RISK-016). Dense GF(2) is within
  ~1.6 symbols of optimal at k ≥ 64; RaptorQ's remaining advantage is decode cost at large k, which
  a 1024-symbol block bound makes moot.
- **OQ-037 is narrowed, not closed.** The code rate is now self-describing per frame, so a receiver
  never needs to be told it — but the fountain symbol size is still the FEC message size, fixed by
  the manifest for the session, so `nsym` still cannot change mid-transfer. Decoupling symbol size
  from frame capacity remains open; this ADR deliberately did not take it on, because on a one-way
  link nothing could yet act on a mid-session change anyway (OQ-013).
- **Cost.** Transmitter: up to k/2 symbol XORs per repair frame — 512 × 3 KB ≈ 1.5 MB at k = 1024,
  well under a millisecond on the render thread. Receiver: comparable per repair symbol, plus a
  k² bit matrix (128 KB at k = 1024).
- **Every v1 capture bundle remains replayable for geometry, photometry and headers**, but its
  headers now fail with `kUnsupportedVersion` and no v1 bundle can complete a v2 transfer. No
  real v1 bundle of a full transfer existed, so nothing of evidential value is lost.

## Alternatives considered

- **Gaussian elimination on the existing LT rows.** Measured: recovers 0–4 points of overhead out of
  20–33 (F49). Rejected — the loss is in the rows.
- **RaptorQ.** Better still at large k, but blocked on RT-07, and unnecessary at the block sizes
  used.
- **Non-systematic LT.** Fixes the repair-row mismatch, but makes a clean channel pay fountain
  overhead it pays nothing for today.
- **Manifest in every frame's payload.** ~3% of every frame, forever, versus a manifest frame that
  is free after the first. Rejected.
- **Manifest at a fixed known `nsym` instead of `fec_nsym` in the header.** Avoids a header change
  but forces data read before the manifest to be buffered *undecoded*, and leaves the code rate a
  shared convention. Rejected.
