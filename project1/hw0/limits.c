#include <stdio.h>
#include <sys/time.h>
#include <sys/resource.h>

int main(int argc, char *argv[]) {
    struct rlimit lim;

    // Get and print stack size limit
    if (getrlimit(RLIMIT_STACK, &lim) == 0) {
        printf("stack size: %ld\n", (long)lim.rlim_cur);
    }

    // Get and print process limit
    if (getrlimit(RLIMIT_NPROC, &lim) == 0) {
        printf("process limit: %ld\n", (long)lim.rlim_cur);
    }

    // Get and print max file descriptors limit
    if (getrlimit(RLIMIT_NOFILE, &lim) == 0) {
        printf("max file descriptors: %ld\n", (long)lim.rlim_cur);
    }

    return 0;
}
