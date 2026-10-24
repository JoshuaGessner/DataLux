// JNI marshalling for the aiming analyser (C02a / feature UI-02).
//
// ADR-0014: the judgement lives in `core/src/framing.cpp` where the desktop suite exercises it. This
// file moves numbers across the boundary and decides nothing.
//
// ON ADR-0003's "NO PER-FRAME DATA CROSSES JNI". This reads the camera's own Y plane through
// `GetDirectBufferAddress` -- a pointer, not a copy -- and writes its results into a caller-owned
// `double[]` allocated once. A non-direct buffer is REFUSED rather than silently copied.
//
// WHY A `double[]` RATHER THAN AN OBJECT. Building a Java object per frame would allocate on a path
// that runs several times a second, and the fields would then have to be kept in sync by hand across
// the boundary -- the same ABI-by-convention hazard `NativeProbe.kt` warns about. One primitive array
// with a documented layout, asserted on both sides, is harder to get quietly wrong.
#include "jni_util.h"

#include <fileflow/framing.h>
#include <fileflow/grid.h>
#include <fileflow/image.h>
#include <fileflow/result.h>

#include <jni.h>

#include <cstdint>
#include <string>

namespace {

// Slot layout. MUST match `AimAnalyser.Slot` in Kotlin; the count is asserted on both sides so a
// mismatch fails loudly at the first call rather than producing plausible nonsense.
enum Slot : int {
    kVerdict = 0,
    kLitFraction,
    kBboxX,
    kBboxY,
    kBboxW,
    kBboxH,
    kClippedLeft,
    kClippedTop,
    kClippedRight,
    kClippedBottom,
    kRotationDeg,
    kPxPerCell,
    kBboxInflation,
    kMidFraction,
    kMeanLuminance,
    kThreshold,
    kLevelSeparation,
    kDarkLevel,
    kBrightLevel,
    kWhiteLevel,
    kSlotCount,
};

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL
Java_dev_fileflow_aim_NativeAim_tileCount(JNIEnv* /*env*/, jclass /*unused*/) {
    return static_cast<jint>(fileflow::kAimTiles);
}

extern "C" JNIEXPORT jint JNICALL
Java_dev_fileflow_aim_NativeAim_tileCols(JNIEnv* /*env*/, jclass /*unused*/) {
    return static_cast<jint>(fileflow::kAimTileCols);
}

extern "C" JNIEXPORT jdouble JNICALL
Java_dev_fileflow_aim_NativeAim_tileUsableSeparation(JNIEnv* /*env*/, jclass /*unused*/) {
    return static_cast<jdouble>(fileflow::kTileUsableSeparation);
}

extern "C" JNIEXPORT jint JNICALL
Java_dev_fileflow_aim_NativeAim_slotCount(JNIEnv* /*env*/, jclass /*unused*/) {
    return static_cast<jint>(kSlotCount);
}

// Analyse one Y plane. Returns the guidance sentence, or an empty string on a marshalling failure --
// distinguishable from a real verdict because `out[kVerdict]` is left at -1.
JNIEXPORT jstring JNICALL
Java_dev_fileflow_aim_NativeAim_analyse(JNIEnv* env, jclass /*unused*/, jobject y_plane, jint width,
                                        jint height, jint row_stride, jint cols, jint rows,
                                        jdoubleArray out, jdoubleArray tiles) {
    if (out != nullptr && env->GetArrayLength(out) >= 1) {
        const jdouble sentinel = -1.0;
        env->SetDoubleArrayRegion(out, kVerdict, 1, &sentinel);
    }
    if (y_plane == nullptr || out == nullptr) return env->NewStringUTF("");
    if (env->GetArrayLength(out) < static_cast<jsize>(kSlotCount)) return env->NewStringUTF("");
    if (width <= 0 || height <= 0 || row_stride < width) return env->NewStringUTF("");

    auto* base = static_cast<const std::uint8_t*>(env->GetDirectBufferAddress(y_plane));
    if (base == nullptr) return env->NewStringUTF("");

    // Bounds-check what the buffer actually holds before viewing it. The camera reports stride and
    // size; trusting them unchecked is how a driver quirk becomes an out-of-bounds read
    // (INPUT-VALIDATION: bounds before use).
    const std::int64_t needed =
        static_cast<std::int64_t>(row_stride) * static_cast<std::int64_t>(height - 1) + width;
    if (env->GetDirectBufferCapacity(y_plane) < needed) return env->NewStringUTF("");

    const fileflow::ImageView8 img(base, width, height, row_stride);
    const fileflow::GridGeometry grid{static_cast<std::uint32_t>(cols),
                                      static_cast<std::uint32_t>(rows)};

    auto r = fileflow::AnalyseAim(img, grid);
    if (!r.ok()) {
        return env->NewStringUTF(
            (std::string("cannot analyse: ") + std::string(fileflow::ErrorName(r.error()))).c_str());
    }
    const fileflow::AimAdvice& a = r.value();

    jdouble v[kSlotCount];
    v[kVerdict] = static_cast<jdouble>(static_cast<int>(a.verdict));
    v[kLitFraction] = a.lit_fraction;
    v[kBboxX] = static_cast<jdouble>(a.bbox_x);
    v[kBboxY] = static_cast<jdouble>(a.bbox_y);
    v[kBboxW] = static_cast<jdouble>(a.bbox_w);
    v[kBboxH] = static_cast<jdouble>(a.bbox_h);
    v[kClippedLeft] = a.clipped_left ? 1.0 : 0.0;
    v[kClippedTop] = a.clipped_top ? 1.0 : 0.0;
    v[kClippedRight] = a.clipped_right ? 1.0 : 0.0;
    v[kClippedBottom] = a.clipped_bottom ? 1.0 : 0.0;
    v[kRotationDeg] = a.rotation_deg;
    v[kPxPerCell] = a.px_per_cell;
    v[kBboxInflation] = a.bbox_inflation;
    v[kMidFraction] = a.mid_fraction;
    v[kMeanLuminance] = a.mean_luminance;
    v[kThreshold] = static_cast<jdouble>(a.threshold);
    v[kLevelSeparation] = a.level_separation;
    v[kDarkLevel] = a.dark_level;
    v[kBrightLevel] = a.bright_level;
    v[kWhiteLevel] = a.white_level;
    env->SetDoubleArrayRegion(out, 0, static_cast<jsize>(kSlotCount), v);

    // The coverage map travels in its own array rather than as extra slots: the slot enum is
    // duplicated in Kotlin and checked field by field, and appending 60 unnamed doubles to it would
    // turn a checked ABI into an offset anyone could shift by one. A short or absent array is
    // tolerated -- a caller that does not want the map should not have to pass one.
    if (tiles != nullptr) {
        const jsize want = static_cast<jsize>(fileflow::kAimTiles);
        if (env->GetArrayLength(tiles) >= want) {
            jdouble t[fileflow::kAimTiles];
            for (std::size_t i = 0; i < fileflow::kAimTiles; ++i) {
                t[i] = static_cast<jdouble>(a.tile_separation[i]);
            }
            env->SetDoubleArrayRegion(tiles, 0, want, t);
        }
    }

    return env->NewStringUTF(a.guidance.c_str());
}

}  // extern "C"
