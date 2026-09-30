/* A signal handler installed with SA_RESTART must not make a blocking call fail: the kernel
 * restarts the call afterwards. Unity relies on this - it reports an interrupted semaphore wait
 * as a failure and gives up - and mono and FMOD signal often enough for it to matter. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static sem_t semaphore;
static atomic_int handled;
static pid_t waiter_tid;

static void on_signal(int sig) {
    (void)sig;
    atomic_fetch_add(&handled, 1);
}

static void* disturber(void* arg) {
    (void)arg;
    const struct timespec pause = {0, 50 * 1000 * 1000};
    while (waiter_tid == 0) nanosleep(&pause, NULL);
    nanosleep(&pause, NULL);
    syscall(SYS_tgkill, getpid(), waiter_tid, SIGUSR1);  /* interrupts the wait */
    nanosleep(&pause, NULL);
    sem_post(&semaphore);                                /* and only then releases it */
    return NULL;
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
    sem_init(&semaphore, 0, 0);

    pthread_t thread;
    pthread_create(&thread, NULL, disturber, NULL);
    waiter_tid = (pid_t)syscall(SYS_gettid);

    const int rc = sem_wait(&semaphore);
    printf("sem_wait returned %d errno %d after %d signals\n", rc, rc == 0 ? 0 : errno,
           atomic_load(&handled));
    pthread_join(thread, NULL);
    return 0;
}
