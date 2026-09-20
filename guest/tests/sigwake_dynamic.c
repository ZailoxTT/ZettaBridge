// A guest signal must reach a thread that is asleep in a blocking syscall, and sigsuspend must
// wait for one. IL2CPP's Boehm collector stops the world exactly this way: it signals every
// thread and waits for each to acknowledge, and a thread parked in a futex must answer.
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static atomic_int futex_word;      /* never woken: only a signal ends the wait */
static atomic_int sleeper_signals;
static atomic_int sleeper_ready;
static atomic_int suspend_signals;
static pid_t sleeper_tid;
static pid_t main_tid;

static void on_sigusr1(int sig) {
    (void)sig;
    atomic_fetch_add(&sleeper_signals, 1);
}

static void on_sigusr2(int sig) {
    (void)sig;
    atomic_fetch_add(&suspend_signals, 1);
}

/* Sends the signal the main thread waits for in sigsuspend. */
static void* waker_thread(void* arg) {
    (void)arg;
    const struct timespec wait = {0, 50 * 1000 * 1000};
    nanosleep(&wait, NULL);
    syscall(SYS_tgkill, getpid(), main_tid, SIGUSR2);
    return NULL;
}

static void* sleeper(void* arg) {
    (void)arg;
    sleeper_tid = (pid_t)syscall(SYS_gettid);
    atomic_store(&sleeper_ready, 1);
    /* Waits until the handler has run twice, the way a stop-the-world loop rechecks its flag. */
    while (atomic_load(&sleeper_signals) < 2) {
        syscall(SYS_futex, &futex_word, FUTEX_WAIT, 0, NULL, NULL, 0);
    }
    return NULL;
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigusr1;
    sigaction(SIGUSR1, &sa, NULL);
    sa.sa_handler = on_sigusr2;
    sigaction(SIGUSR2, &sa, NULL);

    pthread_t thread;
    pthread_create(&thread, NULL, sleeper, NULL);
    while (atomic_load(&sleeper_ready) == 0) usleep(1000);

    const struct timespec pause = {0, 20 * 1000 * 1000};
    for (int i = 0; i < 2; ++i) {
        nanosleep(&pause, NULL);  /* let the thread reach the futex */
        syscall(SYS_tgkill, getpid(), sleeper_tid, SIGUSR1);
    }
    pthread_join(thread, NULL);
    printf("sleeper woke after %d signals\n", atomic_load(&sleeper_signals));

    /* sigsuspend: block SIGUSR2, then wait for it with a mask that lets it through. */
    sigset_t blocked, empty, previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR2);
    sigemptyset(&empty);
    pthread_sigmask(SIG_BLOCK, &blocked, &previous);

    pthread_t waker;
    main_tid = (pid_t)syscall(SYS_gettid);
    pthread_create(&waker, NULL, waker_thread, NULL);

    const int rc = sigsuspend(&empty);
    printf("sigsuspend returned %d errno %s after %d signals\n", rc,
           errno == EINTR ? "EINTR" : strerror(errno), atomic_load(&suspend_signals));
    pthread_join(waker, NULL);

    sigset_t now;
    pthread_sigmask(SIG_SETMASK, NULL, &now);
    printf("SIGUSR2 blocked again: %d\n", sigismember(&now, SIGUSR2));
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    return 0;
}
