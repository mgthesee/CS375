#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

int main() {
    pid_t pid1 = fork();

    if (pid1 == 0) {
        printf("Child 1, PID: %d\n", getpid());
        return 0;
    } else if (pid1 > 0) {
        pid_t pid2 = fork();
        if (pid2 == 0) {
            printf("Child 2, PID: %d\n", getpid());
            return 0;
        } else if (pid2 > 0) {
            wait(NULL);
            wait(NULL);
            printf("I am the parent, PID: %d\n", getpid());
        } else {
            printf("Fork 2 failed!\n");
        }
    } else {
        printf("Fork 1 failed!\n");
    }
    return 0;
}
