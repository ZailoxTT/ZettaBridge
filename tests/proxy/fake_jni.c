// Fake-JNI unit test for core/android/zbproxy.c (libzbproxy.so).
//
// The proxy is built as a host shared library and loaded with dlopen by absolute path, so
// its dladdr self-identification runs for real. JavaVM and JNIEnv are fakes that record
// every call, track local references, and model a pending exception. A JNI call other than
// the exception/local-reference functions while an exception is pending is a violation,
// as CheckJNI would report on ART.

#include <dlfcn.h>
#include <jni.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s (case %s)\n", __FILE__, __LINE__, \
                    #cond, current_case);                                            \
            fflush(stderr);                                                          \
            _Exit(1);                                                                \
        }                                                                            \
    } while (0)

static const char* current_case = "setup";

enum { kMaxRefs = 16 };

struct Fake {
    // Scenario.
    int getenv_fails;
    int pending_on_entry;
    int find_class_fails;
    int method_fails;
    int new_string_fails;
    int call_throws;
    jint call_result;
    jboolean boolean_result;

    // Observations.
    jint getenv_version;
    int exception_pending;
    int violations;
    int find_class_calls;
    int method_calls;
    int new_string_calls;
    int static_int_calls;
    int static_boolean_calls;
    jlong activity;
    jlong saved_state;
    jlong saved_state_size;
    int deleted_unknown;
    int live_refs;
    int ref_live[kMaxRefs];
    char class_name[128];
    char method_name[64];
    char method_sig[64];
    char string_value[4096];
    jclass call_class;
    jmethodID call_method;
    jstring call_arg;
};

static struct Fake fake;
static int ref_tokens[kMaxRefs];
static int method_token;

static jobject new_ref(void) {
    for (int i = 0; i < kMaxRefs; ++i) {
        if (!fake.ref_live[i]) {
            fake.ref_live[i] = 1;
            ++fake.live_refs;
            return (jobject)&ref_tokens[i];
        }
    }
    CHECK(!"out of fake local references");
    return NULL;
}

static int ref_index(jobject ref) {
    for (int i = 0; i < kMaxRefs; ++i) {
        if (ref == (jobject)&ref_tokens[i]) return i;
    }
    return -1;
}

static void note_call(void) {
    if (fake.exception_pending) ++fake.violations;
}

static jclass JNICALL fake_find_class(JNIEnv* env, const char* name) {
    (void)env;
    note_call();
    ++fake.find_class_calls;
    snprintf(fake.class_name, sizeof fake.class_name, "%s", name);
    if (fake.find_class_fails) {
        fake.exception_pending = 1;
        return NULL;
    }
    return (jclass)new_ref();
}

static jmethodID JNICALL fake_get_static_method_id(JNIEnv* env, jclass cls, const char* name,
                                                   const char* sig) {
    (void)env;
    note_call();
    ++fake.method_calls;
    CHECK(ref_index(cls) >= 0 && fake.ref_live[ref_index(cls)]);
    snprintf(fake.method_name, sizeof fake.method_name, "%s", name);
    snprintf(fake.method_sig, sizeof fake.method_sig, "%s", sig);
    if (fake.method_fails) {
        fake.exception_pending = 1;
        return NULL;
    }
    return (jmethodID)&method_token;
}

static jstring JNICALL fake_new_string_utf(JNIEnv* env, const char* utf) {
    (void)env;
    note_call();
    ++fake.new_string_calls;
    snprintf(fake.string_value, sizeof fake.string_value, "%s", utf);
    if (fake.new_string_fails) {
        fake.exception_pending = 1;
        return NULL;
    }
    return (jstring)new_ref();
}

static jint JNICALL fake_call_static_int_method(JNIEnv* env, jclass cls, jmethodID method, ...) {
    (void)env;
    note_call();
    ++fake.static_int_calls;
    va_list args;
    va_start(args, method);
    jstring arg = va_arg(args, jstring);
    va_end(args);
    fake.call_class = cls;
    fake.call_method = method;
    fake.call_arg = arg;
    if (fake.call_throws) fake.exception_pending = 1;
    return fake.call_result;
}

static jboolean JNICALL fake_call_static_boolean_method(JNIEnv* env, jclass cls, jmethodID method, ...) {
    (void)env;
    note_call();
    ++fake.static_boolean_calls;
    va_list args;
    va_start(args, method);
    fake.activity = va_arg(args, jlong);
    fake.saved_state = va_arg(args, jlong);
    fake.saved_state_size = va_arg(args, jlong);
    fake.call_arg = va_arg(args, jstring);
    va_end(args);
    fake.call_class = cls;
    fake.call_method = method;
    if (fake.call_throws) fake.exception_pending = 1;
    return fake.boolean_result;
}

static void JNICALL fake_delete_local_ref(JNIEnv* env, jobject ref) {
    (void)env;
    if (ref == NULL) return;
    const int index = ref_index(ref);
    if (index < 0 || !fake.ref_live[index]) {
        ++fake.deleted_unknown;
        return;
    }
    fake.ref_live[index] = 0;
    --fake.live_refs;
}

static jboolean JNICALL fake_exception_check(JNIEnv* env) {
    (void)env;
    return fake.exception_pending ? JNI_TRUE : JNI_FALSE;
}

static void JNICALL fake_exception_clear(JNIEnv* env) {
    (void)env;
    fake.exception_pending = 0;
}

static struct JNINativeInterface_ env_table;
static const struct JNINativeInterface_* env_ptr = &env_table;

static jint JNICALL fake_get_env(JavaVM* vm, void** penv, jint version) {
    (void)vm;
    fake.getenv_version = version;
    if (fake.getenv_fails) {
        *penv = NULL;
        return JNI_EDETACHED;
    }
    *penv = (void*)&env_ptr;
    return JNI_OK;
}

static struct JNIInvokeInterface_ vm_table;
static const struct JNIInvokeInterface_* vm_ptr = &vm_table;

typedef jint (*OnLoadFn)(JavaVM*, void*);
static OnLoadFn on_load;
typedef void (*OnCreateFn)(void*, void*, size_t);
static OnCreateFn on_create;
static const char* proxy_path;

static void reset(const char* name) {
    current_case = name;
    memset(&fake, 0, sizeof fake);
}

static jint run(void) {
    fake.exception_pending = fake.pending_on_entry;
    return on_load((JavaVM*)&vm_ptr, NULL);
}

static void check_request(void) {
    CHECK(fake.getenv_version == JNI_VERSION_1_6);
    CHECK(strcmp(fake.class_name, "com/zettabridge/core/ZBridge") == 0);
    CHECK(strcmp(fake.method_name, "onProxyLoaded") == 0);
    CHECK(strcmp(fake.method_sig, "(Ljava/lang/String;)I") == 0);
    CHECK(strcmp(fake.string_value, proxy_path) == 0);
}

static void check_clean(void) {
    CHECK(fake.violations == 0);
    CHECK(fake.deleted_unknown == 0);
    CHECK(fake.live_refs == 0);
}

static void test_success_versions(void) {
    static const struct {
        const char* name;
        jint reported;
        jint expected;
    } cases[] = {
        {"reported 0 defaults to 1.6", 0, JNI_VERSION_1_6},
        {"reported 1.2", JNI_VERSION_1_2, JNI_VERSION_1_2},
        {"reported 1.4", JNI_VERSION_1_4, JNI_VERSION_1_4},
        {"reported 1.6", JNI_VERSION_1_6, JNI_VERSION_1_6},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        reset(cases[i].name);
        fake.call_result = cases[i].reported;
        CHECK(run() == cases[i].expected);
        check_request();
        CHECK(fake.static_int_calls == 1);
        CHECK(fake.call_method == (jmethodID)&method_token);
        CHECK(ref_index(fake.call_class) >= 0);
        CHECK(ref_index(fake.call_arg) >= 0);
        CHECK(fake.call_class != (jclass)fake.call_arg);
        CHECK(!fake.exception_pending);
        check_clean();
    }
}

static void test_unsupported_versions(void) {
    static const struct {
        const char* name;
        jint reported;
    } cases[] = {
        {"reported 1.1 is rejected by ART", 0x00010001},
        {"reported unknown version", 0x00010008},
        {"reported JNI_ERR without exception", JNI_ERR},
        {"reported negative", -7},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        reset(cases[i].name);
        fake.call_result = cases[i].reported;
        CHECK(run() == JNI_ERR);
        check_request();
        CHECK(!fake.exception_pending);
        check_clean();
    }
}

static void test_call_throws(void) {
    reset("onProxyLoaded throws");
    fake.call_throws = 1;
    fake.call_result = JNI_VERSION_1_6;  // A value next to the exception must not count.
    CHECK(run() == JNI_ERR);
    check_request();
    CHECK(fake.exception_pending);  // Left for System.loadLibrary.
    check_clean();
}

static void test_get_env_fails(void) {
    reset("GetEnv fails");
    fake.getenv_fails = 1;
    CHECK(run() == JNI_ERR);
    CHECK(fake.find_class_calls == 0);
    CHECK(fake.static_int_calls == 0);
    check_clean();
}

static void test_pending_on_entry(void) {
    reset("exception pending on entry");
    fake.pending_on_entry = 1;
    CHECK(run() == JNI_ERR);
    CHECK(fake.find_class_calls == 0);
    CHECK(fake.exception_pending);
    check_clean();
}

static void test_find_class_fails(void) {
    reset("FindClass fails");
    fake.find_class_fails = 1;
    CHECK(run() == JNI_ERR);
    CHECK(fake.method_calls == 0 && fake.new_string_calls == 0 && fake.static_int_calls == 0);
    CHECK(fake.exception_pending);
    check_clean();
}

static void test_method_fails(void) {
    reset("GetStaticMethodID fails");
    fake.method_fails = 1;
    CHECK(run() == JNI_ERR);
    CHECK(fake.new_string_calls == 0 && fake.static_int_calls == 0);
    CHECK(fake.exception_pending);
    check_clean();
}

static void test_new_string_fails(void) {
    reset("NewStringUTF fails");
    fake.new_string_fails = 1;
    CHECK(run() == JNI_ERR);
    CHECK(fake.static_int_calls == 0);
    CHECK(fake.exception_pending);
    check_clean();
}

// ANativeActivity_onCreate is the one export android.app.NativeActivity looks for. It arrives
// with no JavaVM of its own, so the proxy must have kept the one JNI_OnLoad was given.
static void test_native_activity(void) {
    reset("ANativeActivity_onCreate before JNI_OnLoad is impossible here (JNI_OnLoad already ran)");
    fake.call_result = JNI_VERSION_1_6;
    CHECK(run() == JNI_VERSION_1_6);

    reset("native activity accepted");
    fake.boolean_result = JNI_TRUE;
    on_create((void*)0x1234, (void*)0x5678, 9);
    CHECK(fake.getenv_version == JNI_VERSION_1_6);
    CHECK(strcmp(fake.class_name, "com/zettabridge/core/ZBridge") == 0);
    CHECK(strcmp(fake.method_name, "onNativeActivityCreated") == 0);
    CHECK(strcmp(fake.method_sig, "(JJJLjava/lang/String;)Z") == 0);
    CHECK(fake.static_boolean_calls == 1);
    CHECK(fake.activity == 0x1234 && fake.saved_state == 0x5678 && fake.saved_state_size == 9);
    CHECK(strcmp(fake.string_value, proxy_path) == 0);
    CHECK(fake.live_refs == 0 && fake.violations == 0);

    // A refusal is reported, not thrown: the framework then fails the activity its own way.
    reset("native activity refused");
    fake.boolean_result = JNI_FALSE;
    on_create((void*)0x1000, NULL, 0);
    CHECK(fake.static_boolean_calls == 1 && fake.live_refs == 0 && !fake.exception_pending);

    // An exception from the bridge is cleared here; leaving it pending would surface at an
    // unrelated JNI call much later.
    reset("native activity throws");
    fake.call_throws = 1;
    on_create((void*)0x1000, NULL, 0);
    CHECK(fake.static_boolean_calls == 1 && !fake.exception_pending);
    CHECK(fake.live_refs == 0 && fake.violations == 0);

    reset("native activity with GetEnv failing");
    fake.getenv_fails = 1;
    on_create((void*)0x1000, NULL, 0);
    CHECK(fake.static_boolean_calls == 0 && fake.find_class_calls == 0);

    reset("native activity with an exception already pending");
    fake.pending_on_entry = 1;
    fake.exception_pending = 1;
    on_create((void*)0x1000, NULL, 0);
    CHECK(fake.find_class_calls == 0 && fake.static_boolean_calls == 0);
}

int main(int argc, char** argv) {
    if (argc != 2 || argv[1][0] != '/') {
        fprintf(stderr, "usage: %s /absolute/path/to/proxy.so\n", argv[0]);
        return 2;
    }
    proxy_path = argv[1];

    env_table.FindClass = fake_find_class;
    env_table.GetStaticMethodID = fake_get_static_method_id;
    env_table.NewStringUTF = fake_new_string_utf;
    env_table.CallStaticIntMethod = fake_call_static_int_method;
    env_table.CallStaticBooleanMethod = fake_call_static_boolean_method;
    env_table.DeleteLocalRef = fake_delete_local_ref;
    env_table.ExceptionCheck = fake_exception_check;
    env_table.ExceptionClear = fake_exception_clear;
    vm_table.GetEnv = fake_get_env;

    void* handle = dlopen(proxy_path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        fprintf(stderr, "dlopen %s: %s\n", proxy_path, dlerror());
        return 1;
    }
    on_load = (OnLoadFn)dlsym(handle, "JNI_OnLoad");
    CHECK(on_load != NULL);
    on_create = (OnCreateFn)dlsym(handle, "ANativeActivity_onCreate");
    CHECK(on_create != NULL);

    test_success_versions();
    test_unsupported_versions();
    test_call_throws();
    test_get_env_fails();
    test_pending_on_entry();
    test_find_class_fails();
    test_method_fails();
    test_new_string_fails();
    test_native_activity();

    printf("zbproxy fake JNI: all cases passed\n");
    return 0;
}
