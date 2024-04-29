#include <sys/prctl.h>

#include <errno.h>
#include <stdio.h> 
#include <stdlib.h>
#include <string.h>
#define _GNU_SOURCE 
#include <unistd.h>
#include <pthread.h>

__attribute__((noinline, section("syscall_section"))) void syscall_remote (void)
{
    getpid();
}

int main()
{
    // 1. Parse mappings to find libc with r-xp //

    // Per-line variables
    unsigned long start, end;
    char permissions[8];
    char *libc;
    // Buffer vars
    char line[256], **linep = &line;
    size_t size = sizeof(line);
    int rc = 1;

    FILE *fp = fopen("/proc/self/maps", "r");
    
    while (rc) {
        rc = (int)getline(linep, &size, fp);

        if (!rc) // No characters read, reached EOF
            break;

        if (rc == -1) {
            printf("bad read of /proc/self/maps\n");
            exit(1);
        }

        rc = sscanf(line, "%lx-%lx %s ", &start, &end, permissions);
        if (rc != 3) {
            printf("in line [ %s ] expected sscanf rc 3 but got rc %d\n", line, rc);
            exit(1);
        }

        if (permissions[2] != 'x')
            continue;
        
        printf("found executable mapping [%s]\n", line);

        libc = strstr(line, "libc");

        if (!libc) {
            printf("mapping [ %s ] is executable, but not libc\n", line);
            continue;
        }

        printf("found libc executable region [%s]\n", line);
        break;
    }

    // 2. mark libc region as kosher, all other syscalls will be SUD'ed //

    printf("tagging syscalls in %lx-%lx as passthrough; other ones will be SUD-ed\n", start, end);

    rc = prctl(PR_SET_SYSCALL_USER_DISPATCH, PR_SYS_DISPATCH_ON, start, end-start);
    if (rc) {
        printf("prctl SUD rc %d errno %d", rc, errno);
        exit(1);
    }

    // 3. Loop and wait for checkpoint //

    printf("ready for checkpoint...\n");

    while (1) {
        sleep(1);
        sched_yield();
    }

    return 0; 
}