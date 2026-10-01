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

// Usually captured in JNI_OnLoad. NativeActivity may enter first, in which case its structure
// supplies the VM and current-thread JNIEnv. The path is copied: dladdr owns its string.
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

static jclass find_bridge(JNIEnv* env, jobject activity) {
    if (activity == NULL) return (*env)->FindClass(env, kBridgeClass);

    jclass activity_class = NULL;
    jobject loader = NULL;
    jclass loader_class = NULL;
    jstring name = NULL;
    jclass bridge = NULL;
    activity_class = (*env)->GetObjectClass(env, activity);
    if (activity_class == NULL) goto done;
    jmethodID get_loader = (*env)->GetMethodID(
        env, activity_class, "getClassLoader", "()Ljava/lang/ClassLoader;");
    if (get_loader == NULL) goto done;
    loader = (*env)->CallObjectMethod(env, activity, get_loader);
    if (loader == NULL || (*env)->ExceptionCheck(env)) goto done;
    loader_class = (*env)->GetObjectClass(env, loader);
    if (loader_class == NULL) goto done;
    jmethodID load_class = (*env)->GetMethodID(
        env, loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    if (load_class == NULL) goto done;
    name = (*env)->NewStringUTF(env, "com.zettabridge.core.ZBridge");
    if (name == NULL) goto done;
    bridge = (jclass)(*env)->CallObjectMethod(env, loader, load_class, name);

done:
    if (name != NULL) (*env)->DeleteLocalRef(env, name);
    if (loader_class != NULL) (*env)->DeleteLocalRef(env, loader_class);
    if (loader != NULL) (*env)->DeleteLocalRef(env, loader);
    if (activity_class != NULL) (*env)->DeleteLocalRef(env, activity_class);
    return bridge;
}

static int proxy_path(char* out, size_t size) {
    Dl_info info;
    if (dladdr((void*)&JNI_OnLoad, &info) == 0 || info.dli_fname == NULL || info.dli_fname[0] != '/') {
        log_error("cannot find the proxy path with dladdr");
        return 0;
    }
    strncpy(out, info.dli_fname, size - 1);
    out[size - 1] = 0;
    return 1;
}

static jint notify_proxy_loaded(JNIEnv* env, const char* path, jobject activity) {
    jclass bridge = find_bridge(env, activity);
    if (bridge == NULL) {
        log_error("%s: class %s not found", path, kBridgeClass);
        return JNI_ERR;
    }

    jint result = JNI_ERR;
    jstring jpath = NULL;
    jmethodID method = (*env)->GetStaticMethodID(env, bridge, kMethodName, kMethodSignature);
    if (method == NULL) {
        log_error("%s: method %s%s not found", path, kMethodName, kMethodSignature);
        goto done;
    }
    jpath = (*env)->NewStringUTF(env, path);
    if (jpath == NULL) {
        log_error("%s: NewStringUTF failed", path);
        goto done;
    }

    jint reported = (*env)->CallStaticIntMethod(env, bridge, method, jpath);
    if ((*env)->ExceptionCheck(env)) {
        log_error("%s: %s threw", path, kMethodName);
        goto done;
    }
    if (reported == 0) {
        result = JNI_VERSION_1_6;
    } else if (supported_version(reported)) {
        result = reported;
    } else {
        log_error("%s: %s returned unsupported JNI version 0x%08x", path, kMethodName,
                  (unsigned)reported);
    }

done:
    // DeleteLocalRef is one of the calls JNI allows with an exception pending.
    if (jpath != NULL) (*env)->DeleteLocalRef(env, jpath);
    (*env)->DeleteLocalRef(env, bridge);
    return result;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;

    if (!proxy_path(g_proxy_path, sizeof g_proxy_path)) return JNI_ERR;

    JNIEnv* env = NULL;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK || env == NULL) {
        log_error("%s: GetEnv(JNI_VERSION_1_6) failed", g_proxy_path);
        return JNI_ERR;
    }
    if ((*env)->ExceptionCheck(env)) {
        log_error("%s: exception already pending in JNI_OnLoad", g_proxy_path);
        return JNI_ERR;
    }

    g_vm = vm;
    return notify_proxy_loaded(env, g_proxy_path, NULL);
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
    JNIEnv* env = NULL;
    jobject activity_object = NULL;
    if (g_vm != NULL && g_proxy_path[0] != 0) {
        if ((*g_vm)->GetEnv(g_vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK || env == NULL) {
            log_error("%s: GetEnv(JNI_VERSION_1_6) failed in ANativeActivity_onCreate", g_proxy_path);
            return;
        }
    } else {
        // ANativeActivity starts with callbacks, JavaVM and JNIEnv. Android can call this entry
        // point without calling JNI_OnLoad, so bootstrap the same proxy load from those fields.
        struct ActivityPrefix {
            void* callbacks;
            JavaVM* vm;
            JNIEnv* env;
            jobject clazz;
        };
        struct ActivityPrefix* native = (struct ActivityPrefix*)activity;
        if (native == NULL || native->vm == NULL || native->env == NULL ||
            !proxy_path(g_proxy_path, sizeof g_proxy_path)) {
            log_error("ANativeActivity_onCreate has no VM, JNIEnv or proxy path");
            return;
        }
        g_vm = native->vm;
        env = native->env;
        activity_object = native->clazz;
        if ((*env)->ExceptionCheck(env)) {
            log_error("%s: exception already pending in ANativeActivity_onCreate", g_proxy_path);
            return;
        }
        if (notify_proxy_loaded(env, g_proxy_path, native->clazz) == JNI_ERR) {
            log_error("%s: proxy load failed in ANativeActivity_onCreate", g_proxy_path);
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            return;
        }
    }
    if ((*env)->ExceptionCheck(env)) {
        log_error("%s: exception already pending in ANativeActivity_onCreate", g_proxy_path);
        return;
    }

    jclass bridge = find_bridge(env, activity_object);
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
