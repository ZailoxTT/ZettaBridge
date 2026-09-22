// libzbproxy.so: the arm64 proxy ART loads in place of an arm32 plugin library.
//
// The launcher's plugin class loader returns one copy of this library per arm32 library
// (plugins/<pkg>/proxy/lib<name>.so). Its only job is to tell the translator which copy was
// loaded: JNI_OnLoad finds its own path with dladdr and calls
//     static int com.zettabridge.core.ZBridge.onProxyLoaded(String proxyPath)
// During JNI_OnLoad ART's class-loader override is the plugin loader, which delegates
// com.zettabridge.core.* to the launcher. The proxy links against nothing of ours (no
// libzbridge.so, no libc++): tools/check_zbproxy.py checks every Android link.
//
// Contract of onProxyLoaded: it returns the guest JNI version (0 means "no preference" and
// becomes JNI_VERSION_1_6) or throws UnsatisfiedLinkError. Any exception, or a version ART
// would reject, makes JNI_OnLoad return JNI_ERR. A pending exception is left pending; note
// that ART's JVM_NativeLoad clears it and System.loadLibrary throws its own
// "JNI_ERR returned from JNI_OnLoad" error, so the translator must record failure detail
// itself.

#define _GNU_SOURCE  // Dl_info and dladdr on glibc (host unit test).

#include <dlfcn.h>
#include <jni.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

static const char kBridgeClass[] = "com/zettabridge/core/ZBridge";
static const char kMethodName[] = "onProxyLoaded";
static const char kMethodSignature[] = "(Ljava/lang/String;)I";
static const char kActivityMethodName[] = "onNativeActivityCreated";
static const char kActivityMethodSignature[] = "(JJJLjava/lang/String;)Z";

// Captured in JNI_OnLoad, because ANativeActivity_onCreate arrives without either. The path is
// copied: dladdr's string belongs to the loader.
static JavaVM* g_vm;
static char g_proxy_path[1024];

__attribute__((format(printf, 1, 2)))
static void log_error(const char* format, ...) {
#ifdef __ANDROID__
    va_list args;
    va_start(args, format);
    __android_log_vprint(ANDROID_LOG_ERROR, "zbproxy", format, args);
    va_end(args);
#else
    (void)format;
#endif
}

// The versions ART's JavaVMExt::IsBadJniVersion accepts from JNI_OnLoad.
static int supported_version(jint version) {
    return version == JNI_VERSION_1_2 || version == JNI_VERSION_1_4 || version == JNI_VERSION_1_6;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;

    Dl_info info;
    if (dladdr((void*)&JNI_OnLoad, &info) == 0 || info.dli_fname == NULL || info.dli_fname[0] != '/') {
        log_error("cannot find the proxy path with dladdr");
        return JNI_ERR;
    }
    const char* proxy_path = info.dli_fname;

    JNIEnv* env = NULL;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK || env == NULL) {
        log_error("%s: GetEnv(JNI_VERSION_1_6) failed", proxy_path);
        return JNI_ERR;
    }
    if ((*env)->ExceptionCheck(env)) {
        log_error("%s: exception already pending in JNI_OnLoad", proxy_path);
        return JNI_ERR;
    }

    jclass bridge = (*env)->FindClass(env, kBridgeClass);
    if (bridge == NULL) {
        log_error("%s: class %s not found", proxy_path, kBridgeClass);
        return JNI_ERR;
    }

    g_vm = vm;
    strncpy(g_proxy_path, proxy_path, sizeof g_proxy_path - 1);

    jint result = JNI_ERR;
    jstring path = NULL;
    jmethodID method = (*env)->GetStaticMethodID(env, bridge, kMethodName, kMethodSignature);
    if (method == NULL) {
        log_error("%s: method %s%s not found", proxy_path, kMethodName, kMethodSignature);
        goto done;
    }
    path = (*env)->NewStringUTF(env, proxy_path);
    if (path == NULL) {
        log_error("%s: NewStringUTF failed", proxy_path);
        goto done;
    }

    jint reported = (*env)->CallStaticIntMethod(env, bridge, method, path);
    if ((*env)->ExceptionCheck(env)) {
        log_error("%s: %s threw", proxy_path, kMethodName);
        goto done;
    }
    if (reported == 0) {
        result = JNI_VERSION_1_6;
    } else if (supported_version(reported)) {
        result = reported;
    } else {
        log_error("%s: %s returned unsupported JNI version 0x%08x", proxy_path, kMethodName,
                  (unsigned)reported);
    }

done:
    // DeleteLocalRef is one of the calls JNI allows with an exception pending.
    if (path != NULL) (*env)->DeleteLocalRef(env, path);
    (*env)->DeleteLocalRef(env, bridge);
    return result;
}

// The one export android.app.NativeActivity looks for. The framework loads the library the
// manifest names, which for a plugin is this proxy, then dlopens that same path and calls this
// function; without it the activity dies with UnsatisfiedLinkError before any guest code runs.
//
// `activity` is an ANativeActivity* of the host, and this file deliberately does not know that
// structure: it hands the pointer to the bridge, which owns the layout and builds the 32-bit
// activity the guest sees. A void* first parameter is ABI-identical to the real signature, and
// keeps the proxy free of NDK headers so the host unit test can load it.
JNIEXPORT void JNICALL ANativeActivity_onCreate(void* activity, void* saved_state, size_t saved_state_size) {
    if (g_vm == NULL || g_proxy_path[0] == 0) {
        log_error("ANativeActivity_onCreate before JNI_OnLoad: no bridge to call");
        return;
    }
    JNIEnv* env = NULL;
    if ((*g_vm)->GetEnv(g_vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK || env == NULL) {
        log_error("%s: GetEnv(JNI_VERSION_1_6) failed in ANativeActivity_onCreate", g_proxy_path);
        return;
    }
    if ((*env)->ExceptionCheck(env)) {
        log_error("%s: exception already pending in ANativeActivity_onCreate", g_proxy_path);
        return;
    }

    jclass bridge = (*env)->FindClass(env, kBridgeClass);
    if (bridge == NULL) {
        log_error("%s: class %s not found", g_proxy_path, kBridgeClass);
        return;
    }
    jstring path = NULL;
    jmethodID method = (*env)->GetStaticMethodID(env, bridge, kActivityMethodName, kActivityMethodSignature);
    if (method == NULL) {
        log_error("%s: method %s%s not found", g_proxy_path, kActivityMethodName, kActivityMethodSignature);
        goto done;
    }
    path = (*env)->NewStringUTF(env, g_proxy_path);
    if (path == NULL) {
        log_error("%s: NewStringUTF failed in ANativeActivity_onCreate", g_proxy_path);
        goto done;
    }

    jboolean ready = (*env)->CallStaticBooleanMethod(env, bridge, method, (jlong)(uintptr_t)activity,
                                                     (jlong)(uintptr_t)saved_state, (jlong)saved_state_size, path);
    if ((*env)->ExceptionCheck(env)) {
        // Cleared on purpose: an exception left pending here surfaces at an unrelated JNI call
        // later. The bridge records the detail in its runtime report, which is what a phone with
        // no usable logcat can actually show.
        log_error("%s: %s threw", g_proxy_path, kActivityMethodName);
        (*env)->ExceptionClear(env);
    } else if (!ready) {
        log_error("%s: %s refused the activity", g_proxy_path, kActivityMethodName);
    }

done:
    if (path != NULL) (*env)->DeleteLocalRef(env, path);
    (*env)->DeleteLocalRef(env, bridge);
}
