// JNI entry points of libzbridge.so for com.zettabridge.core.ZBridge.
#include <jni.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "guest_jni_runtime.h"
#include "zb/elf_fixups.h"
#include "zb/log.h"
#include "zb/runtime_report.h"
#include "zb/zbridge.h"

namespace {

std::vector<std::string> to_strings(JNIEnv* env, jobjectArray array) {
    std::vector<std::string> out;
    if (array == nullptr) return out;
    const jsize count = env->GetArrayLength(array);
    out.reserve(static_cast<std::size_t>(count));
    for (jsize i = 0; i < count; ++i) {
        auto value = static_cast<jstring>(env->GetObjectArrayElement(array, i));
        if (value == nullptr) {
            out.emplace_back();
            continue;
        }
        const char* chars = env->GetStringUTFChars(value, nullptr);
        out.emplace_back(chars != nullptr ? chars : "");
        if (chars != nullptr) env->ReleaseStringUTFChars(value, chars);
        env->DeleteLocalRef(value);
    }
    return out;
}

std::vector<const char*> to_pointers(const std::vector<std::string>& strings) {
    std::vector<const char*> pointers;
    pointers.reserve(strings.size() + 1);
    for (const auto& s : strings) pointers.push_back(s.c_str());
    pointers.push_back(nullptr);
    return pointers;
}

// nullopt for a null string or when GetStringUTFChars failed (OutOfMemoryError pending).
std::optional<std::string> to_string(JNIEnv* env, jstring value) {
    if (value == nullptr) return std::nullopt;
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) return std::nullopt;
    std::string out(chars);
    env->ReleaseStringUTFChars(value, chars);
    return out;
}

void throw_new(JNIEnv* env, const char* class_name, const std::string& message) {
    if (env->ExceptionCheck()) return;
    jclass cls = env->FindClass(class_name);
    if (cls == nullptr) return;  // NoClassDefFoundError pending
    env->ThrowNew(cls, message.c_str());
    env->DeleteLocalRef(cls);
}

zb::JniBackend::Env to_env(JNIEnv* env) {
    return static_cast<zb::JniBackend::Env>(reinterpret_cast<std::uintptr_t>(env));
}

jstring optional_string(JNIEnv* env, const std::optional<std::string>& value) {
    return value ? env->NewStringUTF(value->c_str()) : nullptr;
}

}  // namespace

extern "C" {

// static native int runExecutable(String sysroot, String[] argv, String[] envp)
// Blocks until the guest exits. envp == null passes the app process environment.
JNIEXPORT jint JNICALL Java_com_zettabridge_core_ZBridge_runExecutable(JNIEnv* env, jclass, jstring sysroot,
                                                                      jobjectArray argv, jobjectArray envp) {
    std::string sysroot_path;
    if (sysroot != nullptr) {
        const char* chars = env->GetStringUTFChars(sysroot, nullptr);
        if (chars != nullptr) {
            sysroot_path = chars;
            env->ReleaseStringUTFChars(sysroot, chars);
        }
    }

    const std::vector<std::string> args = to_strings(env, argv);
    if (args.empty()) return 2;
    const std::vector<const char*> arg_pointers = to_pointers(args);

    std::vector<std::string> env_strings;
    std::vector<const char*> env_pointers;
    if (envp != nullptr) {
        env_strings = to_strings(env, envp);
        env_pointers = to_pointers(env_strings);
    }

    return zb_run_executable(sysroot != nullptr ? sysroot_path.c_str() : nullptr, static_cast<int>(args.size()),
                             arg_pointers.data(), envp != nullptr ? env_pointers.data() : nullptr);
}

// static native void activatePlugin(String pluginRoot, int targetSdk, ClassLoader classLoader)
// Throws IllegalStateException with the reason.
JNIEXPORT void JNICALL Java_com_zettabridge_core_ZBridge_activatePlugin(JNIEnv* env, jclass, jstring plugin_root,
                                                                       jint target_sdk, jobject class_loader) {
    const std::optional<std::string> root = to_string(env, plugin_root);
    if (!root) {
        throw_new(env, "java/lang/NullPointerException", "pluginRoot");
        return;
    }
    if (target_sdk <= 0) {
        throw_new(env, "java/lang/IllegalStateException", "invalid plugin targetSdk " + std::to_string(target_sdk));
        return;
    }
    std::string error;
    if (!zb::GuestJniRuntime::get(env).proxies().activate_plugin(
            to_env(env), *root, static_cast<std::uint32_t>(target_sdk),
            static_cast<zb::JniBackend::Ref>(reinterpret_cast<std::uintptr_t>(class_loader)), error)) {
        zb::log("ZBridge.activatePlugin: %s", error.c_str());
        if (env->ExceptionCheck()) env->ExceptionClear();
        throw_new(env, "java/lang/IllegalStateException", "ZettaBridge cannot activate plugin: " + error);
    }
}

// static native int onProxyLoaded(String proxyPath), called by libzbproxy.so's JNI_OnLoad.
// Returns the guest JNI version or throws UnsatisfiedLinkError with the full detail, which also
// stays available through loadError/lastLoadError because ART replaces it.
JNIEXPORT jint JNICALL Java_com_zettabridge_core_ZBridge_onProxyLoaded(JNIEnv* env, jclass, jstring proxy_path) {
    const std::optional<std::string> path = to_string(env, proxy_path);
    if (!path) {
        throw_new(env, "java/lang/UnsatisfiedLinkError", "ZettaBridge: null proxy path");
        return JNI_ERR;
    }
    const zb::ProxyLoadResult result = zb::GuestJniRuntime::get(env).proxies().on_proxy_loaded(to_env(env), *path);
    if (result.ok) return result.jni_version;
    if (env->ExceptionCheck()) env->ExceptionClear();
    throw_new(env, "java/lang/UnsatisfiedLinkError", result.error);
    return JNI_ERR;
}

// static native boolean onNativeActivityCreated(long activity, long savedState, long size, String proxy)
// Called by libzbproxy.so from ANativeActivity_onCreate. Never throws: the framework is in the
// middle of creating an activity, and an exception here would surface somewhere unrelated. A
// refusal is false, with the reason in the runtime report.
JNIEXPORT jboolean JNICALL Java_com_zettabridge_core_ZBridge_onNativeActivityCreated(
    JNIEnv* env, jclass, jlong activity, jlong saved_state, jlong saved_state_size, jstring proxy_path) {
    const std::optional<std::string> path = to_string(env, proxy_path);
    if (!path) return JNI_FALSE;
    const bool ok = zb::GuestJniRuntime::get(env).proxies().on_native_activity_created(
        to_env(env), static_cast<std::uint64_t>(activity), static_cast<std::uint64_t>(saved_state),
        static_cast<std::uint64_t>(saved_state_size), *path);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return ok ? JNI_TRUE : JNI_FALSE;
}

// static native String loadError(String proxyPath): the stored failure of that proxy, or null.
JNIEXPORT jstring JNICALL Java_com_zettabridge_core_ZBridge_loadError(JNIEnv* env, jclass, jstring proxy_path) {
    const std::optional<std::string> path = to_string(env, proxy_path);
    zb::GuestJniRuntime* runtime = zb::GuestJniRuntime::peek();
    if (!path || runtime == nullptr) return nullptr;
    return optional_string(env, runtime->proxies().load_error(*path));
}

// static native String lastLoadError(): the most recent proxy load failure, or null.
JNIEXPORT jstring JNICALL Java_com_zettabridge_core_ZBridge_lastLoadError(JNIEnv* env, jclass) {
    zb::GuestJniRuntime* runtime = zb::GuestJniRuntime::peek();
    return runtime != nullptr ? optional_string(env, runtime->proxies().last_load_error()) : nullptr;
}

// static native boolean setReportFile(String path)
// Starts persisting the runtime report to `path`, rewriting it atomically whenever the report
// changes. Called once per :guest process, before plugin code runs, so a process that dies
// silently still leaves its last state on disk (OxygenOS drops our logcat output).
JNIEXPORT jboolean JNICALL Java_com_zettabridge_core_ZBridge_setReportFile(JNIEnv* env, jclass, jstring path) {
    const std::optional<std::string> file = to_string(env, path);
    if (!file) {
        throw_new(env, "java/lang/NullPointerException", "path");
        return JNI_FALSE;
    }
    return zb::write_runtime_report_to(zb::runtime_report(), *file) ? JNI_TRUE : JNI_FALSE;
}

// static native String runtimeReport(): the same text that setReportFile persists.
JNIEXPORT jstring JNICALL Java_com_zettabridge_core_ZBridge_runtimeReport(JNIEnv* env, jclass) {
    return env->NewStringUTF(zb::runtime_report().text().c_str());
}

// static native String fixGuestLibrary(String path) throws IOException
// Applies the shared import fixups in place. Returns "unchanged", "changed: <c1>; <c2>" or
// "skipped: <reason>" (not an ARM ELF32 shared library); throws IOException on a malformed file.
JNIEXPORT jstring JNICALL Java_com_zettabridge_core_ZBridge_fixGuestLibrary(JNIEnv* env, jclass, jstring path) {
    const std::optional<std::string> file = to_string(env, path);
    if (!file) {
        throw_new(env, "java/lang/NullPointerException", "path");
        return nullptr;
    }
    const zb::ElfFixupReport report = zb::fix_guest_library(*file);
    std::string summary;
    switch (report.status) {
    case zb::ElfFixupStatus::Unchanged:
        summary = "unchanged";
        break;
    case zb::ElfFixupStatus::Changed:
        summary = "changed: ";
        for (std::size_t i = 0; i < report.changes.size(); ++i) {
            if (i != 0) summary += "; ";
            summary += report.changes[i];
        }
        break;
    case zb::ElfFixupStatus::Skipped:
        summary = "skipped: " + report.message;
        break;
    case zb::ElfFixupStatus::Error:
        throw_new(env, "java/io/IOException", *file + ": " + report.message);
        return nullptr;
    }
    return env->NewStringUTF(summary.c_str());
}

// static native void setPreciseFaults(boolean enabled)
// Must be called before the guest runtime/Process is constructed: LibraryRuntime is
// process-lifetime and reads ZB_PRECISE_FAULTS only at Process construction.
JNIEXPORT void JNICALL Java_com_zettabridge_core_ZBridge_setPreciseFaults(JNIEnv*, jclass, jboolean enabled) {
    ::setenv("ZB_PRECISE_FAULTS", enabled ? "1" : "0", 1);
}

// static native void setGlDiagnostics(boolean enabled)
// Must be called before the guest JNI runtime is constructed: it reads ZB_GL_DIAGNOSTICS once.
JNIEXPORT void JNICALL Java_com_zettabridge_core_ZBridge_setGlDiagnostics(JNIEnv*, jclass, jboolean enabled) {
    ::setenv("ZB_GL_DIAGNOSTICS", enabled ? "1" : "0", 1);
}

}  // extern "C"
