#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

int main() {
    pid_t pid = fork();

    if (pid == 0) {
        printf("Executing ls command...\n");
        execlp("ls", "ls", "-l", NULL);
        perror("execlp failed");
        return 1;
    } else if (pid > 0) {
        wait(NULL);
        printf("Parent process finished.\n");
    } else {
        perror("fork failed");
        return 1;
    }
    return 0;
}
