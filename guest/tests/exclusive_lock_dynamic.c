/* Match Mono's ARM test-and-set lock. Its release is a plain store after a
 * barrier, so an exclusive store must be atomic against ordinary writes. */
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

enum { kThreads = 6, kIterations = 30000 };
static uint32_t lock_word;
static unsigned completed;
static atomic_bool start;

__attribute__((target("arm"))) static void take_lock(void) {
    uint32_t previous;
    uint32_t status;
    const uint32_t held = 1;
    do {
        __asm__ volatile(
            "1: ldrex %[previous], [%[word]]\n"
            "strex %[status], %[held], [%[word]]\n"
            "cmp %[status], #0\n"
            "bne 1b"
            : [previous] "=&r"(previous), [status] "=&r"(status)
            : [word] "r"(&lock_word), [held] "r"(held)
            : "cc", "memory");
    } while (previous != 0);
    __asm__ volatile("dmb ish" ::: "memory");
}

__attribute__((target("arm"))) static void release_lock(void) {
    __asm__ volatile("dmb ish\n"
                     "str %[free], [%[word]]"
                     :: [free] "r"(0u), [word] "r"(&lock_word) : "memory");
}

__attribute__((target("arm"))) static void* worker(void* arg) {
    (void)arg;
    while (!atomic_load(&start)) sched_yield();
    for (unsigned i = 0; i < kIterations; ++i) {
        take_lock();
        ++completed;
        release_lock();
    }
    return NULL;
}

int main(void) {
    alarm(15);
    pthread_t threads[kThreads];
    for (unsigned i = 0; i < kThreads; ++i) {
        if (pthread_create(&threads[i], NULL, worker, NULL) != 0) return 1;
    }
    atomic_store(&start, 1);
    for (unsigned i = 0; i < kThreads; ++i) {
        if (pthread_join(threads[i], NULL) != 0) return 1;
    }
    alarm(0);
    printf("exclusive lock acquisitions: %u\n", completed);
    return completed != kThreads * kIterations;
}
