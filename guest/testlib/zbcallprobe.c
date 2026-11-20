/* Guest probe library for library_runtime_test: return types, softfp arguments, thread
 * identity, bionic contention, signals and runtime host calls. Every export uses base AAPCS. */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define API __attribute__((visibility("default"), pcs("aapcs")))
#define DATA __attribute__((visibility("default")))

API int32_t zb_probe_mix(uint32_t env, uint32_t self, uint8_t z, int8_t b, uint16_t c, int16_t s, int32_t i,
                         int64_t j, float f, double d, uint32_t ref) {
    if (env != 0xe000 || self != 0x1077 || z != 1 || b != -2 || c != 0x1234 || s != -3 || i != 4 ||
        j != INT64_C(0x1122334455667788) || f != 1.5f || d != -2.25 || ref != 0x1099) {
        return -1;
    }
    return 42;
}

API void zb_return_v(void) {}
API uint8_t zb_return_z(void) { return 1; }
API int8_t zb_return_b(void) { return -2; }
API uint16_t zb_return_c(void) { return 0x1234; }
API int16_t zb_return_s(void) { return -3; }
API int32_t zb_return_i(void) { return 42; }
API int64_t zb_return_j(void) { return INT64_C(0x1122334455667788); }
API float zb_return_f(void) { return 3.5f; }
API double zb_return_d(void) { return -1.25; }
API uint32_t zb_return_l(void) { return 0x12345; }

API uint32_t zb_probe_tls(void) {
    return (uint32_t)(uintptr_t)__builtin_thread_pointer();
}

API int32_t zb_probe_tid(void) {
    return (int32_t)syscall(__NR_gettid);
}

API int32_t zb_probe_cached_tid(void) {
    return (int32_t)gettid();
}

/* Runtime host call outside the generated stub range, answered by a chained handler. */
API uint32_t zb_probe_host_call(uint32_t value) {
    register uint32_t r0 __asm__("r0") = value;
    __asm__ volatile("svc #0x5afd00" : "+r"(r0) : : "memory");
    return r0;
}

static uint32_t overlap_count;

/* Returns only after two callers are inside at the same time (or -1 after 2 s). */
API int32_t zb_probe_overlap(void) {
    const uint32_t ticket = __atomic_add_fetch(&overlap_count, 1, __ATOMIC_SEQ_CST);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (__atomic_load_n(&overlap_count, __ATOMIC_SEQ_CST) < 2) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const int64_t elapsed = (int64_t)(now.tv_sec - start.tv_sec) * 1000000000LL + now.tv_nsec - start.tv_nsec;
        if (elapsed > 2000000000LL) return -1;
        sched_yield();
    }
    return (int32_t)ticket;
}

static pthread_mutex_t contention_lock = PTHREAD_MUTEX_INITIALIZER;
DATA uint32_t zb_contention_total;

/* Mutex, malloc/free, errno and thread identity under concurrency. 0 on success. */
API int32_t zb_probe_contention(uint32_t iterations) {
    const pthread_t self = pthread_self();
    const pid_t tid = gettid();
    for (uint32_t i = 0; i < iterations; ++i) {
        if (pthread_mutex_lock(&contention_lock) != 0) return -1;
        ++zb_contention_total;
        if (pthread_mutex_unlock(&contention_lock) != 0) return -2;
        char* block = malloc(16 + (i % 512));
        if (block == NULL) return -3;
        memset(block, (int)(i & 0xff), 16);
        free(block);
        const int expected = (int)((tid ^ i) & 0x7fff) + 1;
        errno = expected;
        sched_yield();
        if (errno != expected) return -4;
        if (!pthread_equal(pthread_self(), self) || gettid() != tid || syscall(__NR_gettid) != tid) return -5;
    }
    return 0;
}

DATA volatile int32_t zb_usr1_count;
DATA volatile int32_t zb_usr1_tid;
DATA volatile uint32_t zb_usr1_tls;

static void on_usr1(int sig) {
    (void)sig;
    zb_usr1_tid = (int32_t)syscall(__NR_gettid);
    zb_usr1_tls = (uint32_t)(uintptr_t)__builtin_thread_pointer();
    ++zb_usr1_count;
}

API int32_t zb_probe_install_usr1(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    return sigaction(SIGUSR1, &sa, NULL);
}

/* A zero target selects this thread's own (cached) tid. */
API int32_t zb_probe_tgkill(int32_t sig, int32_t tid) {
    return (int32_t)syscall(__NR_tgkill, getpid(), tid != 0 ? tid : gettid(), sig);
}

DATA volatile int32_t zb_alarm_count;
DATA volatile int32_t zb_alarm_tid;

static void on_alarm(int sig) {
    (void)sig;
    zb_alarm_tid = (int32_t)syscall(__NR_gettid);
    ++zb_alarm_count;
}

/* One-shot ITIMER_REAL; the handler records the tid it ran on. */
API int32_t zb_probe_arm_alarm(int32_t usec) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    if (sigaction(SIGALRM, &sa, NULL) != 0) return -1;
    zb_alarm_count = 0;
    zb_alarm_tid = 0;
    struct itimerval timer;
    memset(&timer, 0, sizeof timer);
    timer.it_value.tv_usec = usec;
    return setitimer(ITIMER_REAL, &timer, NULL);
}

API int32_t zb_probe_block_signal(int32_t sig, int32_t block) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig);
    return sigprocmask(block ? SIG_BLOCK : SIG_UNBLOCK, &set, NULL);
}

API int32_t zb_probe_signal_blocked(int32_t sig) {
    sigset_t current;
    if (sigprocmask(SIG_BLOCK, NULL, &current) != 0) return -1;
    return sigismember(&current, sig);
}

/* ss_sp of the enabled alternate signal stack, or 0. */
API uint32_t zb_probe_altstack_sp(void) {
    stack_t ss;
    if (sigaltstack(NULL, &ss) != 0 || (ss.ss_flags & SS_DISABLE) != 0) return 0;
    return (uint32_t)(uintptr_t)ss.ss_sp;
}
