#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>

#define PORT 8080
#define BUFFER_SIZE 1024

int main() {
    int sock;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];
    fd_set read_fds;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connect failed");
        return 1;
    }

    printf("Connected to broadcast server. Type messages:\n");

    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        FD_SET(sock, &read_fds);

        select(sock + 1, &read_fds, NULL, NULL, NULL);

        if (FD_ISSET(sock, &read_fds)) {
            ssize_t bytes = read(sock, buffer, sizeof(buffer) - 1);
            if (bytes <= 0) break;
            buffer[bytes] = '\0';
            printf("\n[Broadcast]: %s\n> ", buffer);
            fflush(stdout);
        }

        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            if (!fgets(buffer, sizeof(buffer), stdin)) break;
            buffer[strcspn(buffer, "\r\n")] = '\0';
            send(sock, buffer, strlen(buffer), 0);
            printf("> ");
            fflush(stdout);
        }
    }

    close(sock);
    return 0;
}
