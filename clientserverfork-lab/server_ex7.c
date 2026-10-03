#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/select.h>

#define PORT 8080
#define MAX_CLIENTS 10
#define BUFFER_SIZE 1024

typedef struct {
    int active;
    int child_pid;
    int to_child_fd;    // Parent writes here to send to child
    int from_child_fd;  // Parent reads here from child
} ClientSession;

void run_child_worker(int client_sock, int rx_from_parent, int tx_to_parent) {
    char buffer[BUFFER_SIZE];
    fd_set fds;

    while (1) {
        FD_ZERO(&fds);
        FD_SET(client_sock, &fds);
        FD_SET(rx_from_parent, &fds);

        int max_fd = (client_sock > rx_from_parent) ? client_sock : rx_from_parent;
        if (select(max_fd + 1, &fds, NULL, NULL, NULL) < 0) break;

        // 1. Data received from network client -> forward to parent dispatcher
        if (FD_ISSET(client_sock, &fds)) {
            ssize_t n = read(client_sock, buffer, sizeof(buffer) - 1);
            if (n <= 0) break; // Client disconnected
            write(tx_to_parent, buffer, n);
        }

        // 2. Broadcast received from parent dispatcher -> write to network client
        if (FD_ISSET(rx_from_parent, &fds)) {
            ssize_t n = read(rx_from_parent, buffer, sizeof(buffer) - 1);
            if (n <= 0) break;
            write(client_sock, buffer, n);
        }
    }

    close(client_sock);
    close(rx_from_parent);
    close(tx_to_parent);
    exit(0);
}

int main() {
    int server_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_size;
    int opt = 1;
    ClientSession sessions[MAX_CLIENTS];

    for (int i = 0; i < MAX_CLIENTS; i++) {
        sessions[i].active = 0;
    }

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
    listen(server_sock, 5);

    while (1) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_sock, &read_fds);
        int max_fd = server_sock;

        // Watch active children for outgoing broadcast traffic
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (sessions[i].active) {
                FD_SET(sessions[i].from_child_fd, &read_fds);
                if (sessions[i].from_child_fd > max_fd) {
                    max_fd = sessions[i].from_child_fd;
                }
            }
        }

        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) continue;

        // Handle a new incoming connection
        if (FD_ISSET(server_sock, &read_fds)) {
            addr_size = sizeof(client_addr);
            int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addr_size);
            if (client_sock >= 0) {
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (!sessions[i].active) {
                        slot = i;
                        break;
                    }
                }

                if (slot == -1) {
                    printf("Server capacity reached. Dropping client.\n");
                    close(client_sock);
                } else {
                    int p_to_c[2]; // Parent -> Child pipe
                    int c_to_p[2]; // Child -> Parent pipe
                    pipe(p_to_c);
                    pipe(c_to_p);

                    pid_t pid = fork();
                    if (pid == 0) {
                        // Child process
                        close(server_sock);
                        close(p_to_c[1]);
                        close(c_to_p[0]);

                        // Close inherited parent pipe handles
                        for (int j = 0; j < MAX_CLIENTS; j++) {
                            if (sessions[j].active) {
                                close(sessions[j].to_child_fd);
                                close(sessions[j].from_child_fd);
                            }
                        }

                        run_child_worker(client_sock, p_to_c[0], c_to_p[1]);
                    } else {
                        // Parent dispatcher process
                        close(client_sock);
                        close(p_to_c[0]);
                        close(c_to_p[1]);

                        sessions[slot].active = 1;
                        sessions[slot].child_pid = pid;
                        sessions[slot].to_child_fd = p_to_c[1];
                        sessions[slot].from_child_fd = c_to_p[0];
                    }
                }
            }
        }

        // Check if any child process forwarded a message to broadcast
        char msg_buf[BUFFER_SIZE];
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (sessions[i].active && FD_ISSET(sessions[i].from_child_fd, &read_fds)) {
                ssize_t bytes = read(sessions[i].from_child_fd, msg_buf, sizeof(msg_buf) - 1);
                if (bytes <= 0) {
                    // Child disconnected
                    close(sessions[i].to_child_fd);
                    close(sessions[i].from_child_fd);
                    waitpid(sessions[i].child_pid, NULL, WNOHANG);
                    sessions[i].active = 0;
                } else {
                    // Broadcast to ALL OTHER active clients
                    for (int j = 0; j < MAX_CLIENTS; j++) {
                        if (sessions[j].active && j != i) {
                            write(sessions[j].to_child_fd, msg_buf, bytes);
                        }
                    }
                }
            }
        }

        while (waitpid(-1, NULL, WNOHANG) > 0);
    }

    close(server_sock);
    return 0;
}
