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
#include <sched.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
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
    kDistinctStates,
    kColorSpread,
    kColorNoise,
    kTransferSeconds,
    kBitsPerCell,
    kOriginalSize,
    kCodec,
    kCompressMs,
    kCombinedFrames,
    kSlotCount,
};

fileflow::LiveReceiver* FromJLong(jlong h) { return reinterpret_cast<fileflow::LiveReceiver*>(h); }

// One chroma plane as Camera2 hands it over -- usually interleaved with the other (pixel stride 2)
// -- copied into a packed half-size plane the decoder can address. False when the buffer is too
// short for the size and strides it claims (INPUT-VALIDATION: never trust them against the buffer).
bool Unpack(JNIEnv* env, jobject plane, int w, int h, jint row_stride, jint pixel_stride,
            std::vector<std::uint8_t>* out) {
    if (plane == nullptr || w <= 0 || h <= 0 || pixel_stride < 1 ||
        row_stride < (w - 1) * pixel_stride + 1) {
        return false;
    }
    const auto* base = static_cast<const std::uint8_t*>(env->GetDirectBufferAddress(plane));
    if (base == nullptr) return false;
    const std::int64_t needed = static_cast<std::int64_t>(row_stride) * (h - 1) +
                                static_cast<std::int64_t>(pixel_stride) * (w - 1) + 1;
    if (env->GetDirectBufferCapacity(plane) < needed) return false;
    out->resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* row = base + static_cast<std::ptrdiff_t>(y) * row_stride;
        std::uint8_t* dst = out->data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
        if (pixel_stride == 1) {
            std::memcpy(dst, row, static_cast<std::size_t>(w));
        } else {
            for (int x = 0; x < w; ++x) dst[x] = row[static_cast<std::ptrdiff_t>(x) * pixel_stride];
        }
    }
    return true;
}

// One number from a sysfs file, or -1.
long ReadSysfs(const char* path) {
    std::FILE* f = std::fopen(path, "re");
    if (f == nullptr) return -1;
    long v = -1;
    if (std::fscanf(f, "%ld", &v) != 1) v = -1;
    std::fclose(f);
    return v;
}

// The cores worth decoding on: those the kernel rates at least half as capable as its best
// (cpu_capacity, or the top clock where that is missing). Empty when every core qualifies --
// then there is nothing to avoid -- or when nothing can be read.
//
// A Pixel 8's four little cores rate 182 of 1024 and decode a frame in ~190 ms against ~33 ms on
// its mid and big cores; left to the scheduler, decode lanes landed on them often enough to
// average 110 ms a frame and miss a third of the sender's frames. The Galaxy (SM-S948U1) rates
// its slowest cores 741 of 1024, so it is left alone.
std::vector<int> FastCores() {
    const long n = sysconf(_SC_NPROCESSORS_CONF);
    if (n <= 1 || n > CPU_SETSIZE) return {};
    std::vector<long> cap(static_cast<std::size_t>(n), -1);
    long best = 0;
    for (long i = 0; i < n; ++i) {
        char path[96];
        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%ld/cpu_capacity", i);
        long v = ReadSysfs(path);
        if (v <= 0) {
            std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_max_freq", i);
            v = ReadSysfs(path);
        }
        cap[static_cast<std::size_t>(i)] = v;
        if (v > best) best = v;
    }
    if (best <= 0) return {};
    std::vector<int> fast;
    for (long i = 0; i < n; ++i) {
        if (cap[static_cast<std::size_t>(i)] * 2 >= best) fast.push_back(static_cast<int>(i));
    }
    if (fast.size() == static_cast<std::size_t>(n)) return {};
    return fast;
}

}  // namespace

extern "C" {

// Confines the CALLING thread to the fast cores; returns how many that is, 0 when it was left
// alone (every core is fast, or the kernel would not say). A decode lane calls it once, first.
JNIEXPORT jint JNICALL
Java_dev_fileflow_rx_NativeReceiver_pinToFastCores(JNIEnv* /*env*/, jclass /*unused*/) {
    static const std::vector<int> fast = FastCores();
    if (fast.empty()) return 0;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (const int c : fast) CPU_SET(c, &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0) return 0;
    return static_cast<jint>(fast.size());
}

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
                                           jint row_stride, jobject u_plane, jobject v_plane,
                                           jint chroma_row_stride, jint chroma_pixel_stride,
                                           jlong timestamp_ns, jint lane, jdoubleArray out) {
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

    // Colour, when the camera gave it: an M5 sender needs it, and an M0 one ignores it. Packed per
    // thread, because each decode lane is one thread and the buffers are reused frame to frame.
    thread_local std::vector<std::uint8_t> u_packed;
    thread_local std::vector<std::uint8_t> v_packed;
    const int cw = width / 2;
    const int ch = height / 2;
    fileflow::ChromaPlanes chroma;
    const bool colour =
        Unpack(env, u_plane, cw, ch, chroma_row_stride, chroma_pixel_stride, &u_packed) &&
        Unpack(env, v_plane, cw, ch, chroma_row_stride, chroma_pixel_stride, &v_packed);
    if (colour) {
        chroma.u = fileflow::ImageView8(u_packed.data(), cw, ch, cw);
        chroma.v = fileflow::ImageView8(v_packed.data(), cw, ch, cw);
    }

    // A copy: other lanes may be updating the receiver's status concurrently.
    const fileflow::LiveStatus s = rx->Ingest(img, static_cast<std::int64_t>(timestamp_ns),
                                              lane > 0 ? static_cast<std::size_t>(lane) : 0u,
                                              colour ? &chroma : nullptr);

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
    v[kDistinctStates] = static_cast<jdouble>(s.distinct_states);
    v[kColorSpread] = s.color_spread;
    v[kColorNoise] = s.color_noise;
    v[kTransferSeconds] = s.transfer_seconds;
    v[kBitsPerCell] = static_cast<jdouble>(s.bits_per_cell);
    v[kOriginalSize] = static_cast<jdouble>(s.content.original_size);
    v[kCodec] = static_cast<jdouble>(s.content.codec);
    v[kCompressMs] = static_cast<jdouble>(s.content.compress_ms);
    v[kCombinedFrames] = static_cast<jdouble>(s.combined_frames);
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
