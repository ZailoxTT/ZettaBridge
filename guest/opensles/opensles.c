/* Guest libOpenSLES.so: an arm32 OpenSL ES 1.0.1 subset for translated guests.
 *
 * The host's 64-bit OpenSL ES cannot be handed to a 32-bit guest, so this library implements
 * the API inside the guest and reaches the host through the guest JNIEnv:
 *  - buffer-queue PCM players are mixed in software on one guest thread and written to a
 *    single java android.media.AudioTrack;
 *  - Android FD (compressed file) players are backed by android.media.MediaPlayer.
 * Unsupported features fail with SL_RESULT_FEATURE_UNSUPPORTED rather than faking success.
 *
 * Coverage follows what OpenSL ES games of the 2012-2016 NDK era use: engine, output mix,
 * audio player with Play, (Android simple) BufferQueue, Volume, PlaybackRate and Seek. */
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <jni.h>

#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))
#define TAG "zb-opensles"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

#define container_of(ptr, type, member) ((type*)((char*)(ptr) - offsetof(type, member)))

/* ---- interface IDs -------------------------------------------------------------------- */

/* The values only need to be distinct: callers pass these exported pointers back to us. */
#define IID(name, a, b, c, d)                                                              \
    static const struct SLInterfaceID_ name##_value = {a, b, c, 0x9e7b, {0x5a, 0x42, 0, 0, 0, d}};       \
    EXPORT const SLInterfaceID name = &name##_value;
IID(SL_IID_NULL, 0xec7178ec, 0xe5e1, 0x4432, 0x00)
IID(SL_IID_OBJECT, 0x79216360, 0xddd7, 0x11db, 0x01)
IID(SL_IID_ENGINE, 0x8d97c260, 0xddd4, 0x11db, 0x02)
IID(SL_IID_ENGINECAPABILITIES, 0x8320d0a0, 0xddd5, 0x11db, 0x03)
IID(SL_IID_OUTPUTMIX, 0x97750f60, 0xddd7, 0x11db, 0x04)
IID(SL_IID_PLAY, 0xef0bd9c0, 0xddd7, 0x11db, 0x05)
IID(SL_IID_PREFETCHSTATUS, 0x2a41ee80, 0xddd8, 0x11db, 0x06)
IID(SL_IID_PLAYBACKRATE, 0x2e3b2a40, 0xddda, 0x11db, 0x07)
IID(SL_IID_SEEK, 0xd43135a0, 0xddd8, 0x11db, 0x08)
IID(SL_IID_VOLUME, 0x09e8ede0, 0xddde, 0x11db, 0x09)
IID(SL_IID_BUFFERQUEUE, 0x2bc99cc0, 0xddd4, 0x11db, 0x0a)
IID(SL_IID_ANDROIDSIMPLEBUFFERQUEUE, 0x198e4940, 0xc5d7, 0x11df, 0x0b)
IID(SL_IID_EFFECTSEND, 0x56e7d200, 0xddd4, 0x11db, 0x0c)
IID(SL_IID_3DLOCATION, 0x2b878020, 0xddd7, 0x11db, 0x0d)
IID(SL_IID_ENVIRONMENTALREVERB, 0xc2e5d5f0, 0x94bd, 0x4763, 0x0e)
IID(SL_IID_METADATAEXTRACTION, 0xaa5b1f80, 0xddd6, 0x11db, 0x0f)
IID(SL_IID_MUTESOLO, 0x5a28ebe0, 0xddd8, 0x11db, 0x10)
IID(SL_IID_ANDROIDCONFIGURATION, 0x89f6a7e0, 0xbeac, 0x11df, 0x11)

static int iid_eq(SLInterfaceID a, SLInterfaceID b) {
    return a == b || (a && b && memcmp(a, b, sizeof(*a)) == 0);
}

/* ---- JNI access ----------------------------------------------------------------------- */

static JavaVM* g_vm;
static pthread_once_t g_jni_once = PTHREAD_ONCE_INIT;

static void jni_init(void) {
    typedef jint (*get_vms_fn)(JavaVM**, jsize, jsize*);
    get_vms_fn get_vms = (get_vms_fn)dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
    jsize count = 0;
    if (!get_vms || get_vms(&g_vm, 1, &count) != JNI_OK || count < 1) {
        g_vm = NULL;
        LOGW("no JavaVM: audio output disabled");
    }
}

/* JNIEnv for the calling thread, attaching it if needed (the attachment is kept). */
static JNIEnv* jni_env(void) {
    pthread_once(&g_jni_once, jni_init);
    if (!g_vm) return NULL;
    JNIEnv* env = NULL;
    if ((*g_vm)->GetEnv(g_vm, (void**)&env, JNI_VERSION_1_6) == JNI_OK) return env;
    JavaVMAttachArgs args = {JNI_VERSION_1_6, "zb-opensles", NULL};
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, &args) != JNI_OK) return NULL;
    return env;
}

static int jni_failed(JNIEnv* env, const char* what) {
    if (!(*env)->ExceptionCheck(env)) return 0;
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    LOGW("java exception in %s", what);
    return 1;
}

/* ---- objects -------------------------------------------------------------------------- */

enum kind { KIND_ENGINE = 1, KIND_OUTPUTMIX, KIND_PLAYER };

struct object {
    const struct SLObjectItf_* itf;  /* SLObjectItf == &object.itf */
    enum kind kind;
    SLuint32 state;
    slObjectCallback callback;
    void* callback_context;
    SLint32 priority;
    SLboolean preemptable;
};

#define MAX_QUEUE 16

struct player {
    struct object obj;
    const struct SLPlayItf_* play_itf;
    const struct SLAndroidSimpleBufferQueueItf_* bq_itf;
    const struct SLVolumeItf_* volume_itf;
    const struct SLPlaybackRateItf_* rate_itf;
    const struct SLSeekItf_* seek_itf;

    int is_fd;          /* MediaPlayer-backed */
    SLuint32 play_state;
    slPlayCallback play_callback;
    void* play_context;
    SLuint32 event_mask;
    SLmillisecond marker;
    SLmillisecond update_period;

    /* volume */
    SLmillibel level;
    SLboolean mute;
    SLboolean stereo_enabled;
    SLpermille stereo_position;

    /* playback rate */
    SLpermille rate;
    SLuint32 rate_properties;

    /* buffer queue (PCM players) */
    SLuint32 channels;
    SLuint32 sample_rate;  /* Hz */
    SLuint32 bits;
    SLuint32 queue_capacity;
    struct { const void* data; SLuint32 size; } queue[MAX_QUEUE];
    SLuint32 queue_head, queue_count, play_index;
    uint64_t frac_pos;  /* position in the head buffer, frames << 32 */
    uint64_t frames_played;
    slAndroidSimpleBufferQueueCallback bq_callback;
    void* bq_context;
    int busy;           /* mixer is running a callback for this player */
    int dead;
    struct player* next;

    /* MediaPlayer (FD players) */
    jobject media;
    SLboolean looping;
    int ended;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_idle = PTHREAD_COND_INITIALIZER;
static struct player* g_players;
static pthread_t g_mixer;
static int g_mixer_started;

/* ---- object interface ----------------------------------------------------------------- */

static struct object* obj_of(SLObjectItf self) { return container_of(self, struct object, itf); }

static void player_destroy(struct player* p);

static SLresult Object_Realize(SLObjectItf self, SLboolean async) {
    struct object* o = obj_of(self);
    if (o->state == SL_OBJECT_STATE_REALIZED) return SL_RESULT_PRECONDITIONS_VIOLATED;
    o->state = SL_OBJECT_STATE_REALIZED;
    if (async && o->callback) o->callback(self, o->callback_context, SL_OBJECT_EVENT_ASYNC_TERMINATION,
                                          SL_RESULT_SUCCESS, SL_OBJECT_STATE_REALIZED, NULL);
    return SL_RESULT_SUCCESS;
}

static SLresult Object_Resume(SLObjectItf self, SLboolean async) {
    (void)async;
    return obj_of(self)->state == SL_OBJECT_STATE_SUSPENDED ? SL_RESULT_SUCCESS
                                                            : SL_RESULT_PRECONDITIONS_VIOLATED;
}

static SLresult Object_GetState(SLObjectItf self, SLuint32* state) {
    if (!state) return SL_RESULT_PARAMETER_INVALID;
    *state = obj_of(self)->state;
    return SL_RESULT_SUCCESS;
}

static const struct SLEngineItf_ engine_vtbl;
static SLEngineItf g_engine_itf;  /* points at the engine object's engine interface */

struct engine {
    struct object obj;
    const struct SLEngineItf_* engine_itf;
};

static SLresult Object_GetInterface(SLObjectItf self, const SLInterfaceID iid, void* out) {
    struct object* o = obj_of(self);
    if (!out) return SL_RESULT_PARAMETER_INVALID;
    *(void**)out = NULL;
    if (o->state != SL_OBJECT_STATE_REALIZED) return SL_RESULT_PRECONDITIONS_VIOLATED;
    if (iid_eq(iid, SL_IID_OBJECT)) { *(void**)out = &o->itf; return SL_RESULT_SUCCESS; }
    if (o->kind == KIND_ENGINE) {
        struct engine* e = (struct engine*)o;
        if (iid_eq(iid, SL_IID_ENGINE)) { *(void**)out = &e->engine_itf; return SL_RESULT_SUCCESS; }
    } else if (o->kind == KIND_PLAYER) {
        struct player* p = (struct player*)o;
        if (iid_eq(iid, SL_IID_PLAY)) { *(void**)out = &p->play_itf; return SL_RESULT_SUCCESS; }
        if (iid_eq(iid, SL_IID_VOLUME)) { *(void**)out = &p->volume_itf; return SL_RESULT_SUCCESS; }
        if (iid_eq(iid, SL_IID_PLAYBACKRATE)) { *(void**)out = &p->rate_itf; return SL_RESULT_SUCCESS; }
        if (!p->is_fd && (iid_eq(iid, SL_IID_ANDROIDSIMPLEBUFFERQUEUE) || iid_eq(iid, SL_IID_BUFFERQUEUE))) {
            *(void**)out = &p->bq_itf;
            return SL_RESULT_SUCCESS;
        }
        if (p->is_fd && iid_eq(iid, SL_IID_SEEK)) { *(void**)out = &p->seek_itf; return SL_RESULT_SUCCESS; }
    }
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult Object_RegisterCallback(SLObjectItf self, slObjectCallback callback, void* context) {
    struct object* o = obj_of(self);
    o->callback = callback;
    o->callback_context = context;
    return SL_RESULT_SUCCESS;
}

static void Object_AbortAsyncOperation(SLObjectItf self) { (void)self; }

static void Object_Destroy(SLObjectItf self) {
    struct object* o = obj_of(self);
    if (o->kind == KIND_PLAYER) {
        player_destroy((struct player*)o);
        return;
    }
    if (o->kind == KIND_ENGINE && g_engine_itf == (SLEngineItf) & ((struct engine*)o)->engine_itf) g_engine_itf = NULL;
    free(o);
}

static SLresult Object_SetPriority(SLObjectItf self, SLint32 priority, SLboolean preemptable) {
    struct object* o = obj_of(self);
    o->priority = priority;
    o->preemptable = preemptable;
    return SL_RESULT_SUCCESS;
}

static SLresult Object_GetPriority(SLObjectItf self, SLint32* priority, SLboolean* preemptable) {
    struct object* o = obj_of(self);
    if (priority) *priority = o->priority;
    if (preemptable) *preemptable = o->preemptable;
    return SL_RESULT_SUCCESS;
}

static SLresult Object_SetLossOfControlInterfaces(SLObjectItf self, SLint16 n, SLInterfaceID* ids, SLboolean enabled) {
    (void)self; (void)n; (void)ids; (void)enabled;
    return SL_RESULT_SUCCESS;
}

static const struct SLObjectItf_ object_vtbl = {
    Object_Realize, Object_Resume, Object_GetState, Object_GetInterface, Object_RegisterCallback,
    Object_AbortAsyncOperation, Object_Destroy, Object_SetPriority, Object_GetPriority,
    Object_SetLossOfControlInterfaces,
};

static void object_init(struct object* o, enum kind kind) {
    o->itf = &object_vtbl;
    o->kind = kind;
    o->state = SL_OBJECT_STATE_UNREALIZED;
    o->preemptable = SL_BOOLEAN_FALSE;
}

/* ---- mixer ---------------------------------------------------------------------------- */

#define OUT_RATE 48000
#define LIMIT_KNEE 24576
#define LIMIT_RANGE (32767 - LIMIT_KNEE)
#define PERIOD 480  /* frames per mix pass: 10 ms */

static float mb_gain(SLmillibel mb) {
    if (mb <= SL_MILLIBEL_MIN) return 0.0f;
    if (mb >= 0) return 1.0f;
    return powf(10.0f, (float)mb / 2000.0f);
}

static void player_gains(const struct player* p, float* left, float* right) {
    float g = p->mute ? 0.0f : mb_gain(p->level);
    float l = g, r = g;
    if (p->stereo_enabled) {
        float pos = (float)p->stereo_position / 1000.0f;  /* -1 left .. 1 right */
        if (pos > 0) l *= 1.0f - pos;
        if (pos < 0) r *= 1.0f + pos;
    }
    *left = l;
    *right = r;
}

static int32_t sample_at(const struct player* p, const uint8_t* data, SLuint32 frame, int ch) {
    SLuint32 c = p->channels == 1 ? 0 : (SLuint32)ch;
    if (p->bits == 8) return ((int32_t)data[frame * p->channels + c] - 128) << 8;
    return ((const int16_t*)data)[frame * p->channels + c];
}

/* Mixes one player into acc. Returns the number of finished buffers (callbacks to run). */
static int mix_player(struct player* p, int32_t* acc, int frames) {
    int finished = 0;
    float gl, gr;
    player_gains(p, &gl, &gr);
    int32_t il = (int32_t)(gl * 32768.0f), ir = (int32_t)(gr * 32768.0f);
    uint64_t step = ((uint64_t)p->sample_rate * (uint64_t)(p->rate ? p->rate : 1000) << 32) /
                    ((uint64_t)OUT_RATE * 1000u);
    SLuint32 frame_bytes = p->channels * (p->bits / 8);
    for (int i = 0; i < frames && p->queue_count; ) {
        const uint8_t* data = p->queue[p->queue_head].data;
        SLuint32 n = p->queue[p->queue_head].size / frame_bytes;
        SLuint32 f = (SLuint32)(p->frac_pos >> 32);
        if (f >= n) {
            p->queue_head = (p->queue_head + 1) % MAX_QUEUE;
            p->queue_count--;
            p->play_index++;
            p->frac_pos -= (uint64_t)n << 32;
            finished++;
            continue;
        }
        uint32_t t = (uint32_t)(p->frac_pos >> 16) & 0xffff;
        SLuint32 f2 = f + 1 < n ? f + 1 : f;
        int32_t l = sample_at(p, data, f, 0), r = sample_at(p, data, f, 1);
        l += ((sample_at(p, data, f2, 0) - l) * (int32_t)t) >> 16;
        r += ((sample_at(p, data, f2, 1) - r) * (int32_t)t) >> 16;
        acc[2 * i] += (l * il) >> 15;
        acc[2 * i + 1] += (r * ir) >> 15;
        p->frac_pos += step;
        p->frames_played++;
        i++;
    }
    return finished;
}

static void media_poll(JNIEnv* env, struct player* p);

static void* mixer_main(void* unused) {
    (void)unused;
    JNIEnv* env = jni_env();
    jobject track = NULL;
    jshortArray buffer = NULL;
    jmethodID write = NULL;
    if (env) {
        jclass cls = (*env)->FindClass(env, "android/media/AudioTrack");
        if (cls && !jni_failed(env, "FindClass AudioTrack")) {
            jmethodID min = (*env)->GetStaticMethodID(env, cls, "getMinBufferSize", "(III)I");
            jmethodID ctor = (*env)->GetMethodID(env, cls, "<init>", "(IIIIII)V");
            jmethodID play = (*env)->GetMethodID(env, cls, "play", "()V");
            write = (*env)->GetMethodID(env, cls, "write", "([SII)I");
            jint min_bytes = (*env)->CallStaticIntMethod(env, cls, min, OUT_RATE, 12, 2);
            jint bytes = PERIOD * 4 * 4;
            if (min_bytes > bytes) bytes = min_bytes;
            jobject local = (*env)->NewObject(env, cls, ctor, 3, OUT_RATE, 12, 2, bytes, 1);
            if (local && !jni_failed(env, "new AudioTrack")) {
                track = (*env)->NewGlobalRef(env, local);
                (*env)->DeleteLocalRef(env, local);
                (*env)->CallVoidMethod(env, track, play);
                if (jni_failed(env, "AudioTrack.play")) track = NULL;
            }
            jshortArray arr = (*env)->NewShortArray(env, PERIOD * 2);
            if (arr) buffer = (*env)->NewGlobalRef(env, arr);
        }
    }
    LOGI("mixer started: %s", track ? "AudioTrack output" : "silent (timed)");

    int32_t acc[PERIOD * 2];
    int16_t out[PERIOD * 2];
    struct player* due[64];
    int due_count[64];
    unsigned poll_tick = 0;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        memset(acc, 0, sizeof(acc));
        int ndue = 0;
        pthread_mutex_lock(&g_lock);
        for (struct player* p = g_players; p; p = p->next) {
            if (p->is_fd || p->dead || p->play_state != SL_PLAYSTATE_PLAYING) continue;
            int done = mix_player(p, acc, PERIOD);
            if (done && p->bq_callback && ndue < 64) {
                p->busy++;
                due[ndue] = p;
                due_count[ndue++] = done;
            }
        }
        pthread_mutex_unlock(&g_lock);

        for (int i = 0; i < ndue; i++) {
            struct player* p = due[i];
            for (int k = 0; k < due_count[i] && !p->dead; k++)
                p->bq_callback((SLAndroidSimpleBufferQueueItf)&p->bq_itf, p->bq_context);
        }
        if (ndue) {
            pthread_mutex_lock(&g_lock);
            for (int i = 0; i < ndue; i++) due[i]->busy--;
            pthread_cond_broadcast(&g_idle);
            pthread_mutex_unlock(&g_lock);
        }

        if (env && (++poll_tick % 10) == 0) {
            /* End-of-media events for MediaPlayer players, about every 100 ms. */
            pthread_mutex_lock(&g_lock);
            for (struct player* p = g_players; p; p = p->next)
                if (p->is_fd && !p->dead && p->play_state == SL_PLAYSTATE_PLAYING) p->busy++;
            struct player* list = g_players;
            pthread_mutex_unlock(&g_lock);
            for (struct player* p = list; p; p = p->next)
                if (p->is_fd && p->busy) media_poll(env, p);
            pthread_mutex_lock(&g_lock);
            for (struct player* p = g_players; p; p = p->next)
                if (p->is_fd && p->busy) p->busy--;
            pthread_cond_broadcast(&g_idle);
            pthread_mutex_unlock(&g_lock);
        }

        int32_t peak = 0;
        for (int i = 0; i < PERIOD * 2; i++) {
            int32_t v = acc[i];
            int32_t mag = v < 0 ? -v : v;
            if (mag > peak) peak = mag;
            /* Soft knee above -2.5 dBFS: loud mixes bend toward full scale instead of clipping. */
            if (mag > LIMIT_KNEE) {
                int32_t over = mag - LIMIT_KNEE;
                mag = LIMIT_KNEE + (int32_t)((int64_t)over * LIMIT_RANGE / (over + LIMIT_RANGE));
                v = v < 0 ? -mag : mag;
            }
            out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        /* Health line every 10 s: players, how many are playing, and the loudest sample. */
        static int32_t window_peak;
        static unsigned stat_tick;
        if (peak > window_peak) window_peak = peak;
        if (++stat_tick % 1000 == 0) {
            int pcm = 0, playing = 0, fd = 0, fd_playing = 0;
            pthread_mutex_lock(&g_lock);
            for (struct player* p = g_players; p; p = p->next) {
                if (p->is_fd) { fd++; fd_playing += p->play_state == SL_PLAYSTATE_PLAYING; }
                else { pcm++; playing += p->play_state == SL_PLAYSTATE_PLAYING; }
            }
            pthread_mutex_unlock(&g_lock);
            LOGI("mixer: pcm players %d (%d playing), media players %d (%d playing), peak %d",
                 pcm, playing, fd, fd_playing, (int)window_peak);
            window_peak = 0;
        }
        if (track && buffer) {
            (*env)->SetShortArrayRegion(env, buffer, 0, PERIOD * 2, out);
            (*env)->CallIntMethod(env, track, write, buffer, 0, PERIOD * 2);  /* blocks: paces us */
            if (jni_failed(env, "AudioTrack.write")) track = NULL;
        } else {
            next.tv_nsec += (long)((int64_t)PERIOD * 1000000000LL / OUT_RATE);
            while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        }
    }
    return NULL;
}

static void mixer_start_locked(void) {
    if (g_mixer_started) return;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&g_mixer, &attr, mixer_main, NULL) == 0) g_mixer_started = 1;
    pthread_attr_destroy(&attr);
}

/* ---- MediaPlayer (FD players) ----------------------------------------------------------- */

static struct {
    jclass cls;
    jmethodID ctor, set_source, prepare, start, pause, seek, set_looping, set_volume,
        position, duration, is_playing, release;
    jclass pfd;
    jmethodID from_fd, get_fd;
} mp;

static int media_init(JNIEnv* env) {
    if (mp.cls) return 1;
    jclass cls = (*env)->FindClass(env, "android/media/MediaPlayer");
    if (!cls || jni_failed(env, "FindClass MediaPlayer")) return 0;
    jclass pfd = (*env)->FindClass(env, "android/os/ParcelFileDescriptor");
    if (!pfd || jni_failed(env, "FindClass ParcelFileDescriptor")) return 0;
    mp.ctor = (*env)->GetMethodID(env, cls, "<init>", "()V");
    mp.set_source = (*env)->GetMethodID(env, cls, "setDataSource", "(Ljava/io/FileDescriptor;JJ)V");
    mp.prepare = (*env)->GetMethodID(env, cls, "prepare", "()V");
    mp.start = (*env)->GetMethodID(env, cls, "start", "()V");
    mp.pause = (*env)->GetMethodID(env, cls, "pause", "()V");
    mp.seek = (*env)->GetMethodID(env, cls, "seekTo", "(I)V");
    mp.set_looping = (*env)->GetMethodID(env, cls, "setLooping", "(Z)V");
    mp.set_volume = (*env)->GetMethodID(env, cls, "setVolume", "(FF)V");
    mp.position = (*env)->GetMethodID(env, cls, "getCurrentPosition", "()I");
    mp.duration = (*env)->GetMethodID(env, cls, "getDuration", "()I");
    mp.is_playing = (*env)->GetMethodID(env, cls, "isPlaying", "()Z");
    mp.release = (*env)->GetMethodID(env, cls, "release", "()V");
    mp.from_fd = (*env)->GetStaticMethodID(env, pfd, "fromFd", "(I)Landroid/os/ParcelFileDescriptor;");
    mp.get_fd = (*env)->GetMethodID(env, pfd, "getFileDescriptor", "()Ljava/io/FileDescriptor;");
    if (jni_failed(env, "MediaPlayer methods")) return 0;
    mp.pfd = (*env)->NewGlobalRef(env, pfd);
    mp.cls = (*env)->NewGlobalRef(env, cls);
    return 1;
}

static SLresult media_create(struct player* p, int fd, SLAint64 offset, SLAint64 length) {
    JNIEnv* env = jni_env();
    if (!env || !media_init(env)) return SL_RESULT_CONTENT_UNSUPPORTED;
    jobject pfd = (*env)->CallStaticObjectMethod(env, mp.pfd, mp.from_fd, fd);  /* dups fd */
    if (!pfd || jni_failed(env, "ParcelFileDescriptor.fromFd")) return SL_RESULT_CONTENT_UNSUPPORTED;
    jobject jfd = (*env)->CallObjectMethod(env, pfd, mp.get_fd);
    jobject media = (*env)->NewObject(env, mp.cls, mp.ctor);
    if (!jfd || !media || jni_failed(env, "new MediaPlayer")) return SL_RESULT_CONTENT_UNSUPPORTED;
    if (length == SL_DATALOCATOR_ANDROIDFD_USE_FILE_SIZE) {
        off_t end = lseek(fd, 0, SEEK_END);
        length = end > offset ? end - offset : 0;
    }
    (*env)->CallVoidMethod(env, media, mp.set_source, jfd, (jlong)offset, (jlong)length);
    if (jni_failed(env, "MediaPlayer.setDataSource")) return SL_RESULT_CONTENT_UNSUPPORTED;
    (*env)->CallVoidMethod(env, media, mp.prepare);
    if (jni_failed(env, "MediaPlayer.prepare")) return SL_RESULT_CONTENT_UNSUPPORTED;
    p->media = (*env)->NewGlobalRef(env, media);
    (*env)->DeleteLocalRef(env, media);
    (*env)->DeleteLocalRef(env, jfd);
    (*env)->DeleteLocalRef(env, pfd);  /* MediaPlayer keeps its own reference to the data */
    return SL_RESULT_SUCCESS;
}

static void media_apply_volume(struct player* p) {
    JNIEnv* env = jni_env();
    if (!env || !p->media) return;
    float l, r;
    player_gains(p, &l, &r);
    (*env)->CallVoidMethod(env, p->media, mp.set_volume, l, r);
    jni_failed(env, "MediaPlayer.setVolume");
}

static void media_poll(JNIEnv* env, struct player* p) {
    if (!p->media || p->looping || p->ended) return;
    jboolean playing = (*env)->CallBooleanMethod(env, p->media, mp.is_playing);
    if (jni_failed(env, "MediaPlayer.isPlaying") || playing) return;
    p->ended = 1;
    p->play_state = SL_PLAYSTATE_PAUSED;  /* what Android's implementation reports at the end */
    if (p->play_callback && (p->event_mask & SL_PLAYEVENT_HEADATEND))
        p->play_callback((SLPlayItf)&p->play_itf, p->play_context, SL_PLAYEVENT_HEADATEND);
}

/* ---- Play ----------------------------------------------------------------------------- */

static struct player* play_of(SLPlayItf self) { return container_of(self, struct player, play_itf); }

static SLresult Play_SetPlayState(SLPlayItf self, SLuint32 state) {
    struct player* p = play_of(self);
    if (state < SL_PLAYSTATE_STOPPED || state > SL_PLAYSTATE_PLAYING) return SL_RESULT_PARAMETER_INVALID;
    if (p->is_fd) {
        JNIEnv* env = jni_env();
        if (env && p->media) {
            if (state == SL_PLAYSTATE_PLAYING) {
                if (p->ended) {
                    (*env)->CallVoidMethod(env, p->media, mp.seek, 0);
                    p->ended = 0;
                }
                (*env)->CallVoidMethod(env, p->media, mp.start);
            } else if (p->play_state == SL_PLAYSTATE_PLAYING) {
                (*env)->CallVoidMethod(env, p->media, mp.pause);
            }
            if (state == SL_PLAYSTATE_STOPPED) {
                (*env)->CallVoidMethod(env, p->media, mp.seek, 0);
                p->ended = 0;
            }
            jni_failed(env, "MediaPlayer state");
        }
        p->play_state = state;
        return SL_RESULT_SUCCESS;
    }
    pthread_mutex_lock(&g_lock);
    if (state == SL_PLAYSTATE_STOPPED) {
        p->frac_pos = 0;
        p->frames_played = 0;
    }
    p->play_state = state;
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult Play_GetPlayState(SLPlayItf self, SLuint32* state) {
    if (!state) return SL_RESULT_PARAMETER_INVALID;
    *state = play_of(self)->play_state;
    return SL_RESULT_SUCCESS;
}

static SLresult Play_GetDuration(SLPlayItf self, SLmillisecond* ms) {
    struct player* p = play_of(self);
    if (!ms) return SL_RESULT_PARAMETER_INVALID;
    *ms = SL_TIME_UNKNOWN;
    if (p->is_fd && p->media) {
        JNIEnv* env = jni_env();
        if (env) {
            jint d = (*env)->CallIntMethod(env, p->media, mp.duration);
            if (!jni_failed(env, "MediaPlayer.getDuration") && d >= 0) *ms = (SLmillisecond)d;
        }
    }
    return SL_RESULT_SUCCESS;
}

static SLresult Play_GetPosition(SLPlayItf self, SLmillisecond* ms) {
    struct player* p = play_of(self);
    if (!ms) return SL_RESULT_PARAMETER_INVALID;
    *ms = 0;
    if (p->is_fd) {
        JNIEnv* env = jni_env();
        if (env && p->media) {
            jint pos = (*env)->CallIntMethod(env, p->media, mp.position);
            if (!jni_failed(env, "MediaPlayer.getCurrentPosition") && pos >= 0) *ms = (SLmillisecond)pos;
        }
    } else if (p->sample_rate) {
        /* Frames are counted at the output rate. */
        *ms = (SLmillisecond)(p->frames_played * 1000u / OUT_RATE);
    }
    return SL_RESULT_SUCCESS;
}

static SLresult Play_RegisterCallback(SLPlayItf self, slPlayCallback cb, void* context) {
    struct player* p = play_of(self);
    p->play_callback = cb;
    p->play_context = context;
    return SL_RESULT_SUCCESS;
}

static SLresult Play_SetCallbackEventsMask(SLPlayItf self, SLuint32 mask) {
    play_of(self)->event_mask = mask;
    return SL_RESULT_SUCCESS;
}

static SLresult Play_GetCallbackEventsMask(SLPlayItf self, SLuint32* mask) {
    if (!mask) return SL_RESULT_PARAMETER_INVALID;
    *mask = play_of(self)->event_mask;
    return SL_RESULT_SUCCESS;
}

static SLresult Play_SetMarkerPosition(SLPlayItf self, SLmillisecond ms) {
    play_of(self)->marker = ms;
    return SL_RESULT_SUCCESS;
}

static SLresult Play_ClearMarkerPosition(SLPlayItf self) {
    play_of(self)->marker = 0;
    return SL_RESULT_SUCCESS;
}

static SLresult Play_GetMarkerPosition(SLPlayItf self, SLmillisecond* ms) {
    if (!ms) return SL_RESULT_PARAMETER_INVALID;
    *ms = play_of(self)->marker;
    return SL_RESULT_SUCCESS;
}

static SLresult Play_SetPositionUpdatePeriod(SLPlayItf self, SLmillisecond ms) {
    play_of(self)->update_period = ms;
    return SL_RESULT_SUCCESS;
}

static SLresult Play_GetPositionUpdatePeriod(SLPlayItf self, SLmillisecond* ms) {
    if (!ms) return SL_RESULT_PARAMETER_INVALID;
    *ms = play_of(self)->update_period;
    return SL_RESULT_SUCCESS;
}

static const struct SLPlayItf_ play_vtbl = {
    Play_SetPlayState, Play_GetPlayState, Play_GetDuration, Play_GetPosition, Play_RegisterCallback,
    Play_SetCallbackEventsMask, Play_GetCallbackEventsMask, Play_SetMarkerPosition,
    Play_ClearMarkerPosition, Play_GetMarkerPosition, Play_SetPositionUpdatePeriod,
    Play_GetPositionUpdatePeriod,
};

/* ---- buffer queue --------------------------------------------------------------------- */

static struct player* bq_of(SLAndroidSimpleBufferQueueItf self) {
    return container_of(self, struct player, bq_itf);
}

static SLresult BQ_Enqueue(SLAndroidSimpleBufferQueueItf self, const void* buffer, SLuint32 size) {
    struct player* p = bq_of(self);
    if (!buffer || !size) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&g_lock);
    SLresult r = SL_RESULT_BUFFER_INSUFFICIENT;
    if (p->queue_count < p->queue_capacity) {
        SLuint32 slot = (p->queue_head + p->queue_count) % MAX_QUEUE;
        p->queue[slot].data = buffer;
        p->queue[slot].size = size;
        p->queue_count++;
        r = SL_RESULT_SUCCESS;
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

static SLresult BQ_Clear(SLAndroidSimpleBufferQueueItf self) {
    struct player* p = bq_of(self);
    pthread_mutex_lock(&g_lock);
    p->queue_count = 0;
    p->queue_head = 0;
    p->frac_pos = 0;
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult BQ_GetState(SLAndroidSimpleBufferQueueItf self, SLAndroidSimpleBufferQueueState* state) {
    struct player* p = bq_of(self);
    if (!state) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&g_lock);
    state->count = p->queue_count;
    state->index = p->play_index;
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult BQ_RegisterCallback(SLAndroidSimpleBufferQueueItf self, slAndroidSimpleBufferQueueCallback cb,
                                    void* context) {
    struct player* p = bq_of(self);
    if (p->play_state != SL_PLAYSTATE_STOPPED) return SL_RESULT_PRECONDITIONS_VIOLATED;
    pthread_mutex_lock(&g_lock);
    p->bq_callback = cb;
    p->bq_context = context;
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static const struct SLAndroidSimpleBufferQueueItf_ bq_vtbl = {
    BQ_Enqueue, BQ_Clear, BQ_GetState, BQ_RegisterCallback,
};

/* ---- Volume --------------------------------------------------------------------------- */

static struct player* vol_of(SLVolumeItf self) { return container_of(self, struct player, volume_itf); }

static SLresult Vol_SetVolumeLevel(SLVolumeItf self, SLmillibel level) {
    struct player* p = vol_of(self);
    if (level > 0) return SL_RESULT_PARAMETER_INVALID;
    p->level = level;
    if (p->is_fd) media_apply_volume(p);
    return SL_RESULT_SUCCESS;
}

static SLresult Vol_GetVolumeLevel(SLVolumeItf self, SLmillibel* level) {
    if (!level) return SL_RESULT_PARAMETER_INVALID;
    *level = vol_of(self)->level;
    return SL_RESULT_SUCCESS;
}

static SLresult Vol_GetMaxVolumeLevel(SLVolumeItf self, SLmillibel* level) {
    (void)self;
    if (!level) return SL_RESULT_PARAMETER_INVALID;
    *level = 0;
    return SL_RESULT_SUCCESS;
}

static SLresult Vol_SetMute(SLVolumeItf self, SLboolean mute) {
    struct player* p = vol_of(self);
    p->mute = mute ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE;
    if (p->is_fd) media_apply_volume(p);
    return SL_RESULT_SUCCESS;
}

static SLresult Vol_GetMute(SLVolumeItf self, SLboolean* mute) {
    if (!mute) return SL_RESULT_PARAMETER_INVALID;
    *mute = vol_of(self)->mute;
    return SL_RESULT_SUCCESS;
}

static SLresult Vol_EnableStereoPosition(SLVolumeItf self, SLboolean enable) {
    struct player* p = vol_of(self);
    p->stereo_enabled = enable ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE;
    if (p->is_fd) media_apply_volume(p);
    return SL_RESULT_SUCCESS;
}

static SLresult Vol_IsEnabledStereoPosition(SLVolumeItf self, SLboolean* enabled) {
    if (!enabled) return SL_RESULT_PARAMETER_INVALID;
    *enabled = vol_of(self)->stereo_enabled;
    return SL_RESULT_SUCCESS;
}

static SLresult Vol_SetStereoPosition(SLVolumeItf self, SLpermille position) {
    struct player* p = vol_of(self);
    if (position < -1000 || position > 1000) return SL_RESULT_PARAMETER_INVALID;
    p->stereo_position = position;
    if (p->is_fd) media_apply_volume(p);
    return SL_RESULT_SUCCESS;
}

static SLresult Vol_GetStereoPosition(SLVolumeItf self, SLpermille* position) {
    if (!position) return SL_RESULT_PARAMETER_INVALID;
    *position = vol_of(self)->stereo_position;
    return SL_RESULT_SUCCESS;
}

static const struct SLVolumeItf_ volume_vtbl = {
    Vol_SetVolumeLevel, Vol_GetVolumeLevel, Vol_GetMaxVolumeLevel, Vol_SetMute, Vol_GetMute,
    Vol_EnableStereoPosition, Vol_IsEnabledStereoPosition, Vol_SetStereoPosition, Vol_GetStereoPosition,
};

/* ---- PlaybackRate --------------------------------------------------------------------- */

#define RATE_MIN 500
#define RATE_MAX 2000

static struct player* rate_of(SLPlaybackRateItf self) { return container_of(self, struct player, rate_itf); }

static SLresult Rate_SetRate(SLPlaybackRateItf self, SLpermille rate) {
    struct player* p = rate_of(self);
    if (rate < RATE_MIN || rate > RATE_MAX) return SL_RESULT_PARAMETER_INVALID;
    if (p->is_fd && rate != 1000) return SL_RESULT_FEATURE_UNSUPPORTED;
    p->rate = rate;  /* read by the mixer on its next pass; a torn read is harmless */
    return SL_RESULT_SUCCESS;
}

static SLresult Rate_GetRate(SLPlaybackRateItf self, SLpermille* rate) {
    if (!rate) return SL_RESULT_PARAMETER_INVALID;
    *rate = rate_of(self)->rate;
    return SL_RESULT_SUCCESS;
}

static SLresult Rate_SetPropertyConstraints(SLPlaybackRateItf self, SLuint32 constraints) {
    rate_of(self)->rate_properties = constraints;
    return SL_RESULT_SUCCESS;
}

static SLresult Rate_GetProperties(SLPlaybackRateItf self, SLuint32* properties) {
    if (!properties) return SL_RESULT_PARAMETER_INVALID;
    *properties = rate_of(self)->rate_properties;
    return SL_RESULT_SUCCESS;
}

static SLresult Rate_GetCapabilitiesOfRate(SLPlaybackRateItf self, SLpermille rate, SLuint32* capabilities) {
    (void)self;
    if (!capabilities) return SL_RESULT_PARAMETER_INVALID;
    *capabilities = (rate >= RATE_MIN && rate <= RATE_MAX) ? SL_RATEPROP_NOPITCHCORAUDIO : 0;
    return SL_RESULT_SUCCESS;
}

static SLresult Rate_GetRateRange(SLPlaybackRateItf self, SLuint8 index, SLpermille* min, SLpermille* max,
                                  SLpermille* step, SLuint32* capabilities) {
    struct player* p = rate_of(self);
    if (index != 0) return SL_RESULT_PARAMETER_INVALID;
    if (min) *min = p->is_fd ? 1000 : RATE_MIN;
    if (max) *max = p->is_fd ? 1000 : RATE_MAX;
    if (step) *step = 0;
    if (capabilities) *capabilities = SL_RATEPROP_NOPITCHCORAUDIO;
    return SL_RESULT_SUCCESS;
}

static const struct SLPlaybackRateItf_ rate_vtbl = {
    Rate_SetRate, Rate_GetRate, Rate_SetPropertyConstraints, Rate_GetProperties,
    Rate_GetCapabilitiesOfRate, Rate_GetRateRange,
};

/* ---- Seek (FD players) ---------------------------------------------------------------- */

static struct player* seek_of(SLSeekItf self) { return container_of(self, struct player, seek_itf); }

static SLresult Seek_SetPosition(SLSeekItf self, SLmillisecond pos, SLuint32 mode) {
    struct player* p = seek_of(self);
    (void)mode;
    JNIEnv* env = jni_env();
    if (env && p->media) {
        (*env)->CallVoidMethod(env, p->media, mp.seek, (jint)pos);
        jni_failed(env, "MediaPlayer.seekTo");
        p->ended = 0;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult Seek_SetLoop(SLSeekItf self, SLboolean enable, SLmillisecond start, SLmillisecond end) {
    struct player* p = seek_of(self);
    (void)start; (void)end;
    p->looping = enable ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE;
    JNIEnv* env = jni_env();
    if (env && p->media) {
        (*env)->CallVoidMethod(env, p->media, mp.set_looping, (jboolean)p->looping);
        jni_failed(env, "MediaPlayer.setLooping");
    }
    return SL_RESULT_SUCCESS;
}

static SLresult Seek_GetLoop(SLSeekItf self, SLboolean* enabled, SLmillisecond* start, SLmillisecond* end) {
    struct player* p = seek_of(self);
    if (enabled) *enabled = p->looping;
    if (start) *start = 0;
    if (end) *end = SL_TIME_UNKNOWN;
    return SL_RESULT_SUCCESS;
}

static const struct SLSeekItf_ seek_vtbl = {Seek_SetPosition, Seek_SetLoop, Seek_GetLoop};

/* ---- player lifetime ------------------------------------------------------------------ */

static void player_destroy(struct player* p) {
    pthread_mutex_lock(&g_lock);
    p->dead = 1;
    for (struct player** link = &g_players; *link; link = &(*link)->next) {
        if (*link == p) {
            *link = p->next;
            break;
        }
    }
    /* A callback may still be running on the mixer thread. If we are that thread (a game
     * releasing a sound from its own callback), the pass finishes before memory is reused
     * only if we leak; that is rare and bounded, so leak rather than deadlock. */
    int self = g_mixer_started && pthread_equal(pthread_self(), g_mixer);
    while (p->busy && !self) pthread_cond_wait(&g_idle, &g_lock);
    int leak = p->busy != 0;
    pthread_mutex_unlock(&g_lock);
    if (p->media) {
        JNIEnv* env = jni_env();
        if (env) {
            (*env)->CallVoidMethod(env, p->media, mp.release);
            jni_failed(env, "MediaPlayer.release");
            (*env)->DeleteGlobalRef(env, p->media);
        }
        p->media = NULL;
    }
    if (!leak) free(p);
}

/* ---- Engine --------------------------------------------------------------------------- */

static SLresult unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }

static SLresult Engine_CreateLEDDevice(SLEngineItf self, SLObjectItf* obj, SLuint32 id, SLuint32 n,
                                       const SLInterfaceID* ids, const SLboolean* req) {
    (void)self; (void)obj; (void)id; (void)n; (void)ids; (void)req;
    return unsupported();
}

static SLresult Engine_CreateVibraDevice(SLEngineItf self, SLObjectItf* obj, SLuint32 id, SLuint32 n,
                                         const SLInterfaceID* ids, const SLboolean* req) {
    (void)self; (void)obj; (void)id; (void)n; (void)ids; (void)req;
    return unsupported();
}

static int player_supports(int is_fd, SLInterfaceID iid) {
    if (iid_eq(iid, SL_IID_PLAY) || iid_eq(iid, SL_IID_VOLUME) || iid_eq(iid, SL_IID_PLAYBACKRATE) ||
        iid_eq(iid, SL_IID_OBJECT))
        return 1;
    if (!is_fd) return iid_eq(iid, SL_IID_ANDROIDSIMPLEBUFFERQUEUE) || iid_eq(iid, SL_IID_BUFFERQUEUE);
    return iid_eq(iid, SL_IID_SEEK);
}

static SLresult Engine_CreateAudioPlayer(SLEngineItf self, SLObjectItf* out, SLDataSource* src, SLDataSink* sink,
                                         SLuint32 n, const SLInterfaceID* ids, const SLboolean* req) {
    (void)self; (void)sink;
    if (!out || !src || !src->pLocator) return SL_RESULT_PARAMETER_INVALID;
    *out = NULL;
    SLuint32 locator = *(const SLuint32*)src->pLocator;
    int is_fd;
    if (locator == SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE || locator == SL_DATALOCATOR_BUFFERQUEUE) {
        is_fd = 0;
    } else if (locator == SL_DATALOCATOR_ANDROIDFD) {
        is_fd = 1;
    } else {
        LOGW("CreateAudioPlayer: unsupported locator 0x%x", (unsigned)locator);
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }
    for (SLuint32 i = 0; i < n; i++) {
        if (req && req[i] && !player_supports(is_fd, ids[i])) {
            LOGW("CreateAudioPlayer: required interface %u unsupported", (unsigned)i);
            return SL_RESULT_FEATURE_UNSUPPORTED;
        }
    }

    struct player* p = calloc(1, sizeof(*p));
    if (!p) return SL_RESULT_MEMORY_FAILURE;
    object_init(&p->obj, KIND_PLAYER);
    p->play_itf = &play_vtbl;
    p->bq_itf = &bq_vtbl;
    p->volume_itf = &volume_vtbl;
    p->rate_itf = &rate_vtbl;
    p->seek_itf = &seek_vtbl;
    p->play_state = SL_PLAYSTATE_STOPPED;
    p->rate = 1000;
    p->rate_properties = SL_RATEPROP_NOPITCHCORAUDIO;
    p->is_fd = is_fd;

    if (!is_fd) {
        const SLDataLocator_AndroidSimpleBufferQueue* loc = src->pLocator;
        const SLDataFormat_PCM* fmt = src->pFormat;
        if (!fmt || fmt->formatType != SL_DATAFORMAT_PCM || (fmt->numChannels != 1 && fmt->numChannels != 2) ||
            (fmt->bitsPerSample != 16 && fmt->bitsPerSample != 8) || fmt->samplesPerSec == 0) {
            free(p);
            return SL_RESULT_CONTENT_UNSUPPORTED;
        }
        p->channels = fmt->numChannels;
        p->bits = fmt->bitsPerSample;
        p->sample_rate = fmt->samplesPerSec / 1000;  /* milliHertz */
        p->queue_capacity = loc->numBuffers < 1 ? 1 : loc->numBuffers > MAX_QUEUE ? MAX_QUEUE : loc->numBuffers;
    } else {
        const SLDataLocator_AndroidFD* loc = src->pLocator;
        SLresult r = media_create(p, loc->fd, loc->offset, loc->length);
        LOGI("media player for fd %d (offset %lld, length %lld): %s", (int)loc->fd, (long long)loc->offset,
             (long long)loc->length, r == SL_RESULT_SUCCESS ? "ready" : "failed");
        if (r != SL_RESULT_SUCCESS) {
            free(p);
            return r;
        }
    }

    pthread_mutex_lock(&g_lock);
    p->next = g_players;
    g_players = p;
    mixer_start_locked();
    pthread_mutex_unlock(&g_lock);
    *out = &p->obj.itf;
    return SL_RESULT_SUCCESS;
}

static SLresult Engine_CreateAudioRecorder(SLEngineItf self, SLObjectItf* obj, SLDataSource* src, SLDataSink* sink,
                                           SLuint32 n, const SLInterfaceID* ids, const SLboolean* req) {
    (void)self; (void)obj; (void)src; (void)sink; (void)n; (void)ids; (void)req;
    return unsupported();
}

static SLresult Engine_CreateMidiPlayer(SLEngineItf self, SLObjectItf* obj, SLDataSource* a, SLDataSource* b,
                                        SLDataSink* c, SLDataSink* d, SLDataSink* e, SLuint32 n,
                                        const SLInterfaceID* ids, const SLboolean* req) {
    (void)self; (void)obj; (void)a; (void)b; (void)c; (void)d; (void)e; (void)n; (void)ids; (void)req;
    return unsupported();
}

static SLresult Engine_CreateListener(SLEngineItf self, SLObjectItf* obj, SLuint32 n, const SLInterfaceID* ids,
                                      const SLboolean* req) {
    (void)self; (void)obj; (void)n; (void)ids; (void)req;
    return unsupported();
}

static SLresult Engine_Create3DGroup(SLEngineItf self, SLObjectItf* obj, SLuint32 n, const SLInterfaceID* ids,
                                     const SLboolean* req) {
    (void)self; (void)obj; (void)n; (void)ids; (void)req;
    return unsupported();
}

static SLresult Engine_CreateOutputMix(SLEngineItf self, SLObjectItf* out, SLuint32 n, const SLInterfaceID* ids,
                                       const SLboolean* req) {
    (void)self;
    if (!out) return SL_RESULT_PARAMETER_INVALID;
    *out = NULL;
    for (SLuint32 i = 0; i < n; i++)
        if (req && req[i] && !iid_eq(ids[i], SL_IID_OBJECT)) return SL_RESULT_FEATURE_UNSUPPORTED;
    struct object* o = calloc(1, sizeof(*o));
    if (!o) return SL_RESULT_MEMORY_FAILURE;
    object_init(o, KIND_OUTPUTMIX);
    *out = &o->itf;
    return SL_RESULT_SUCCESS;
}

static SLresult Engine_CreateMetadataExtractor(SLEngineItf self, SLObjectItf* obj, SLDataSource* src,
                                               SLuint32 n, const SLInterfaceID* ids, const SLboolean* req) {
    (void)self; (void)obj; (void)src; (void)n; (void)ids; (void)req;
    return unsupported();
}

static SLresult Engine_CreateExtensionObject(SLEngineItf self, SLObjectItf* obj, void* params, SLuint32 id,
                                             SLuint32 n, const SLInterfaceID* ids, const SLboolean* req) {
    (void)self; (void)obj; (void)params; (void)id; (void)n; (void)ids; (void)req;
    return unsupported();
}

static SLresult Engine_QueryNumSupportedInterfaces(SLEngineItf self, SLuint32 id, SLuint32* n) {
    (void)self;
    if (!n) return SL_RESULT_PARAMETER_INVALID;
    switch (id) {
        case SL_OBJECTID_ENGINE: *n = 2; return SL_RESULT_SUCCESS;
        case SL_OBJECTID_OUTPUTMIX: *n = 1; return SL_RESULT_SUCCESS;
        case SL_OBJECTID_AUDIOPLAYER: *n = 7; return SL_RESULT_SUCCESS;
        default: *n = 0; return SL_RESULT_FEATURE_UNSUPPORTED;
    }
}

static SLresult Engine_QuerySupportedInterfaces(SLEngineItf self, SLuint32 id, SLuint32 index, SLInterfaceID* iid) {
    (void)self;
    if (!iid) return SL_RESULT_PARAMETER_INVALID;
    const SLInterfaceID* list;
    SLuint32 count;
    static SLInterfaceID engine[2], mix[1], player[7];
    engine[0] = SL_IID_OBJECT; engine[1] = SL_IID_ENGINE;
    mix[0] = SL_IID_OBJECT;
    player[0] = SL_IID_OBJECT; player[1] = SL_IID_PLAY; player[2] = SL_IID_VOLUME;
    player[3] = SL_IID_PLAYBACKRATE; player[4] = SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
    player[5] = SL_IID_BUFFERQUEUE; player[6] = SL_IID_SEEK;
    switch (id) {
        case SL_OBJECTID_ENGINE: list = engine; count = 2; break;
        case SL_OBJECTID_OUTPUTMIX: list = mix; count = 1; break;
        case SL_OBJECTID_AUDIOPLAYER: list = player; count = 7; break;
        default: return SL_RESULT_FEATURE_UNSUPPORTED;
    }
    if (index >= count) return SL_RESULT_PARAMETER_INVALID;
    *iid = list[index];
    return SL_RESULT_SUCCESS;
}

static SLresult Engine_QueryNumSupportedExtensions(SLEngineItf self, SLuint32* n) {
    (void)self;
    if (!n) return SL_RESULT_PARAMETER_INVALID;
    *n = 0;
    return SL_RESULT_SUCCESS;
}

static SLresult Engine_QuerySupportedExtension(SLEngineItf self, SLuint32 index, SLchar* name, SLint16* len) {
    (void)self; (void)index; (void)name; (void)len;
    return SL_RESULT_PARAMETER_INVALID;
}

static SLresult Engine_IsExtensionSupported(SLEngineItf self, const SLchar* name, SLboolean* supported) {
    (void)self; (void)name;
    if (!supported) return SL_RESULT_PARAMETER_INVALID;
    *supported = SL_BOOLEAN_FALSE;
    return SL_RESULT_SUCCESS;
}

static const struct SLEngineItf_ engine_vtbl = {
    Engine_CreateLEDDevice, Engine_CreateVibraDevice, Engine_CreateAudioPlayer, Engine_CreateAudioRecorder,
    Engine_CreateMidiPlayer, Engine_CreateListener, Engine_Create3DGroup, Engine_CreateOutputMix,
    Engine_CreateMetadataExtractor, Engine_CreateExtensionObject, Engine_QueryNumSupportedInterfaces,
    Engine_QuerySupportedInterfaces, Engine_QueryNumSupportedExtensions, Engine_QuerySupportedExtension,
    Engine_IsExtensionSupported,
};

/* ---- entry points --------------------------------------------------------------------- */

EXPORT SLresult slCreateEngine(SLObjectItf* out, SLuint32 num_options, const SLEngineOption* options,
                               SLuint32 n, const SLInterfaceID* ids, const SLboolean* req) {
    (void)num_options; (void)options;
    if (!out) return SL_RESULT_PARAMETER_INVALID;
    *out = NULL;
    for (SLuint32 i = 0; i < n; i++)
        if (req && req[i] && !iid_eq(ids[i], SL_IID_ENGINE) && !iid_eq(ids[i], SL_IID_OBJECT))
            return SL_RESULT_FEATURE_UNSUPPORTED;
    struct engine* e = calloc(1, sizeof(*e));
    if (!e) return SL_RESULT_MEMORY_FAILURE;
    object_init(&e->obj, KIND_ENGINE);
    e->engine_itf = &engine_vtbl;
    g_engine_itf = (SLEngineItf)&e->engine_itf;
    *out = &e->obj.itf;
    LOGI("slCreateEngine");
    return SL_RESULT_SUCCESS;
}

EXPORT SLresult slQueryNumSupportedEngineInterfaces(SLuint32* n) {
    if (!n) return SL_RESULT_PARAMETER_INVALID;
    *n = 2;
    return SL_RESULT_SUCCESS;
}

EXPORT SLresult slQuerySupportedEngineInterfaces(SLuint32 index, SLInterfaceID* iid) {
    if (!iid || index > 1) return SL_RESULT_PARAMETER_INVALID;
    *iid = index == 0 ? SL_IID_OBJECT : SL_IID_ENGINE;
    return SL_RESULT_SUCCESS;
}
