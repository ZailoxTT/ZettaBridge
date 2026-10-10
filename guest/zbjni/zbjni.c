/* Guest JNIEnv and JavaVM (libzbjni.so), preloaded RTLD_GLOBAL by zbhost.
 *
 * Every JNINativeInterface slot is a guest C function here unless tools/gen_jni.py configures it
 * as a host stub. Most slots forward to one flat host call (gen/hostcalls.h), which the host
 * serves in core/src/jni/host_jni*.cpp against its JniBackend. Handles, method ids and field ids
 * are opaque 32-bit values issued by the host.
 *
 * Done here, in guest code:
 * - Call*Method with ... and va_list: converted to a jvalue array by the method shorty, which the
 *   host returns with each method id and which is cached per id;
 * - Get*ArrayElements, GetPrimitiveArrayCritical, GetString*Chars, GetStringCritical: guest
 *   malloc copies filled through region host calls (isCopy = JNI_TRUE), released per mode;
 * - GetVersion, DefineClass, GetJavaVM, DestroyJavaVM. */
#include <jni.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zb/jni_hostcalls.h"
#include "zb/jni_protocol.h"
#include "gen/hostcalls.h"

_Static_assert(sizeof(void*) == 4, "libzbjni.so is arm32 guest code");
_Static_assert(sizeof(jvalue) == 8, "jvalue");
_Static_assert(sizeof(JNINativeMethod) == sizeof(struct zb_jni_native_method), "JNINativeMethod");
_Static_assert(sizeof(JavaVMAttachArgs) == sizeof(struct zb_jni_attach_args), "JavaVMAttachArgs");

#define U(value) ((uint32_t)(uintptr_t)(value))
#define P(type, value) ((type)(uintptr_t)(value))

/* ---- Method shorty cache ------------------------------------------------------------------ */

/* Guest method ids are dense and start at 1. The cache is a lock-free two-level table; an id
 * beyond it asks the host every time. Entries are written once and never freed. */
#define ZBJNI_SHORTY_PAGE_BITS 10u
#define ZBJNI_SHORTY_PAGE_SIZE (1u << ZBJNI_SHORTY_PAGE_BITS)
#define ZBJNI_SHORTY_PAGES 4096u

typedef _Atomic(const char*) zbjni_shorty_entry;
static _Atomic(zbjni_shorty_entry*) zbjni_shorty_pages[ZBJNI_SHORTY_PAGES];

static const char* zbjni_cached_shorty(uint32_t id) {
    const uint32_t page = id >> ZBJNI_SHORTY_PAGE_BITS;
    if (page >= ZBJNI_SHORTY_PAGES) return NULL;
    zbjni_shorty_entry* entries = atomic_load(&zbjni_shorty_pages[page]);
    return entries != NULL ? atomic_load(&entries[id & (ZBJNI_SHORTY_PAGE_SIZE - 1)]) : NULL;
}

static const char* zbjni_cache_shorty(uint32_t id, const char* shorty) {
    const uint32_t page = id >> ZBJNI_SHORTY_PAGE_BITS;
    if (page >= ZBJNI_SHORTY_PAGES) return shorty;
    zbjni_shorty_entry* entries = atomic_load(&zbjni_shorty_pages[page]);
    if (entries == NULL) {
        zbjni_shorty_entry* fresh = calloc(ZBJNI_SHORTY_PAGE_SIZE, sizeof *fresh);
        if (fresh == NULL) return shorty;
        zbjni_shorty_entry* expected = NULL;
        if (atomic_compare_exchange_strong(&zbjni_shorty_pages[page], &expected, fresh)) {
            entries = fresh;
        } else {
            free(fresh);
            entries = expected;
        }
    }
    zbjni_shorty_entry* entry = &entries[id & (ZBJNI_SHORTY_PAGE_SIZE - 1)];
    const char* current = atomic_load(entry);
    if (current != NULL) return current;
    char* copy = strdup(shorty);
    if (copy == NULL) return shorty;
    const char* expected = NULL;
    if (atomic_compare_exchange_strong(entry, &expected, copy)) return copy;
    free(copy);
    return expected;
}

/* The shorty of a method id. buffer (ZB_JNI_SHORTY_SIZE) receives it when the cache cannot
 * hold it. NULL for an id the host does not know (the host has reported it). */
static const char* zbjni_method_shorty(jmethodID method, char* buffer) {
    const uint32_t id = U(method);
    const char* cached = zbjni_cached_shorty(id);
    if (cached != NULL) return cached;
    if (zbjni_hc_GetMethodShorty(id, U(buffer)) == 0) return NULL;
    return zbjni_cache_shorty(id, buffer);
}

/* ---- Value conversions -------------------------------------------------------------------- */

static jobject zbjni_to_L(uint64_t value) { return P(jobject, (uint32_t)value); }
static jboolean zbjni_to_Z(uint64_t value) { return (jboolean)value; }
static jbyte zbjni_to_B(uint64_t value) { return (jbyte)value; }
static jchar zbjni_to_C(uint64_t value) { return (jchar)value; }
static jshort zbjni_to_S(uint64_t value) { return (jshort)value; }
static jint zbjni_to_I(uint64_t value) { return (jint)value; }
static jlong zbjni_to_J(uint64_t value) { return (jlong)value; }

static jfloat zbjni_to_F(uint64_t value) {
    const uint32_t bits = (uint32_t)value;
    jfloat result;
    memcpy(&result, &bits, sizeof result);
    return result;
}

static jdouble zbjni_to_D(uint64_t value) {
    jdouble result;
    memcpy(&result, &value, sizeof result);
    return result;
}

static uint64_t zbjni_bits_L(jobject value) { return U(value); }
static uint64_t zbjni_bits_Z(jboolean value) { return value; }
static uint64_t zbjni_bits_B(jbyte value) { return (uint8_t)value; }
static uint64_t zbjni_bits_C(jchar value) { return value; }
static uint64_t zbjni_bits_S(jshort value) { return (uint16_t)value; }
static uint64_t zbjni_bits_I(jint value) { return (uint32_t)value; }
static uint64_t zbjni_bits_J(jlong value) { return (uint64_t)value; }

static uint64_t zbjni_bits_F(jfloat value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static uint64_t zbjni_bits_D(jdouble value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

/* ---- Version, classes, reflection ---------------------------------------------------------- */

static jint zbjni_GetVersion(JNIEnv* env) {
    return JNI_VERSION_1_6;
}

/* Android does not support DefineClass; ART also returns NULL. */
static jclass zbjni_DefineClass(JNIEnv* env, const char* name, jobject loader, const jbyte* buf, jsize len) {
    return NULL;
}

static jclass zbjni_FindClass(JNIEnv* env, const char* name) {
    return P(jclass, zbjni_hc_FindClass(U(name)));
}

static jmethodID zbjni_FromReflectedMethod(JNIEnv* env, jobject method) {
    char shorty[ZB_JNI_SHORTY_SIZE];
    const uint32_t id = zbjni_hc_FromReflectedMethod(U(method), U(shorty));
    if (id != 0) zbjni_cache_shorty(id, shorty);
    return P(jmethodID, id);
}

static jfieldID zbjni_FromReflectedField(JNIEnv* env, jobject field) {
    return P(jfieldID, zbjni_hc_FromReflectedField(U(field)));
}

static jobject zbjni_ToReflectedMethod(JNIEnv* env, jclass cls, jmethodID method, jboolean is_static) {
    return P(jobject, zbjni_hc_ToReflectedMethod(U(cls), U(method), is_static));
}

static jclass zbjni_GetSuperclass(JNIEnv* env, jclass cls) {
    return P(jclass, zbjni_hc_GetSuperclass(U(cls)));
}

static jboolean zbjni_IsAssignableFrom(JNIEnv* env, jclass from, jclass to) {
    return (jboolean)zbjni_hc_IsAssignableFrom(U(from), U(to));
}

static jobject zbjni_ToReflectedField(JNIEnv* env, jclass cls, jfieldID field, jboolean is_static) {
    return P(jobject, zbjni_hc_ToReflectedField(U(cls), U(field), is_static));
}

/* ---- Exceptions ---------------------------------------------------------------------------- */

static jint zbjni_Throw(JNIEnv* env, jthrowable throwable) {
    return (jint)zbjni_hc_Throw(U(throwable));
}

static jint zbjni_ThrowNew(JNIEnv* env, jclass cls, const char* message) {
    return (jint)zbjni_hc_ThrowNew(U(cls), U(message));
}

static jthrowable zbjni_ExceptionOccurred(JNIEnv* env) {
    return P(jthrowable, zbjni_hc_ExceptionOccurred());
}

static void zbjni_ExceptionDescribe(JNIEnv* env) {
    zbjni_hc_ExceptionDescribe();
}

static void zbjni_ExceptionClear(JNIEnv* env) {
    zbjni_hc_ExceptionClear();
}

static void zbjni_FatalError(JNIEnv* env, const char* message) {
    zbjni_hc_FatalError(U(message));
    abort();
}

static jboolean zbjni_ExceptionCheck(JNIEnv* env) {
    return (jboolean)zbjni_hc_ExceptionCheck();
}

/* ---- References ---------------------------------------------------------------------------- */

static jint zbjni_PushLocalFrame(JNIEnv* env, jint capacity) {
    return (jint)zbjni_hc_PushLocalFrame((uint32_t)capacity);
}

static jobject zbjni_PopLocalFrame(JNIEnv* env, jobject result) {
    return P(jobject, zbjni_hc_PopLocalFrame(U(result)));
}

static jobject zbjni_NewGlobalRef(JNIEnv* env, jobject obj) {
    return P(jobject, zbjni_hc_NewGlobalRef(U(obj)));
}

static void zbjni_DeleteGlobalRef(JNIEnv* env, jobject ref) {
    zbjni_hc_DeleteGlobalRef(U(ref));
}

static void zbjni_DeleteLocalRef(JNIEnv* env, jobject ref) {
    zbjni_hc_DeleteLocalRef(U(ref));
}

static jboolean zbjni_IsSameObject(JNIEnv* env, jobject a, jobject b) {
    return (jboolean)zbjni_hc_IsSameObject(U(a), U(b));
}

static jobject zbjni_NewLocalRef(JNIEnv* env, jobject obj) {
    return P(jobject, zbjni_hc_NewLocalRef(U(obj)));
}

static jint zbjni_EnsureLocalCapacity(JNIEnv* env, jint capacity) {
    return (jint)zbjni_hc_EnsureLocalCapacity((uint32_t)capacity);
}

static jweak zbjni_NewWeakGlobalRef(JNIEnv* env, jobject obj) {
    return P(jweak, zbjni_hc_NewWeakGlobalRef(U(obj)));
}

static void zbjni_DeleteWeakGlobalRef(JNIEnv* env, jweak ref) {
    zbjni_hc_DeleteWeakGlobalRef(U(ref));
}

static jobjectRefType zbjni_GetObjectRefType(JNIEnv* env, jobject obj) {
    return (jobjectRefType)zbjni_hc_GetObjectRefType(U(obj));
}

/* ---- Objects ------------------------------------------------------------------------------- */

static jobject zbjni_AllocObject(JNIEnv* env, jclass cls) {
    return P(jobject, zbjni_hc_AllocObject(U(cls)));
}

static jclass zbjni_GetObjectClass(JNIEnv* env, jobject obj) {
    return P(jclass, zbjni_hc_GetObjectClass(U(obj)));
}

static jboolean zbjni_IsInstanceOf(JNIEnv* env, jobject obj, jclass cls) {
    return (jboolean)zbjni_hc_IsInstanceOf(U(obj), U(cls));
}

static jmethodID zbjni_get_method_id(jclass cls, const char* name, const char* signature, uint32_t is_static) {
    char shorty[ZB_JNI_SHORTY_SIZE];
    const uint32_t id = zbjni_hc_GetMethodID(U(cls), U(name), U(signature), is_static, U(shorty));
    if (id != 0) zbjni_cache_shorty(id, shorty);
    return P(jmethodID, id);
}

static jmethodID zbjni_GetMethodID(JNIEnv* env, jclass cls, const char* name, const char* signature) {
    return zbjni_get_method_id(cls, name, signature, 0);
}

static jmethodID zbjni_GetStaticMethodID(JNIEnv* env, jclass cls, const char* name, const char* signature) {
    return zbjni_get_method_id(cls, name, signature, 1);
}

static jfieldID zbjni_GetFieldID(JNIEnv* env, jclass cls, const char* name, const char* signature) {
    return P(jfieldID, zbjni_hc_GetFieldID(U(cls), U(name), U(signature), 0));
}

static jfieldID zbjni_GetStaticFieldID(JNIEnv* env, jclass cls, const char* name, const char* signature) {
    return P(jfieldID, zbjni_hc_GetFieldID(U(cls), U(name), U(signature), 1));
}

/* ---- Calls --------------------------------------------------------------------------------- */

/* One CallMethodA host call for every ... and va_list form. */
static uint64_t zbjni_call_v(uint32_t kind, char type, jobject obj, jclass cls, jmethodID method, va_list ap) {
    char buffer[ZB_JNI_SHORTY_SIZE];
    const char* shorty = zbjni_method_shorty(method, buffer);
    const size_t count = shorty != NULL ? strlen(shorty) - 1 : 0;
    jvalue args[count != 0 ? count : 1];
    for (size_t i = 0; i < count; ++i) {
        switch (shorty[i + 1]) {
        case 'Z': args[i].z = (jboolean)va_arg(ap, int); break;
        case 'B': args[i].b = (jbyte)va_arg(ap, int); break;
        case 'C': args[i].c = (jchar)va_arg(ap, int); break;
        case 'S': args[i].s = (jshort)va_arg(ap, int); break;
        case 'I': args[i].i = va_arg(ap, jint); break;
        case 'J': args[i].j = va_arg(ap, jlong); break;
        case 'F': args[i].f = (jfloat)va_arg(ap, double); break;
        case 'D': args[i].d = va_arg(ap, jdouble); break;
        default: args[i].l = va_arg(ap, jobject); break;
        }
    }
    /* An unknown id reaches the host with no arguments; the host reports it. */
    return zbjni_hc_CallMethodA(kind, (uint32_t)type, U(obj), U(cls), U(method), shorty != NULL ? U(args) : 0);
}

#define ZBJNI_CALLS(Name, jtype, letter)                                                                    \
    static jtype zbjni_Call##Name##MethodV(JNIEnv* env, jobject obj, jmethodID method, va_list ap) {        \
        return zbjni_to_##letter(zbjni_call_v(ZB_JNI_CALL_VIRTUAL, #letter[0], obj, NULL, method, ap));     \
    }                                                                                                       \
    static jtype zbjni_Call##Name##Method(JNIEnv* env, jobject obj, jmethodID method, ...) {                \
        va_list ap;                                                                                         \
        va_start(ap, method);                                                                               \
        const jtype result = zbjni_Call##Name##MethodV(env, obj, method, ap);                              \
        va_end(ap);                                                                                         \
        return result;                                                                                      \
    }                                                                                                       \
    static jtype zbjni_Call##Name##MethodA(JNIEnv* env, jobject obj, jmethodID method, const jvalue* args) { \
        return zbjni_to_##letter(                                                                           \
            zbjni_hc_CallMethodA(ZB_JNI_CALL_VIRTUAL, #letter[0], U(obj), 0, U(method), U(args)));          \
    }                                                                                                       \
    static jtype zbjni_CallNonvirtual##Name##MethodV(JNIEnv* env, jobject obj, jclass cls, jmethodID method, \
                                                     va_list ap) {                                          \
        return zbjni_to_##letter(zbjni_call_v(ZB_JNI_CALL_NONVIRTUAL, #letter[0], obj, cls, method, ap));   \
    }                                                                                                       \
    static jtype zbjni_CallNonvirtual##Name##Method(JNIEnv* env, jobject obj, jclass cls, jmethodID method, \
                                                    ...) {                                                  \
        va_list ap;                                                                                         \
        va_start(ap, method);                                                                               \
        const jtype result = zbjni_CallNonvirtual##Name##MethodV(env, obj, cls, method, ap);               \
        va_end(ap);                                                                                         \
        return result;                                                                                      \
    }                                                                                                       \
    static jtype zbjni_CallNonvirtual##Name##MethodA(JNIEnv* env, jobject obj, jclass cls, jmethodID method, \
                                                     const jvalue* args) {                                  \
        return zbjni_to_##letter(                                                                           \
            zbjni_hc_CallMethodA(ZB_JNI_CALL_NONVIRTUAL, #letter[0], U(obj), U(cls), U(method), U(args)));  \
    }                                                                                                       \
    static jtype zbjni_CallStatic##Name##MethodV(JNIEnv* env, jclass cls, jmethodID method, va_list ap) {   \
        return zbjni_to_##letter(zbjni_call_v(ZB_JNI_CALL_STATIC, #letter[0], NULL, cls, method, ap));      \
    }                                                                                                       \
    static jtype zbjni_CallStatic##Name##Method(JNIEnv* env, jclass cls, jmethodID method, ...) {           \
        va_list ap;                                                                                         \
        va_start(ap, method);                                                                               \
        const jtype result = zbjni_CallStatic##Name##MethodV(env, cls, method, ap);                        \
        va_end(ap);                                                                                         \
        return result;                                                                                      \
    }                                                                                                       \
    static jtype zbjni_CallStatic##Name##MethodA(JNIEnv* env, jclass cls, jmethodID method,                \
                                                 const jvalue* args) {                                      \
        return zbjni_to_##letter(                                                                           \
            zbjni_hc_CallMethodA(ZB_JNI_CALL_STATIC, #letter[0], 0, U(cls), U(method), U(args)));           \
    }

ZBJNI_CALLS(Object, jobject, L)
ZBJNI_CALLS(Boolean, jboolean, Z)
ZBJNI_CALLS(Byte, jbyte, B)
ZBJNI_CALLS(Char, jchar, C)
ZBJNI_CALLS(Short, jshort, S)
ZBJNI_CALLS(Int, jint, I)
ZBJNI_CALLS(Long, jlong, J)
ZBJNI_CALLS(Float, jfloat, F)
ZBJNI_CALLS(Double, jdouble, D)

static void zbjni_CallVoidMethodV(JNIEnv* env, jobject obj, jmethodID method, va_list ap) {
    zbjni_call_v(ZB_JNI_CALL_VIRTUAL, 'V', obj, NULL, method, ap);
}

static void zbjni_CallVoidMethod(JNIEnv* env, jobject obj, jmethodID method, ...) {
    va_list ap;
    va_start(ap, method);
    zbjni_CallVoidMethodV(env, obj, method, ap);
    va_end(ap);
}

static void zbjni_CallVoidMethodA(JNIEnv* env, jobject obj, jmethodID method, const jvalue* args) {
    zbjni_hc_CallMethodA(ZB_JNI_CALL_VIRTUAL, 'V', U(obj), 0, U(method), U(args));
}

static void zbjni_CallNonvirtualVoidMethodV(JNIEnv* env, jobject obj, jclass cls, jmethodID method, va_list ap) {
    zbjni_call_v(ZB_JNI_CALL_NONVIRTUAL, 'V', obj, cls, method, ap);
}

static void zbjni_CallNonvirtualVoidMethod(JNIEnv* env, jobject obj, jclass cls, jmethodID method, ...) {
    va_list ap;
    va_start(ap, method);
    zbjni_CallNonvirtualVoidMethodV(env, obj, cls, method, ap);
    va_end(ap);
}

static void zbjni_CallNonvirtualVoidMethodA(JNIEnv* env, jobject obj, jclass cls, jmethodID method,
                                            const jvalue* args) {
    zbjni_hc_CallMethodA(ZB_JNI_CALL_NONVIRTUAL, 'V', U(obj), U(cls), U(method), U(args));
}

static void zbjni_CallStaticVoidMethodV(JNIEnv* env, jclass cls, jmethodID method, va_list ap) {
    zbjni_call_v(ZB_JNI_CALL_STATIC, 'V', NULL, cls, method, ap);
}

static void zbjni_CallStaticVoidMethod(JNIEnv* env, jclass cls, jmethodID method, ...) {
    va_list ap;
    va_start(ap, method);
    zbjni_CallStaticVoidMethodV(env, cls, method, ap);
    va_end(ap);
}

static void zbjni_CallStaticVoidMethodA(JNIEnv* env, jclass cls, jmethodID method, const jvalue* args) {
    zbjni_hc_CallMethodA(ZB_JNI_CALL_STATIC, 'V', 0, U(cls), U(method), U(args));
}

static jobject zbjni_NewObjectV(JNIEnv* env, jclass cls, jmethodID method, va_list ap) {
    return zbjni_to_L(zbjni_call_v(ZB_JNI_CALL_NEW_OBJECT, 'L', NULL, cls, method, ap));
}

static jobject zbjni_NewObject(JNIEnv* env, jclass cls, jmethodID method, ...) {
    va_list ap;
    va_start(ap, method);
    const jobject result = zbjni_NewObjectV(env, cls, method, ap);
    va_end(ap);
    return result;
}

static jobject zbjni_NewObjectA(JNIEnv* env, jclass cls, jmethodID method, const jvalue* args) {
    return zbjni_to_L(zbjni_hc_CallMethodA(ZB_JNI_CALL_NEW_OBJECT, 'L', 0, U(cls), U(method), U(args)));
}

/* ---- Fields -------------------------------------------------------------------------------- */

#define ZBJNI_FIELDS(Name, jtype, letter)                                                                  \
    static jtype zbjni_Get##Name##Field(JNIEnv* env, jobject obj, jfieldID field) {                        \
        return zbjni_to_##letter(zbjni_hc_GetField(0, #letter[0], U(obj), U(field)));                      \
    }                                                                                                      \
    static void zbjni_Set##Name##Field(JNIEnv* env, jobject obj, jfieldID field, jtype value) {           \
        const uint64_t bits = zbjni_bits_##letter(value);                                                  \
        zbjni_hc_SetField(0, #letter[0], U(obj), U(field), (uint32_t)bits, (uint32_t)(bits >> 32));        \
    }                                                                                                      \
    static jtype zbjni_GetStatic##Name##Field(JNIEnv* env, jclass cls, jfieldID field) {                   \
        return zbjni_to_##letter(zbjni_hc_GetField(1, #letter[0], U(cls), U(field)));                      \
    }                                                                                                      \
    static void zbjni_SetStatic##Name##Field(JNIEnv* env, jclass cls, jfieldID field, jtype value) {      \
        const uint64_t bits = zbjni_bits_##letter(value);                                                  \
        zbjni_hc_SetField(1, #letter[0], U(cls), U(field), (uint32_t)bits, (uint32_t)(bits >> 32));        \
    }

ZBJNI_FIELDS(Object, jobject, L)
ZBJNI_FIELDS(Boolean, jboolean, Z)
ZBJNI_FIELDS(Byte, jbyte, B)
ZBJNI_FIELDS(Char, jchar, C)
ZBJNI_FIELDS(Short, jshort, S)
ZBJNI_FIELDS(Int, jint, I)
ZBJNI_FIELDS(Long, jlong, J)
ZBJNI_FIELDS(Float, jfloat, F)
ZBJNI_FIELDS(Double, jdouble, D)

/* ---- Guest copies of Java data ------------------------------------------------------------- */

/* Every buffer handed out by Get*Elements / Get*Chars / Get*Critical starts with this header.
 * 16 bytes keep the payload aligned for jlong and jdouble. */
#define ZBJNI_BUFFER_MAGIC 0x5a424a42u /* "BJBZ" */

struct zbjni_buffer {
    uint32_t magic;
    int32_t length; /* elements */
    uint32_t type;  /* element letter; 'U' for modified UTF-8 */
    uint32_t reserved;
};

_Static_assert(sizeof(struct zbjni_buffer) == 16, "zbjni_buffer");

static size_t zbjni_element_size(char type) {
    switch (type) {
    case 'Z': case 'B': case 'U': return 1;
    case 'C': case 'S': return 2;
    case 'I': case 'F': return 4;
    case 'J': case 'D': return 8;
    default: return 0;
    }
}

/* length elements plus one zero element of slack (a terminator for strings). */
static void* zbjni_buffer_new(char type, jsize length) {
    const size_t size = zbjni_element_size(type);
    if (length < 0 || size == 0) return NULL;
    const uint64_t payload64 = ((uint64_t)length + 1) * (uint64_t)size;
    const uint64_t total64 = (uint64_t)sizeof(struct zbjni_buffer) + payload64;
    if (total64 > SIZE_MAX) return NULL;
    const size_t payload = (size_t)payload64;
    struct zbjni_buffer* header = malloc((size_t)total64);
    if (header == NULL) return NULL;
    header->magic = ZBJNI_BUFFER_MAGIC;
    header->length = length;
    header->type = (uint32_t)type;
    header->reserved = 0;
    memset(header + 1, 0, payload);
    return header + 1;
}

static struct zbjni_buffer* zbjni_buffer_header(const void* data, const char* function) {
    if (data == NULL) return NULL;
    struct zbjni_buffer* header = (struct zbjni_buffer*)((char*)(uintptr_t)data - sizeof(struct zbjni_buffer));
    if (header->magic != ZBJNI_BUFFER_MAGIC) {
        char message[128];
        snprintf(message, sizeof message, "%s: the pointer was not returned by a matching Get function", function);
        zbjni_hc_FatalError(U(message));
        abort();
    }
    return header;
}

static void zbjni_buffer_free(struct zbjni_buffer* header) {
    header->magic = 0;
    free(header);
}

static void* zbjni_get_elements(jarray array, char type, jboolean* is_copy) {
    const jsize length = (jsize)zbjni_hc_GetArrayLength(U(array));
    void* data = zbjni_buffer_new(type, length);
    if (data == NULL) return NULL;
    if (length > 0) zbjni_hc_GetPrimitiveArrayRegion((uint32_t)type, U(array), 0, (uint32_t)length, U(data));
    if (is_copy != NULL) *is_copy = JNI_TRUE;
    return data;
}

/* mode 0: copy back and free; JNI_COMMIT: copy back; JNI_ABORT: free. */
static void zbjni_release_elements(jarray array, void* data, jint mode, const char* function) {
    struct zbjni_buffer* header = zbjni_buffer_header(data, function);
    if (header == NULL) return;
    if (mode != JNI_ABORT && header->length > 0) {
        zbjni_hc_SetPrimitiveArrayRegion(header->type, U(array), 0, (uint32_t)header->length, U(data));
    }
    if (mode != JNI_COMMIT) zbjni_buffer_free(header);
}

/* ---- Strings ------------------------------------------------------------------------------- */

static jstring zbjni_NewString(JNIEnv* env, const jchar* chars, jsize length) {
    return P(jstring, zbjni_hc_NewString(U(chars), (uint32_t)length));
}

static jsize zbjni_GetStringLength(JNIEnv* env, jstring str) {
    return (jsize)zbjni_hc_GetStringLength(U(str));
}

static const jchar* zbjni_GetStringChars(JNIEnv* env, jstring str, jboolean* is_copy) {
    if (str == NULL) return NULL;
    const jsize length = zbjni_GetStringLength(env, str);
    jchar* chars = zbjni_buffer_new('C', length);
    if (chars == NULL) return NULL;
    if (length > 0) zbjni_hc_GetStringRegion(U(str), 0, (uint32_t)length, U(chars));
    if (is_copy != NULL) *is_copy = JNI_TRUE;
    return chars;
}

static void zbjni_ReleaseStringChars(JNIEnv* env, jstring str, const jchar* chars) {
    struct zbjni_buffer* header = zbjni_buffer_header(chars, "ReleaseStringChars");
    if (header != NULL) zbjni_buffer_free(header);
}

static jstring zbjni_NewStringUTF(JNIEnv* env, const char* utf) {
    return P(jstring, zbjni_hc_NewStringUTF(U(utf)));
}

static jsize zbjni_GetStringUTFLength(JNIEnv* env, jstring str) {
    return (jsize)zbjni_hc_GetStringUTFLength(U(str));
}

static const char* zbjni_GetStringUTFChars(JNIEnv* env, jstring str, jboolean* is_copy) {
    if (str == NULL) return NULL;
    const jsize bytes = zbjni_GetStringUTFLength(env, str);
    const jsize length = zbjni_GetStringLength(env, str);
    char* utf = zbjni_buffer_new('U', bytes);
    if (utf == NULL) return NULL;
    /* Writes the bytes and a NUL, which the buffer has room for. */
    zbjni_hc_GetStringUTFRegion(U(str), 0, (uint32_t)length, U(utf));
    if (is_copy != NULL) *is_copy = JNI_TRUE;
    return utf;
}

static void zbjni_ReleaseStringUTFChars(JNIEnv* env, jstring str, const char* utf) {
    struct zbjni_buffer* header = zbjni_buffer_header(utf, "ReleaseStringUTFChars");
    if (header != NULL) zbjni_buffer_free(header);
}

static void zbjni_GetStringRegion(JNIEnv* env, jstring str, jsize start, jsize length, jchar* buf) {
    zbjni_hc_GetStringRegion(U(str), (uint32_t)start, (uint32_t)length, U(buf));
}

static void zbjni_GetStringUTFRegion(JNIEnv* env, jstring str, jsize start, jsize length, char* buf) {
    zbjni_hc_GetStringUTFRegion(U(str), (uint32_t)start, (uint32_t)length, U(buf));
}

static const jchar* zbjni_GetStringCritical(JNIEnv* env, jstring str, jboolean* is_copy) {
    return zbjni_GetStringChars(env, str, is_copy);
}

static void zbjni_ReleaseStringCritical(JNIEnv* env, jstring str, const jchar* chars) {
    struct zbjni_buffer* header = zbjni_buffer_header(chars, "ReleaseStringCritical");
    if (header != NULL) zbjni_buffer_free(header);
}

/* ---- Arrays -------------------------------------------------------------------------------- */

static jsize zbjni_GetArrayLength(JNIEnv* env, jarray array) {
    return (jsize)zbjni_hc_GetArrayLength(U(array));
}

static jobjectArray zbjni_NewObjectArray(JNIEnv* env, jsize length, jclass cls, jobject initial) {
    return P(jobjectArray, zbjni_hc_NewObjectArray((uint32_t)length, U(cls), U(initial)));
}

static jobject zbjni_GetObjectArrayElement(JNIEnv* env, jobjectArray array, jsize index) {
    return P(jobject, zbjni_hc_GetObjectArrayElement(U(array), (uint32_t)index));
}

static void zbjni_SetObjectArrayElement(JNIEnv* env, jobjectArray array, jsize index, jobject value) {
    zbjni_hc_SetObjectArrayElement(U(array), (uint32_t)index, U(value));
}

#define ZBJNI_ARRAYS(Name, jtype, letter)                                                                   \
    static jtype##Array zbjni_New##Name##Array(JNIEnv* env, jsize length) {                                 \
        return P(jtype##Array, zbjni_hc_NewPrimitiveArray(#letter[0], (uint32_t)length));                   \
    }                                                                                                       \
    static jtype* zbjni_Get##Name##ArrayElements(JNIEnv* env, jtype##Array array, jboolean* is_copy) {      \
        return zbjni_get_elements(array, #letter[0], is_copy);                                              \
    }                                                                                                       \
    static void zbjni_Release##Name##ArrayElements(JNIEnv* env, jtype##Array array, jtype* elems,          \
                                                   jint mode) {                                             \
        zbjni_release_elements(array, elems, mode, "Release" #Name "ArrayElements");                        \
    }                                                                                                       \
    static void zbjni_Get##Name##ArrayRegion(JNIEnv* env, jtype##Array array, jsize start, jsize length,   \
                                             jtype* buf) {                                                  \
        zbjni_hc_GetPrimitiveArrayRegion(#letter[0], U(array), (uint32_t)start, (uint32_t)length, U(buf));  \
    }                                                                                                       \
    static void zbjni_Set##Name##ArrayRegion(JNIEnv* env, jtype##Array array, jsize start, jsize length,   \
                                             const jtype* buf) {                                            \
        zbjni_hc_SetPrimitiveArrayRegion(#letter[0], U(array), (uint32_t)start, (uint32_t)length, U(buf));  \
    }

ZBJNI_ARRAYS(Boolean, jboolean, Z)
ZBJNI_ARRAYS(Byte, jbyte, B)
ZBJNI_ARRAYS(Char, jchar, C)
ZBJNI_ARRAYS(Short, jshort, S)
ZBJNI_ARRAYS(Int, jint, I)
ZBJNI_ARRAYS(Long, jlong, J)
ZBJNI_ARRAYS(Float, jfloat, F)
ZBJNI_ARRAYS(Double, jdouble, D)

static void* zbjni_GetPrimitiveArrayCritical(JNIEnv* env, jarray array, jboolean* is_copy) {
    const char type = (char)zbjni_hc_GetArrayElementType(U(array));
    if (zbjni_element_size(type) == 0 || type == 'U') return NULL;
    return zbjni_get_elements(array, type, is_copy);
}

static void zbjni_ReleasePrimitiveArrayCritical(JNIEnv* env, jarray array, void* data, jint mode) {
    zbjni_release_elements(array, data, mode, "ReleasePrimitiveArrayCritical");
}

/* ---- Natives, monitors, VM, direct buffers ------------------------------------------------- */

static jint zbjni_RegisterNatives(JNIEnv* env, jclass cls, const JNINativeMethod* methods, jint count) {
    return (jint)zbjni_hc_RegisterNatives(U(cls), U(methods), (uint32_t)count);
}

static jint zbjni_UnregisterNatives(JNIEnv* env, jclass cls) {
    return (jint)zbjni_hc_UnregisterNatives(U(cls));
}

static jint zbjni_MonitorEnter(JNIEnv* env, jobject obj) {
    return (jint)zbjni_hc_MonitorEnter(U(obj));
}

static jint zbjni_MonitorExit(JNIEnv* env, jobject obj) {
    return (jint)zbjni_hc_MonitorExit(U(obj));
}

static jint zbjni_GetJavaVM(JNIEnv* env, JavaVM** vm);

static jobject zbjni_NewDirectByteBuffer(JNIEnv* env, void* address, jlong capacity) {
    const uint64_t bits = (uint64_t)capacity;
    return P(jobject, zbjni_hc_NewDirectByteBuffer(U(address), (uint32_t)bits, (uint32_t)(bits >> 32)));
}

static void* zbjni_GetDirectBufferAddress(JNIEnv* env, jobject buffer) {
    return P(void*, zbjni_hc_GetDirectBufferAddress(U(buffer)));
}

static jlong zbjni_GetDirectBufferCapacity(JNIEnv* env, jobject buffer) {
    return (jlong)zbjni_hc_GetDirectBufferCapacity(U(buffer));
}

/* ---- JavaVM -------------------------------------------------------------------------------- */

static jint zbjni_DestroyJavaVM(JavaVM* vm) {
    return JNI_ERR;
}

static jint zbjni_AttachCurrentThread(JavaVM* vm, JNIEnv** env, void* args) {
    return (jint)zbjni_hc_AttachCurrentThread(U(env), U(args), 0);
}

static jint zbjni_AttachCurrentThreadAsDaemon(JavaVM* vm, JNIEnv** env, void* args) {
    return (jint)zbjni_hc_AttachCurrentThread(U(env), U(args), 1);
}

static jint zbjni_DetachCurrentThread(JavaVM* vm) {
    return (jint)zbjni_hc_DetachCurrentThread();
}

static jint zbjni_GetEnv(JavaVM* vm, void** env, jint version) {
    if (version != JNI_VERSION_1_1 && version != JNI_VERSION_1_2 && version != JNI_VERSION_1_4 &&
        version != JNI_VERSION_1_6) {
        *env = NULL;
        return JNI_EVERSION;
    }
    const uint32_t current = zbjni_hc_GetEnv();
    *env = P(void*, current);
    return current != 0 ? JNI_OK : JNI_EDETACHED;
}

#include "gen/tables.inc"

static const JavaVM zbjni_java_vm = &zbjni_invoke_interface;

static jint zbjni_GetJavaVM(JNIEnv* env, JavaVM** vm) {
    *vm = (JavaVM*)(uintptr_t)&zbjni_java_vm;
    return JNI_OK;
}

/* JNI invocation API: guest system libraries (the OpenSL ES library) find the VM through this,
 * as they would on a real device. There is exactly one VM. */
__attribute__((visibility("default"))) jint JNI_GetCreatedJavaVMs(JavaVM** vms, jsize size, jsize* count) {
    if (count != NULL) *count = 1;
    if (vms != NULL && size > 0) vms[0] = (JavaVM*)(uintptr_t)&zbjni_java_vm;
    return JNI_OK;
}

static uint32_t zbjni_new_env(void) {
    JNIEnv* env = malloc(sizeof *env);
    if (env != NULL) *env = &zbjni_native_interface;
    return U(env);
}

static void zbjni_free_env(uint32_t env) {
    free(P(JNIEnv*, env));
}

__attribute__((constructor)) static void zbjni_register(void) {
    const struct zb_jni_guest_api api = {
        sizeof(api), ZB_JNI_PROTOCOL_VERSION, U(zbjni_new_env), U(zbjni_free_env), U(&zbjni_java_vm),
    };
    if (zbjni_hc_Register(U(&api)) == 0) {
        fprintf(stderr, "libzbjni.so: the host did not accept the JNI bridge API\n");
    }
}
