#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>

int main() {
    char command[256];

    while (1) {
        printf("myshell> ");
        fflush(stdout);

        if (fgets(command, sizeof(command), stdin) == NULL) {
            break;
        }

        // Strip newline
        command[strcspn(command, "\n")] = 0;

        // Ignore empty enter presses
        if (strlen(command) == 0) {
            continue;
        }

        // Exit command
        if (strcmp(command, "exit") == 0) {
            break;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("Fork failed");
        } else if (pid == 0) {
            execlp(command, command, NULL);
            printf("Command execution failed!\n");
            exit(1);
        } else {
            wait(NULL);
        }
    }
    return 0;
}
