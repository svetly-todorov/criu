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
// Thread local selector variable allows for SUD toggle
__thread char selector;

int gettid() {
    return syscall(SYS_gettid);
}

void sigsys_handler(int signo, siginfo_t *si, void *ucontext)
{
    struct ucontext *ctxt = (struct ucontext *)ucontext;

    selector = SYSCALL_DISPATCH_FILTER_ALLOW;

    // set the return code to expected value of zero
    ctxt->uc_mcontext.rax = 0;

    return;
}

void *run_test(void *thread_data)
{
    uint64_t start, end, ttl;
    int i, id;
    struct timespec x;

    struct sigaction sa;
    sa.sa_sigaction = sigsys_handler;
    sa.sa_flags = SA_SIGINFO;
    
    selector = SYSCALL_DISPATCH_FILTER_BLOCK;
    id = gettid() - getpid();

    // register sigsys handler

    if (sigaction(SIGSYS, &sa, NULL)) {
        printf("%d: sud sigaction errno %d\n", id, errno);
        return NULL;
    }

    // initialize syscall dispatch

    if (prctl(PR_SET_SYSCALL_USER_DISPATCH, PR_SYS_DISPATCH_ON, 4096ull * 4096, 4096, &selector)) {
        printf("%d: sud prctl errno %d\n", id, errno);
        return NULL;
    }

    i = iterations;
    ttl = 0;
    while (i--)
    {
        selector = SYSCALL_DISPATCH_FILTER_BLOCK;
        start = __rdtsc();
        if (syscall(SYS_clock_gettime, CLOCK_REALTIME, &x, NULL) != 0)
        {
            printf("clock_gettime failed\n");
            exit(EXIT_FAILURE);
        }
        // side effect of syscall catcher should be that selector -> SYSCALL_DISPATCH_FILTER_ALLOW
        end = __rdtsc();
        ttl += end-start;
    }
    printf("%d: AVG: %ld cycles\n", id, ttl/1000000);
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

    num_threads = atoi(argv[1]);
    iterations = atoi(argv[2]);

    printf("./criu/criu dump -vvvv -j -t %d\n", getpid());

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
    }

    return 0;
}