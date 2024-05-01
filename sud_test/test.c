#include <sys/prctl.h>
#include <sys/mman.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h> 
#include <stdlib.h>
#include <string.h>
#define _GNU_SOURCE 
#include <unistd.h>
#include <pthread.h>

__attribute__((noinline, section("syscall_section"))) void remote_syscall(void *this_page)
{
    // depricated: mprotect(this_page, 4096, PROT_READ | PROT_EXEC);
    // https://stackoverflow.com/questions/20326025/linux-assembly-how-to-call-syscall
    // https://blog.rchapman.org/posts/Linux_System_Call_Table_for_x86_64/
    __asm__ (
        "mov $60,%rax\n\t" // on x86-64, exit() is syscall $60
        "mov $99,%rdi\n\t" // exit code 99 goes in first argument (rdi)
        "syscall"
    );
}

void *remote_map(unsigned long sud_start)
{
    void *map;
    void *start = (void*)(sud_start - 4096);

    map = mmap(start, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
    if (map == MAP_FAILED)
        return NULL;

    // https://stackoverflow.com/a/69290721
    extern char __start_syscall_section[];
    extern char __stop_syscall_section[];
    memcpy(map, remote_syscall, __stop_syscall_section - __start_syscall_section);

    return map;
}

void map_and_wait(unsigned long start_addr, bool sud_is_enabled)
{
    void *remote_mapping;
    void (*remote_call)(void *);

    remote_mapping = remote_map(start_addr);

    if (!remote_mapping) {
        printf("failed to mmap a page specifically for remote syscall\n");
        exit(1);
    }

    remote_call = remote_mapping;

    printf("---\nmapped function containing a syscall into %p\n---\n", remote_mapping);

    // 4. Wait for checkpoint //

    printf(
        "PID is %d.\n"
        "Press any key to trigger nonlocal syscall.\n"
        "%s",
        getpid(),
        sud_is_enabled ? "Expect SIGSYS.\n" : "Expect exit with status 99.\n"
    );

    getchar();

    remote_call(remote_mapping);
}

int main(int argc, char **argv)
{
    // 1. Parse mappings to find libc with r-xp //

    // Per-line variables
    unsigned long start, end;
    char permissions[8];
    char *libc;
    // Buffer vars
    char *line;
    size_t size = 256;
    int rc = 1;

    FILE *fp;

    if (argc > 1) {
        printf("---\ndetected args. will run remote syscall without SUD.\n---\n");
        map_and_wait(0x7000000000ull, false);
        return 0;
    }
    
    line = malloc(size);
    if (!line) {
        printf("failed to malloc %lu bytes for reading /proc/maps\n", size);
        exit(1);
    }

    fp = fopen("/proc/self/maps", "r");

    while (rc) {
        rc = (int)getline(&line, &size, fp);

        if (!rc) // No characters read, reached EOF
            break;

        if (rc == -1) {
            printf("bad read of /proc/self/maps\n");
            exit(1);
        }

        rc = sscanf(line, "%lx-%lx %s ", &start, &end, permissions);
        if (rc != 3) {
            printf("in line...\n%s... expected sscanf rc 3 but got rc %d\n", line, rc);
            exit(1);
        }

        if (permissions[2] != 'x')
            continue;
        
        printf("---\nfound executable mapping:\n%s---\n", line);

        libc = strstr(line, "libc");

        if (!libc) {
            printf("---\nmapping...\n%s... is executable, but not libc\n---\n", line);
            continue;
        }

        printf("---\nfound libc executable region!\n%s---\n", line);
        break;
    }

    // 2. mark libc region as kosher, all other syscalls will be SUD'ed //

    printf("tagging syscalls in %lx-%lx as passthrough; other ones will be SUD-ed\n---\n", start, end);

    rc = prctl(PR_SET_SYSCALL_USER_DISPATCH, PR_SYS_DISPATCH_ON, start, end-start);
    if (rc) {
        printf("prctl SUD rc %d errno %d", rc, errno);
        exit(1);
    }
    
    map_and_wait(start, true);

    return 0; 
}