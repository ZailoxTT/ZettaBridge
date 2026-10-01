/* Stress the boundary between guest signal delivery and interrupted host syscalls. Each worker
 * verifies file bytes, then blocks in both bionic sem_wait and a raw futex while another thread
 * continuously sends SA_RESTART signals. No wait may leak EINTR and every worker must progress. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

enum {
    kWorkers = 3,
    kIterations = 1000,
    kFileSize = 256 * 1024,
    kReadSize = 1024,
};

struct worker {
    int index;
    atomic_int tid;
    atomic_int stage;
    atomic_int progress;
    atomic_int futex_word;
    sem_t semaphore;
};

static struct worker workers[kWorkers];
static atomic_int failures;
static atomic_int handled;
static atomic_int sem_signals;
static atomic_int futex_signals;
static atomic_int signaler_done;
static int input_fd;
static _Thread_local struct worker* current_worker;

static unsigned char byte_at(long offset) {
    return (unsigned char)((offset * 131 + (offset >> 7) * 17 + (offset % 251)) & 0xff);
}

static void on_signal(int sig) {
    (void)sig;
    atomic_fetch_add(&handled, 1);
    if (current_worker != NULL) {
        const int stage = atomic_load(&current_worker->stage);
        if (stage > 0) atomic_fetch_add((stage & 1) != 0 ? &sem_signals : &futex_signals, 1);
    }
}

static void on_alarm(int sig) {
    (void)sig;
    _exit(124);
}

static void* signaler(void* arg) {
    (void)arg;
    while (!atomic_load(&signaler_done)) {
        for (int i = 0; i < kWorkers; ++i) {
            const int tid = atomic_load(&workers[i].tid);
            if (tid != 0) syscall(SYS_tgkill, getpid(), tid, SIGUSR1);
        }
        sched_yield();
    }
    return NULL;
}

static void* run_worker(void* arg) {
    struct worker* worker = arg;
    unsigned char buffer[kReadSize];
    current_worker = worker;
    atomic_store(&worker->tid, (int)syscall(SYS_gettid));

    for (int iteration = 0; iteration < kIterations; ++iteration) {
        const long offset =
            ((long)iteration * 4093 + (long)worker->index * 65537) % (kFileSize - kReadSize);
        const ssize_t count = pread(input_fd, buffer, sizeof buffer, offset);
        if (count != (ssize_t)sizeof buffer) {
            atomic_fetch_add(&failures, 1);
        } else {
            for (long i = 0; i < (long)sizeof buffer; ++i) {
                if (buffer[i] != byte_at(offset + i)) {
                    atomic_fetch_add(&failures, 1);
                    break;
                }
            }
        }

        atomic_store(&worker->stage, iteration * 2 + 1);
        if (sem_wait(&worker->semaphore) != 0) atomic_fetch_add(&failures, 1);

        const int token = atomic_load(&worker->futex_word);
        atomic_store(&worker->stage, iteration * 2 + 2);
        const long result = syscall(SYS_futex, &worker->futex_word, FUTEX_WAIT_PRIVATE, token,
                                    NULL, NULL, 0);
        if (result != 0 && errno != EAGAIN) atomic_fetch_add(&failures, 1);
        atomic_store(&worker->stage, 0);
        atomic_store(&worker->progress, iteration + 1);
    }
    atomic_store(&worker->stage, -1);
    return NULL;
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "/data/local/tmp/zb_sigio_race.tmp";
    input_fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (input_fd < 0) return 2;
    unsigned char* source = malloc(kFileSize);
    if (source == NULL) return 2;
    for (long i = 0; i < kFileSize; ++i) source[i] = byte_at(i);
    if (write(input_fd, source, kFileSize) != kFileSize) return 2;
    free(source);

    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_signal;
    action.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &action, NULL);
    action.sa_handler = on_alarm;
    action.sa_flags = 0;
    sigaction(SIGALRM, &action, NULL);
    alarm(30);

    pthread_t threads[kWorkers];
    for (int i = 0; i < kWorkers; ++i) {
        workers[i].index = i;
        sem_init(&workers[i].semaphore, 0, 0);
        pthread_create(&threads[i], NULL, run_worker, &workers[i]);
    }
    pthread_t signal_thread;
    pthread_create(&signal_thread, NULL, signaler, NULL);

    int seen[kWorkers] = {};
    int finished = 0;
    while (finished != kWorkers) {
        finished = 0;
        for (int i = 0; i < kWorkers; ++i) {
            const int stage = atomic_load(&workers[i].stage);
            if (stage == -1) {
                ++finished;
                continue;
            }
            if (stage == 0 || stage == seen[i]) continue;
            seen[i] = stage;
            /* Leave a short scheduling window in which the signaler can interrupt the wait. */
            for (int spin = 0; spin < 8; ++spin) sched_yield();
            if ((stage & 1) != 0) {
                sem_post(&workers[i].semaphore);
            } else {
                atomic_fetch_add(&workers[i].futex_word, 1);
                syscall(SYS_futex, &workers[i].futex_word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
            }
        }
        sched_yield();
    }

    atomic_store(&signaler_done, 1);
    pthread_join(signal_thread, NULL);
    int progress = 0;
    for (int i = 0; i < kWorkers; ++i) {
        pthread_join(threads[i], NULL);
        progress += atomic_load(&workers[i].progress);
        sem_destroy(&workers[i].semaphore);
    }
    alarm(0);
    close(input_fd);
    unlink(path);

    const int signals_ok = atomic_load(&handled) > 0;
    const int sem_signals_ok = atomic_load(&sem_signals) > 0;
    const int futex_signals_ok = atomic_load(&futex_signals) > 0;
    const int errors = atomic_load(&failures) + !signals_ok + !sem_signals_ok + !futex_signals_ok;
    printf("signal io race: workers %d progress %d signals %d phases %d%d errors %d\n", kWorkers,
           progress, signals_ok, sem_signals_ok, futex_signals_ok, errors);
    return errors == 0 && progress == kWorkers * kIterations ? 0 : 1;
}
