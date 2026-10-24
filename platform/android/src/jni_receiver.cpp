// JNI marshalling for the live receiver (C13b).
//
// ADR-0014: every decision -- what phase the transfer is in, what to tell the person holding the
// phone, whether the file verified -- is made in `core/src/live.cpp`, where the desktop suite
// tests it against simulated handheld motion. This file moves numbers and bytes across the
// boundary and decides nothing.
//
// Same conventions as jni_framing.cpp: the Y plane is read through `GetDirectBufferAddress` (a
// pointer, not a copy, and a non-direct buffer is refused), and per-frame numbers come back in a
// caller-owned `double[]` whose slot layout is asserted on both sides.
#include "jni_util.h"

#include <fileflow/image.h>
#include <fileflow/live.h>
#include <fileflow/result.h>

#include <jni.h>

#include <cstdint>
#include <new>
#include <string>
#include <vector>

using fileflow::android_jni::ThrowIllegalState;

namespace {

// MUST match `NativeReceiver.Slot` in Kotlin; the count is asserted there.
enum Slot : int {
    kPhase = 0,
    kHint,
    kProgress,
    kSecondsRemaining,
    kMotionBlurCells,
    kRecentReadFraction,
    kScreenInView,
    kFrames,
    kFileSize,
    kPayloadFrames,
    kBytesPerSecond,
    kSlotCount,
};

fileflow::LiveReceiver* FromJLong(jlong h) { return reinterpret_cast<fileflow::LiveReceiver*>(h); }

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL
Java_dev_fileflow_rx_NativeReceiver_slotCount(JNIEnv* /*env*/, jclass /*unused*/) {
    return static_cast<jint>(kSlotCount);
}

JNIEXPORT jlong JNICALL
Java_dev_fileflow_rx_NativeReceiver_open(JNIEnv* env, jclass /*unused*/, jint cols, jint rows,
                                         jlong exposure_ns, jint lanes) {
    if (cols <= 0 || rows <= 0) {
        ThrowIllegalState(env, "cols and rows must both be positive");
        return 0;
    }
    fileflow::LiveConfig cfg;
    // The blur threshold is in cells of motion DURING an exposure, so it needs the real exposure.
    if (exposure_ns > 0) cfg.exposure_s = static_cast<double>(exposure_ns) * 1e-9;
    // Lanes the app will drive from separate threads at once (F60); LiveReceiver validates.
    cfg.lanes = lanes > 0 ? static_cast<std::size_t>(lanes) : 1u;
    auto r = fileflow::LiveReceiver::Create(
        fileflow::GridGeometry{static_cast<std::uint32_t>(cols), static_cast<std::uint32_t>(rows)},
        cfg);
    if (!r.ok()) {
        ThrowIllegalState(env, std::string("receiver: ") +
                                   std::string(fileflow::ErrorName(r.error())));
        return 0;
    }
    auto* h = new (std::nothrow) fileflow::LiveReceiver(std::move(r).value());
    if (h == nullptr) {
        ThrowIllegalState(env, "out of memory opening the receiver");
        return 0;
    }
    return reinterpret_cast<jlong>(h);
}

// One Y plane in, the status sentence out, numbers in `out`. An empty string means a marshalling
// failure, distinguishable because `out[kPhase]` is left at -1.
JNIEXPORT jstring JNICALL
Java_dev_fileflow_rx_NativeReceiver_ingest(JNIEnv* env, jclass /*unused*/, jlong handle,
                                           jobject y_plane, jint width, jint height,
                                           jint row_stride, jlong timestamp_ns, jint lane,
                                           jdoubleArray out) {
    if (out != nullptr && env->GetArrayLength(out) >= 1) {
        const jdouble sentinel = -1.0;
        env->SetDoubleArrayRegion(out, kPhase, 1, &sentinel);
    }
    fileflow::LiveReceiver* rx = FromJLong(handle);
    if (rx == nullptr || y_plane == nullptr || out == nullptr) return env->NewStringUTF("");
    if (env->GetArrayLength(out) < static_cast<jsize>(kSlotCount)) return env->NewStringUTF("");
    if (width <= 0 || height <= 0 || row_stride < width) return env->NewStringUTF("");

    auto* base = static_cast<const std::uint8_t*>(env->GetDirectBufferAddress(y_plane));
    if (base == nullptr) return env->NewStringUTF("");
    // Bounds before use (INPUT-VALIDATION): never trust the reported stride against the buffer.
    const std::int64_t needed =
        static_cast<std::int64_t>(row_stride) * static_cast<std::int64_t>(height - 1) + width;
    if (env->GetDirectBufferCapacity(y_plane) < needed) return env->NewStringUTF("");

    const fileflow::ImageView8 img(base, width, height, row_stride);
    // A copy: other lanes may be updating the receiver's status concurrently.
    const fileflow::LiveStatus s = rx->Ingest(img, static_cast<std::int64_t>(timestamp_ns),
                                              lane > 0 ? static_cast<std::size_t>(lane) : 0u);

    jdouble v[kSlotCount];
    v[kPhase] = static_cast<jdouble>(static_cast<int>(s.phase));
    v[kHint] = static_cast<jdouble>(static_cast<int>(s.hint));
    v[kProgress] = s.progress;
    v[kSecondsRemaining] = s.seconds_remaining;
    v[kMotionBlurCells] = s.motion_blur_cells;
    v[kRecentReadFraction] = s.recent_read_fraction;
    v[kScreenInView] = s.screen_in_view ? 1.0 : 0.0;
    v[kFrames] = static_cast<jdouble>(s.frames);
    v[kFileSize] = static_cast<jdouble>(s.file_size);
    v[kPayloadFrames] = static_cast<jdouble>(s.payload_frames);
    v[kBytesPerSecond] = s.bytes_per_second;
    env->SetDoubleArrayRegion(out, 0, static_cast<jsize>(kSlotCount), v);
    return env->NewStringUTF(s.message.c_str());
}

// The manifest's file name: a sanitised DISPLAY HINT, never a path (THREAT-MODEL T3).
JNIEXPORT jstring JNICALL
Java_dev_fileflow_rx_NativeReceiver_fileName(JNIEnv* env, jclass /*unused*/, jlong handle) {
    fileflow::LiveReceiver* rx = FromJLong(handle);
    return env->NewStringUTF(rx != nullptr ? rx->status().file_name.c_str() : "");
}

// The verified file, or null. Never partial or unverified bytes (G3).
JNIEXPORT jbyteArray JNICALL
Java_dev_fileflow_rx_NativeReceiver_fileBytes(JNIEnv* env, jclass /*unused*/, jlong handle) {
    fileflow::LiveReceiver* rx = FromJLong(handle);
    if (rx == nullptr) return nullptr;
    auto f = rx->File();
    if (!f.ok()) return nullptr;
    const std::vector<std::uint8_t>& bytes = f.value();
    if (bytes.size() > static_cast<std::size_t>(INT32_MAX)) return nullptr;
    jbyteArray arr = env->NewByteArray(static_cast<jsize>(bytes.size()));
    if (arr == nullptr) return nullptr;  // OutOfMemoryError is already pending
    env->SetByteArrayRegion(arr, 0, static_cast<jsize>(bytes.size()),
                            reinterpret_cast<const jbyte*>(bytes.data()));
    return arr;
}

JNIEXPORT void JNICALL
Java_dev_fileflow_rx_NativeReceiver_close(JNIEnv* /*env*/, jclass /*unused*/, jlong handle) {
    delete FromJLong(handle);
}

}  // extern "C"
