#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <signal.h>
#include <asm-generic/ucontext.h>

#include <x86intrin.h>

// Total clock_gettime invocations
int iterations;
// __thread local variables probably don't work with CRIU;
// try harebrained scheme instead
char *selectors;
int num_selectors;

pthread_key_t tid_key;

int gettid() {
    return syscall(SYS_gettid);
}

void sigsys_handler(int signo, siginfo_t *si, void *ucontext)
{
    struct ucontext *ctxt = (struct ucontext *)ucontext;
    int id;

    // set the return code to expected value of zero
    ctxt->uc_mcontext.rax = 0;

    id = *(int *)pthread_getspecific(tid_key);

    selectors[id] = SYSCALL_DISPATCH_FILTER_ALLOW;

    return;
}

void *run_test(void *thread_data)
{
    uint64_t start, end, ttl;
    int i, id, rc;
    struct timespec x;

    struct sigaction sa;
    sa.sa_sigaction = sigsys_handler;
    sa.sa_flags = SA_SIGINFO;
    
    id = gettid() - getpid();
    if (id >= num_selectors) {
        printf("%d: id is greater than # of selectors\n", id);
        return NULL;
    }

    pthread_setspecific(tid_key, (void *)&id);

    selectors[id] = SYSCALL_DISPATCH_FILTER_BLOCK;

    // register sigsys handler

    if (sigaction(SIGSYS, &sa, NULL)) {
        printf("%d: sud sigaction errno %d\n", id, errno);
        return NULL;
    }

    // initialize syscall dispatch

    if (prctl(PR_SET_SYSCALL_USER_DISPATCH, PR_SYS_DISPATCH_ON, 4096ull * 4096, 4096, &selectors[id])) {
        printf("%d: sud prctl errno %d\n", id, errno);
        return NULL;
    }

    i = iterations;
    ttl = 0;
    while (i--)
    {
        selectors[id] = SYSCALL_DISPATCH_FILTER_BLOCK;
        start = __rdtsc();
        rc = syscall(SYS_clock_gettime, CLOCK_REALTIME, &x, NULL);
        if (rc)
        {
            selectors[id] = SYSCALL_DISPATCH_FILTER_ALLOW;
            printf("%d: clock_gettime failed with rc %d\n", gettid(), rc);
            exit(EXIT_FAILURE);
        }
        // sleep(1);
        volatile uint64_t count = 0;
        while (count++ < 1000) ;
        // side effect of syscall catcher should be that selectors[id] -> SYSCALL_DISPATCH_FILTER_ALLOW
        end = __rdtsc();
        ttl += end-start;
    }
    printf("%d: AVG: %ld cycles\n", id, ttl/iterations);
    return NULL;
}

int main(int argc, char **argv)
{
    int num_threads, t;
    pthread_t *threads;

    if (argc < 3) {
        printf("./clock num_threads iterations\n");
        exit(1);
    }

    // global variables
    num_threads = atoi(argv[1]);
    iterations = atoi(argv[2]);
    num_selectors = num_threads * 2;
    selectors = (char *)malloc(num_selectors * sizeof(char));

    pthread_key_create(&tid_key, NULL);

    printf("sudo ./criu/criu dump -vvvv -j -t %d\n", getpid());

    threads = (pthread_t *)malloc(num_threads * sizeof(pthread_t));
    if (!threads) {
        printf("failed to malloc space for %d pthreads\n", t);
        exit(2);
    }

    for (t = 0; t < num_threads; ++t) {
        if (pthread_create(&threads[t], NULL, run_test, NULL)) {
            printf("failed to create pthread %d errno %d\n", t, errno);
            exit(2);
        }
    }

    for (t = 0; t < num_threads; ++t) {
        if (pthread_join(threads[t], NULL)) {
            printf("failed to join pthread %d errno %d\n", t, errno);
            exit(2);
        }
        printf("main thread joined %d\n", t);
    }

    printf("main thread exiting\n");

    return 0;
}