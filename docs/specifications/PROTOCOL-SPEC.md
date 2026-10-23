# Protocol layer specification

> **Status:** Draft
> **Owner:** Protocol subsystem
> **Last reviewed:** 2026-10-23
> **Related:** ADR-0009, ADR-0015, OPTICAL-FRAME-CANDIDATES.md, INPUT-VALIDATION.md

## Layer stack

```
┌──────────────────────────────────────────────────────────┐
│ FILE          original file, manifest, hash, safe write  │
├──────────────────────────────────────────────────────────┤
│ SESSION       capabilities, metadata, state, completion  │
├──────────────────────────────────────────────────────────┤
│ FOUNTAIN      cross-frame erasure recovery (rateless)    │
├──────────────────────────────────────────────────────────┤
│ FEC           intra-frame correction, erasure handling   │
├──────────────────────────────────────────────────────────┤
│ OPTICAL FRAME header, payload symbols, pilots, seq, CRC  │
├──────────────────────────────────────────────────────────┤
│ OPTICAL PHY   rendering, capture, geometry, timing       │
└──────────────────────────────────────────────────────────┘
```

Each layer is independently testable with deterministic vectors (C18).

---

## Optical physical layer

Not a byte protocol. Defines: grid geometry, cell pitch, marker patterns, timing tracks,
presentation timing, capture configuration, and the geometric/photometric mapping between
display and camera. Specified in
[OPTICAL-FRAME-CANDIDATES.md](OPTICAL-FRAME-CANDIDATES.md) and
[MODULATION-SPEC.md](MODULATION-SPEC.md).

**Output to the layer above:** soft symbols (LLRs) plus erasure flags plus frame metadata.

---

## Optical frame layer

One optical frame = one display state. Structure per
[OPTICAL-FRAME-CANDIDATES.md](OPTICAL-FRAME-CANDIDATES.md#header-contents).

**Invariants:**
- Every frame is self-describing: sequence number and phase indicator always present.
- Header is M0-modulated, low code rate, replicated, CRC-32 protected, regardless of the
  payload modulation in use.
- Header decode failure ⇒ the entire frame is an erasure. `H` in the goodput model is
  exactly this probability.

---

## FEC layer

**Input:** LLRs + erasure flags for the payload region.
**Output:** packets + per-packet status, or a frame-level erasure.

- Spatial interleaving so that adjacent cells belong to different codewords (clustered
  damage is the expected failure mode — see [coding-theory.md](../research/coding-theory.md)).
- Soft input throughout. Any stage that hard-decides destroys information permanently.
- A **CRC above the FEC layer** catches miscorrection — a code can "successfully" decode to
  the wrong codeword, and without a CRC that corruption flows into the file.
- Uncorrectable ⇒ erasure signalled upward. **Never** pass through best-effort data.

Code selection pending EXP-011. Provisional: LDPC payload, RS/BCH header.

---

## Fountain layer

**Input:** packets with block ID and encoding symbol ID.
**Output:** recovered source blocks; completion signal.

| Property | Requirement |
|---|---|
| Systematic | Source symbols transmitted first, repair symbols after. A clean channel then decodes with near-zero fountain work. |
| Rateless | Transmitter emits repair symbols indefinitely; completion is receiver-determined. |
| Duplicate tolerance | Duplicates ignored idempotently, never corrupt state. |
| Out-of-order tolerance | Arrival order irrelevant by construction. |
| Bounded memory | Block size negotiated and **validated against a hard limit before allocation**. |
| Progress reporting | Receiver knows its own completion fraction per block. |

Blocking: the file is partitioned into blocks sized so the decoder working set stays
bounded regardless of file size. A 100 MB transfer must not need 100 MB of decoder memory.

~~Scheme undecided — EXP-012 and legal review (RT-07).~~ **Decided in protocol v2 (ADR-0015):**

| Property | v2 |
|---|---|
| Code | Systematic random linear fountain over GF(2). ESI < k is source symbol ESI; ESI ≥ k is a dense row, each source symbol in with probability ½, from `SplitMix64((session_seed << 32) ^ ESI·0x9E3779B97F4A7C15)` 64 bits per word, final word masked to k bits, an all-zero row replaced by unit row `ESI mod k` |
| Decoder | Incremental Gaussian elimination over GF(2); ~1.6 symbols over k at any loss rate |
| Block size | k ≤ 1024 (a cost bound as well as a memory bound); as few, balanced blocks as fit (`AutoBlockSymbols`) |
| Session seed per block | `session_id XOR block_index` |
| Schedule | Round-robin over blocks; per block, ESIs ascending |
| Padding | Source symbols wholly past end-of-file are **never sent**; the receiver primes them as zeros from the manifest |

---

## Session layer

Carries everything needed to interpret the transfer, repeated periodically so that a
receiver joining late — or recovering from a long tracking loss — can still synchronise
without restarting.

### As implemented in protocol v2 (ADR-0015)

The full session header below is the long-term design. What is **on the wire today** is smaller,
and it is what a receiver actually depends on:

- **Frame header** (34 bytes before RS, every frame): magic, version (= 2), session ID, sequence,
  profile, phase, block ID, ESI, payload bytes, flags, **`fec_nsym`**, payload CRC-32, header
  CRC-32. `fec_nsym` is the intra-frame code rate, so the receiver reads it before anything else in
  the frame. Flag `0x01` = last block, flag **`0x02` = manifest frame**. The payload CRC covers
  the first `payload_bytes` bytes of the frame message.
- **Manifest frame** (every `manifest_interval` states, first at sequence 0; default
  `clamp(symbols_needed / 2, 16, 128)`): payload is `FileManifest::Encode()` — version, session
  ID, file size, SHA-256, block count, symbols per block, symbol size, sanitised name, extension
  length + skipped extensions, CRC-32 — followed by pseudorandom filler, never zeros.
- **Receiver rules.** The first valid manifest owns the session; frames with any other session ID
  are refused. Symbols read before the manifest are held (bounded) and replayed when it arrives.
  A manifest frame is never passed to the fountain layer.

### Session header (repeated every N frames)

| Field | Size | Notes |
|---|---|---|
| Protocol version | 8 bits | Major version; incompatible change ⇒ refuse |
| Session ID | 32 bits | Random per session; distinguishes adjacent transmitters |
| File size | 64 bits | **Validated against available storage and a hard max before allocation** |
| File name length | 8 bits | Bounded |
| File name | variable | **Sanitised**: no path separators, no traversal, no control characters, length-bounded — see INPUT-VALIDATION.md |
| MIME/type hint | 16 bits | Advisory only, never trusted for execution decisions |
| SHA-256 of file | 256 bits | The integrity anchor |
| Block count | 32 bits | Bounded |
| Block size | 32 bits | **Bounded — drives decoder allocation** |
| Symbol size | 16 bits | Bounded |
| FEC parameters | 32 bits | Code, rate, interleaver seed |
| Modulation profile | 8 bits | Current profile |
| Capability flags | 32 bits | Optional features (encryption, signatures, compression) |
| Extension length | 16 bits | Forward compatibility |
| Extensions | variable | TLV; **unknown extensions skipped, never rejected** |
| CRC-32 | 32 bits | Over the session header |

### Session states

```
IDLE → ANNOUNCING → TRANSFERRING → COMPLETING → VERIFIED
                          ↓              ↓
                       ABORTED ←─────────┘
```

The receiver never signals in the initial one-way design (NG9). `COMPLETING` is a
receiver-local state: it has all blocks and is verifying. The transmitter cannot know this
and simply continues emitting repair symbols until the user stops it or a time limit is
reached — a real inefficiency at the transfer tail, and one of the strongest arguments for
reverse optical control (Phase 9).

### Capability negotiation without a reverse channel

The transmitter cannot learn the receiver's capabilities. It therefore advertises its own
in the session header and transmits using a profile schedule; the receiver decodes what it
can. This is the honest consequence of a one-way link and is tracked as OQ-013.

---

## File layer

**Responsibilities:** manifest handling, safe reconstruction, hash verification.

**Rules:**
1. Data streams to a temporary file with a randomised name, created with `O_EXCL` in an
   app-private directory. Never a predictable path, never a shared directory.
2. The output file is only moved into place **after** hash verification succeeds.
3. Hash mismatch ⇒ hard failure, temporary file deleted, **nothing delivered**.
4. Declared file size is validated against available storage before the transfer starts and
   enforced during writing — a stream that exceeds its declared length is a protocol
   violation and aborts.
5. File names are sanitised, not merely validated. See INPUT-VALIDATION.md.
6. Compression, if used, is applied **above** this layer and its gains reported separately
   (NG8). Decompression is bounded against decompression bombs.

---

## Versioning and forward compatibility

| Mechanism | Rule |
|---|---|
| Major version | Incompatible change. A receiver seeing a higher major version refuses cleanly with a clear message rather than misparsing. |
| Minor version | Compatible additions. Older receivers ignore what they do not understand. |
| Extension fields | TLV. **Unknown types are skipped by length, never treated as errors.** This is what makes forward compatibility actually work. |
| Reserved fields | Must be transmitted as zero and **not validated as zero** on receive — otherwise future use breaks old receivers. |
| Profile IDs | Registry in this document; unknown profile ⇒ frame skipped, not session aborted. |

The "skip, do not fail" discipline is deliberate: a protocol that rejects unknown fields
cannot be extended without breaking every deployed receiver.

---

## Deterministic test vectors

Generated by C18, committed to the repository, covering each layer:

| Layer | Vector content |
|---|---|
| Optical frame | Bits → exact expected cell matrix (golden frames) |
| FEC | Known input + known error pattern → expected output and status |
| Fountain | Symbol sequence with defined losses → expected recovered blocks |
| Session | Header bytes → parsed structure, **including malformed cases that must be rejected** |
| File | Blocks + manifest → expected file and hash result |

Vectors must be **cross-platform deterministic**. Any floating-point in the decode path
that affects vector output is a determinism hazard and needs either fixed-point or a
documented tolerance.

Malformed-input vectors are as important as valid ones and are the seed corpus for fuzzing.

---

## Bounds summary

Every one of these is checked **before** any allocation or loop bound derived from it.
Full rationale in [INPUT-VALIDATION.md](../security/INPUT-VALIDATION.md).

| Quantity | Bound |
|---|---|
| File size | Hard maximum; also checked against free storage |
| File name length | Bounded, sanitised |
| Block count / block size / symbol size | Bounded; product checked for overflow |
| Payload length per frame | Bounded by grid geometry |
| Extension field length | Bounded; sum bounded |
| Fountain decoder memory | Derived from validated block parameters, capped |
| Session duration / frame count | Capped to prevent unbounded resource use |
