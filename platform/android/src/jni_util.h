// Shared JNI marshalling helpers.
//
// ADR-0014: this layer copies data and makes no decisions, so everything here is deliberately
// mechanical. The one opinion it holds is that a field we failed to read is an ERROR, never a
// default -- see FieldReader.
#pragma once

#include <fileflow/device.h>

#include <jni.h>

#include <cstdint>
#include <string>
#include <vector>

namespace fileflow::android_jni {

// Reads fields off a Java object, remembering the first failure instead of throwing per access.
//
// WHY IT DOES NOT SUBSTITUTE DEFAULTS. If Kotlin renames `measuredFd`, `GetFieldID` returns
// null. Quietly reading 0.0 would produce a DeviceReport that looks measured, claims a
// plausible value, and is fiction -- the invisible substitution the Evidence enum exists to
// prevent. So the reader records the failure and the caller refuses to proceed.
class FieldReader {
  public:
    FieldReader(JNIEnv* env, jobject obj) : env_(env), cls_(env->GetObjectClass(obj)), obj_(obj) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] const std::string& first_error() const noexcept { return first_error_; }

    double Double(const char* name) {
        jfieldID f = Field(name, "D");
        return f != nullptr ? env_->GetDoubleField(obj_, f) : 0.0;
    }

    std::int32_t Int(const char* name) {
        jfieldID f = Field(name, "I");
        return f != nullptr ? env_->GetIntField(obj_, f) : 0;
    }

    std::int64_t Long(const char* name) {
        jfieldID f = Field(name, "J");
        return f != nullptr ? env_->GetLongField(obj_, f) : 0;
    }

    bool Bool(const char* name) {
        jfieldID f = Field(name, "Z");
        return f != nullptr && env_->GetBooleanField(obj_, f) == JNI_TRUE;
    }

    std::string String(const char* name) {
        jfieldID f = Field(name, "Ljava/lang/String;");
        if (f == nullptr) return {};
        auto js = static_cast<jstring>(env_->GetObjectField(obj_, f));
        if (js == nullptr) return {};  // a null String field is legitimately "not recorded"
        const char* c = env_->GetStringUTFChars(js, nullptr);
        std::string out = c != nullptr ? c : "";
        if (c != nullptr) env_->ReleaseStringUTFChars(js, c);
        env_->DeleteLocalRef(js);
        return out;
    }

  private:
    jfieldID Field(const char* name, const char* sig) {
        jfieldID f = env_->GetFieldID(cls_, name, sig);
        if (f == nullptr) {
            if (ok_) first_error_ = std::string("missing field ") + name + " : " + sig;
            ok_ = false;
            env_->ExceptionClear();  // we report via first_error_, not a pending exception
        }
        return f;
    }

    JNIEnv* env_;
    jclass cls_;
    jobject obj_;
    bool ok_ = true;
    std::string first_error_;
};

// Kotlin passes evidence as an ordinal so the enum is defined in exactly one place (device.h).
// Anything out of range becomes kUnknown, the conservative reading: "we did not check".
inline Evidence EvidenceFromOrdinal(std::int32_t v) noexcept {
    switch (v) {
        case 1: return Evidence::kClaimed;
        case 2: return Evidence::kVerified;
        case 3: return Evidence::kRefuted;
        default: return Evidence::kUnknown;
    }
}

// A Kotlin `String.toByteArray(UTF_8)` as a std::string. Strings cross as bytes rather than jstring
// because `GetStringUTFChars` returns MODIFIED UTF-8, which encodes anything outside the BMP -- an
// emoji in a file name -- differently from the UTF-8 everything else expects.
inline std::string Utf8(JNIEnv* env, jbyteArray bytes) {
    if (bytes == nullptr) return {};
    const jsize n = env->GetArrayLength(bytes);
    std::string out(static_cast<std::size_t>(n), '\0');
    if (n > 0) env->GetByteArrayRegion(bytes, 0, n, reinterpret_cast<jbyte*>(out.data()));
    return out;
}

inline void ThrowIllegalState(JNIEnv* env, const std::string& msg) {
    jclass cls = env->FindClass("java/lang/IllegalStateException");
    if (cls != nullptr) env->ThrowNew(cls, msg.c_str());
}

inline jobject MakeStringList(JNIEnv* env, const std::vector<std::string>& items) {
    jclass list_cls = env->FindClass("java/util/ArrayList");
    if (list_cls == nullptr) return nullptr;
    jmethodID ctor = env->GetMethodID(list_cls, "<init>", "(I)V");
    jmethodID add = env->GetMethodID(list_cls, "add", "(Ljava/lang/Object;)Z");
    if (ctor == nullptr || add == nullptr) return nullptr;

    jobject list = env->NewObject(list_cls, ctor, static_cast<jint>(items.size()));
    if (list == nullptr) return nullptr;
    for (const std::string& s : items) {
        jstring js = env->NewStringUTF(s.c_str());
        if (js == nullptr) return list;  // OOM pending; caller sees the exception
        env->CallBooleanMethod(list, add, js);
        env->DeleteLocalRef(js);
    }
    return list;
}

}  // namespace fileflow::android_jni
