#include "guest_jni_runtime.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace zb {

namespace {

JNIEnv* E(JniBackend::Env env) {
    return reinterpret_cast<JNIEnv*>(static_cast<std::uintptr_t>(env));
}

std::once_flag g_once;
std::atomic<GuestJniRuntime*> g_instance{nullptr};

}  // namespace

GuestJniRuntime& GuestJniRuntime::get(JNIEnv* env) {
    std::call_once(g_once, [env] {
        JavaVM* vm = nullptr;
        if (env->GetJavaVM(&vm) != JNI_OK || vm == nullptr) env->FatalError("ZettaBridge: GetJavaVM failed");
        // Process-lifetime by design: never deleted (see the class comment).
        g_instance.store(new GuestJniRuntime(vm));
    });
    return *g_instance.load();
}

GuestJniRuntime* GuestJniRuntime::peek() {
    return g_instance.load();
}

GuestJniRuntime::GuestJniRuntime(JavaVM* vm)
    : backend_(vm),
      engine_(backend_, gl_backend_, asset_backend_, egl_backend_, window_backend_, looper_backend_,
              input_backend_, configuration_backend_),
      native_activity_glue_(*engine_.host_native_activity(), engine_.host_native_window(), engine_.host_input()),
      proxies_(engine_) {
    engine_.set_native_activity_platform(&native_activity_glue_);
}

bool GuestJniRuntime::Engine::bind_class_loader(JniBackend::Env env, JniBackend::Ref loader, std::string& error) {
    JNIEnv* e = E(env);
    if (jni_backend_.set_class_loader(e, reinterpret_cast<jobject>(static_cast<std::uintptr_t>(loader)))) {
        return true;
    }
    const std::string pending = take_pending_exception(env);
    error = "the object is not a ClassLoader, or a different plugin class loader is already bound";
    if (!pending.empty()) error += " (" + pending + ")";
    return false;
}

// Throwable.toString() of the pending exception, which is cleared. ART clears it anyway after a
// failed JNI_OnLoad, so its text only survives inside ZettaBridge's own error message.
std::string GuestJniRuntime::Engine::take_pending_exception(JniBackend::Env env) {
    JNIEnv* e = E(env);
    if (e->ExceptionCheck() == JNI_FALSE) return {};
    jthrowable pending = e->ExceptionOccurred();
    e->ExceptionClear();
    std::string text = "a Java exception was pending";
    if (pending == nullptr || e->PushLocalFrame(4) != JNI_OK) {
        e->ExceptionClear();
        if (pending != nullptr) e->DeleteLocalRef(pending);
        return text;
    }
    jclass throwable = e->FindClass("java/lang/Throwable");
    jmethodID to_string =
        throwable != nullptr ? e->GetMethodID(throwable, "toString", "()Ljava/lang/String;") : nullptr;
    auto description = to_string != nullptr ? static_cast<jstring>(e->CallObjectMethod(pending, to_string)) : nullptr;
    if (e->ExceptionCheck() == JNI_FALSE && description != nullptr) {
        const char* chars = e->GetStringUTFChars(description, nullptr);
        if (chars != nullptr) {
            text = chars;
            e->ReleaseStringUTFChars(description, chars);
        }
    }
    e->ExceptionClear();
    e->PopLocalFrame(nullptr);
    e->DeleteLocalRef(pending);
    return text;
}

}  // namespace zb
