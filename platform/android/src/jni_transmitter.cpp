// JNI marshalling for the optical frame generator (C04) driving the Android transmitter (C03).
//
// ADR-0014 again: every decision lives in portable C++ that the desktop suite exercises. This file
// marshals and owns lifetime; it decides nothing. The frame content comes from the SAME
// `TransmitSession` (C13a) that ffsim and the end-to-end tests drive, so a frame presented on a
// phone is the frame `ffsim` would have rendered for the same session -- which is what makes a
// real capture comparable to a simulated one at all. It used to wire the layout, modulator,
// header codec, FEC and fountain together by hand, a second copy of the protocol that had to be
// kept in step with the first; protocol v2 (ADR-0015) is where that would have broken.
//
// ON ADR-0003's "NO PER-FRAME DATA CROSSES JNI". A cell matrix is one byte per CELL, not per pixel:
// 34,560 bytes at 144x240, against 4.5 MB for the panel's 1440x3120. It is written directly into a
// caller-supplied direct `ByteBuffer`, so Kotlin uploads native memory to a GL texture without a
// copy. A non-direct buffer is REFUSED rather than silently copied.
//
// WHY CELLS AND NOT PIXELS. The renderer upscales by an exact integer factor per axis with
// GL_NEAREST, which is bit-exact rather than approximate *because* the cell pitch is required to be
// an integer number of panel pixels (DEVICE-MATRIX). So uploading cells rather than pixels costs
// nothing in fidelity and saves ~130x the bandwidth. If that integer-pitch requirement is ever
// relaxed, this shortcut stops being exact and must be revisited.
#include "jni_util.h"

#include <fileflow/fountain.h>
#include <fileflow/grid.h>
#include <fileflow/hash.h>
#include <fileflow/intra_fec.h>
#include <fileflow/modulation.h>
#include <fileflow/result.h>
#include <fileflow/session.h>

#include <jni.h>

#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <vector>

using fileflow::android_jni::ThrowIllegalState;

namespace {

// One transmit session. The layout is heap-allocated so its address is stable for the lifetime of
// the session, which holds a pointer to it.
struct TxHandle {
    std::unique_ptr<fileflow::FrameLayout> layout;
    std::optional<fileflow::TransmitSession> session;
    fileflow::CellMatrix frame;
    fileflow::GridGeometry geom;
    std::vector<std::uint8_t> payload;
    std::uint32_t last_sequence = 0;

    TxHandle(std::unique_ptr<fileflow::FrameLayout> l, fileflow::GridGeometry g,
             std::vector<std::uint8_t> p)
        : layout(std::move(l)), frame(g.cols, g.rows), geom(g), payload(std::move(p)) {}
};

TxHandle* FromJLong(jlong h) { return reinterpret_cast<TxHandle*>(h); }

}  // namespace

extern "C" {

// Signature matches app/src/main/kotlin/dev/fileflow/tx/NativeTransmitter.kt.
//
// `payload_bytes` is generated deterministically from `seed` rather than passed in: a transmit run
// needs a reproducible payload, and shipping the bytes across JNI to produce them here would be
// pointless. The receiver checks the SHA-256 the in-band manifest carries (ADR-0015), so the
// payload only has to be reproducible on the sending side.
JNIEXPORT jlong JNICALL
Java_dev_fileflow_tx_NativeTransmitter_open(JNIEnv* env, jclass /*unused*/, jint cols, jint rows,
                                           jint nsym, jint payload_bytes, jlong seed) {
    if (cols <= 0 || rows <= 0) {
        ThrowIllegalState(env, "cols and rows must both be positive");
        return 0;
    }
    if (nsym < 0 || nsym >= 255) {
        ThrowIllegalState(env, "nsym must be in [0, 255)");
        return 0;
    }

    const fileflow::GridGeometry geom{static_cast<std::uint32_t>(cols),
                                      static_cast<std::uint32_t>(rows)};
    auto layout_r = fileflow::FrameLayout::Create(geom, fileflow::LayoutConfig{});
    if (!layout_r.ok()) {
        ThrowIllegalState(env, std::string("layout: ") +
                                   std::string(fileflow::ErrorName(layout_r.error())));
        return 0;
    }
    auto layout = std::make_unique<fileflow::FrameLayout>(std::move(layout_r).value());

    const fileflow::M0Modulator probe_mod(*layout);
    const fileflow::HeaderCodec probe_hdr;
    // Said plainly here because TransmitSession can only say kValueOutOfRange, and a grid too small
    // for its own header is the one configuration mistake an operator actually makes.
    if (probe_mod.header_capacity_bytes() < probe_hdr.coded_size()) {
        ThrowIllegalState(env, "grid " + std::to_string(cols) + "x" + std::to_string(rows) +
                                   " is too small for its coded header: band holds " +
                                   std::to_string(probe_mod.header_capacity_bytes()) +
                                   " bytes, header needs " +
                                   std::to_string(probe_hdr.coded_size()));
        return 0;
    }

    // `payload_bytes <= 0` keeps the old default size -- 64 symbols' worth -- so runs stay
    // comparable with captures taken before protocol v2. It no longer needs to "fill the block":
    // v1 sent the zero padding past the end of the file, which rendered as near-black frames and
    // confounded every optical measurement across them (F15, F39). v2 never transmits padding at
    // all (ADR-0015), so any size is safe.
    std::size_t symbol_size = probe_mod.payload_capacity_bytes();
    if (nsym > 0) {
        fileflow::IntraFecParams fp;
        fp.nsym = static_cast<std::size_t>(nsym);
        auto f = fileflow::IntraFec::Create(probe_mod.payload_capacity_bytes(), fp);
        if (!f.ok()) {
            ThrowIllegalState(env, std::string("intra-frame FEC: ") +
                                       std::string(fileflow::ErrorName(f.error())));
            return 0;
        }
        symbol_size = f.value().message_bytes();
    }
    const std::size_t want = payload_bytes > 0 ? static_cast<std::size_t>(payload_bytes)
                                               : symbol_size * 64u;
    if (want == 0) {
        ThrowIllegalState(env, "this grid carries no payload");
        return 0;
    }
    std::vector<std::uint8_t> payload(want);
    fileflow::SplitMix64 prng(static_cast<std::uint64_t>(seed) ^ 0xF11EF10ULL);
    for (auto& b : payload) b = static_cast<std::uint8_t>(prng.Next() & 0xFF);

    auto* handle = new (std::nothrow) TxHandle(std::move(layout), geom, std::move(payload));
    if (handle == nullptr) {
        ThrowIllegalState(env, "out of memory opening the transmitter");
        return 0;
    }

    fileflow::TransmitConfig tc;
    tc.fec_nsym = static_cast<std::size_t>(nsym);
    auto tx_r = fileflow::TransmitSession::Create(*handle->layout, handle->payload, "tx.bin",
                                                  0x51533101, tc);
    if (!tx_r.ok()) {
        delete handle;
        ThrowIllegalState(env, std::string("transmitter: ") +
                                   std::string(fileflow::ErrorName(tx_r.error())));
        return 0;
    }
    handle->session.emplace(std::move(tx_r).value());
    return reinterpret_cast<jlong>(handle);
}

// Renders the next display state into `dst` as one byte per cell, row-major.
//
// Returns 0 on success or a `fileflow::Error` ordinal. Not an exception: this is the per-frame path
// and it runs on the render thread, where throwing per frame would be both slow and wrong.
JNIEXPORT jint JNICALL
Java_dev_fileflow_tx_NativeTransmitter_nextFrame(JNIEnv* env, jclass /*unused*/, jlong handle,
                                                 jobject dst) {
    TxHandle* h = FromJLong(handle);
    if (h == nullptr || dst == nullptr || !h->session.has_value()) {
        return static_cast<jint>(fileflow::Error::kInternal);
    }

    auto* out = static_cast<std::uint8_t*>(env->GetDirectBufferAddress(dst));
    if (out == nullptr) return static_cast<jint>(fileflow::Error::kInternal);

    const std::size_t need = static_cast<std::size_t>(h->geom.cols) * h->geom.rows;
    if (env->GetDirectBufferCapacity(dst) < static_cast<jlong>(need)) {
        return static_cast<jint>(fileflow::Error::kTruncated);
    }

    auto header = h->session->Next(&h->frame);
    if (!header.ok()) return static_cast<jint>(header.error());
    h->last_sequence = header.value().sequence;

    // CellMatrix already stores one byte per cell in row-major order, which is exactly the texture
    // layout the renderer wants, so this is a straight copy rather than a transform.
    std::memcpy(out, h->frame.data().data(), need);
    return 0;
}

JNIEXPORT jint JNICALL
Java_dev_fileflow_tx_NativeTransmitter_lastSequence(JNIEnv* /*env*/, jclass /*unused*/,
                                                    jlong handle) {
    TxHandle* h = FromJLong(handle);
    return h != nullptr ? static_cast<jint>(h->last_sequence) : -1;
}

// The payload hash the receiver must reproduce. Without it a completed transfer proves nothing,
// because "the bytes arrived" and "the right bytes arrived" are different claims (G3).
JNIEXPORT jstring JNICALL
Java_dev_fileflow_tx_NativeTransmitter_payloadSha256(JNIEnv* env, jclass /*unused*/, jlong handle) {
    TxHandle* h = FromJLong(handle);
    if (h == nullptr) return env->NewStringUTF("");
    const std::string hex = fileflow::ToHex(fileflow::Sha256::Of(h->payload));
    return env->NewStringUTF(hex.c_str());
}

JNIEXPORT jint JNICALL
Java_dev_fileflow_tx_NativeTransmitter_payloadCells(JNIEnv* /*env*/, jclass /*unused*/,
                                                    jlong handle) {
    TxHandle* h = FromJLong(handle);
    return h != nullptr ? static_cast<jint>(h->layout->payload_cells().size()) : -1;
}

JNIEXPORT void JNICALL
Java_dev_fileflow_tx_NativeTransmitter_close(JNIEnv* /*env*/, jclass /*unused*/, jlong handle) {
    delete FromJLong(handle);
}

}  // extern "C"
