#include <stdio.h>
#include <unistd.h>

int main() {
    pid_t pid = fork();

    if (pid > 0) {
        printf("I am the parent, PID: %d\n", getpid());
    } else if (pid == 0) {
        printf("I am the child, PID: %d\n", getpid());
    } else {
        printf("Fork failed!\n");
    }
    return 0;
}
