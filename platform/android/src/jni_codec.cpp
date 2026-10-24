// JNI adapter for file compression (core/compress.h). Marshalling only: the codec, its bounds and
// its errors are all decided in core (ADR-0014).
#include <fileflow/compress.h>

#include <jni.h>

#include <cstdint>
#include <span>
#include <vector>

namespace {

jbyteArray ToJava(JNIEnv* env, const std::vector<std::uint8_t>& bytes) {
    jbyteArray out = env->NewByteArray(static_cast<jsize>(bytes.size()));
    if (out == nullptr) return nullptr;  // OutOfMemoryError is pending
    if (!bytes.empty()) {
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(bytes.size()),
                                reinterpret_cast<const jbyte*>(bytes.data()));
    }
    return out;
}

std::vector<std::uint8_t> FromJava(JNIEnv* env, jbyteArray in, jint offset, jint length) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    if (length > 0) {
        env->GetByteArrayRegion(in, offset, length, reinterpret_cast<jbyte*>(bytes.data()));
    }
    return bytes;
}

bool InBounds(JNIEnv* env, jbyteArray a, jint offset, jint length) {
    return a != nullptr && offset >= 0 && length >= 0 &&
           static_cast<jlong>(offset) + length <= env->GetArrayLength(a);
}

}  // namespace

extern "C" {

// Signatures match app/src/main/kotlin/dev/fileflow/secure/NativeCodec.kt. Null = failure.
JNIEXPORT jbyteArray JNICALL
Java_dev_fileflow_secure_NativeCodec_zstdCompress(JNIEnv* env, jclass /*unused*/, jbyteArray data,
                                                  jint level, jboolean long_window) {
    if (data == nullptr) return nullptr;
    const auto in = FromJava(env, data, 0, env->GetArrayLength(data));
    auto r = fileflow::Compress(in, {.level = level, .long_window = long_window == JNI_TRUE});
    return r.ok() ? ToJava(env, r.value()) : nullptr;
}

JNIEXPORT jbyteArray JNICALL
Java_dev_fileflow_secure_NativeCodec_zstdDecompress(JNIEnv* env, jclass /*unused*/, jbyteArray data,
                                                    jint offset, jint length, jint expected) {
    if (!InBounds(env, data, offset, length) || expected < 0) return nullptr;
    const auto in = FromJava(env, data, offset, length);
    auto r = fileflow::Decompress(in, static_cast<std::size_t>(expected));
    return r.ok() ? ToJava(env, r.value()) : nullptr;
}

JNIEXPORT jint JNICALL
Java_dev_fileflow_secure_NativeCodec_zstdMaxLevel(JNIEnv* /*env*/, jclass /*unused*/) {
    return fileflow::MaxCompressionLevel();
}

}  // extern "C"
