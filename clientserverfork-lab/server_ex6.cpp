#include <iostream>
#include <fstream>
#include <ctime>
#include <unistd.h>
#include <netinet/in.h>
#include <cstring>
#include <sys/wait.h>

#define PORT 8080

void log_error(const std::string& message) {
    std::ofstream log_file("server_errors.log", std::ios::app);
    if (!log_file.is_open()) return;

    std::time_t now = std::time(nullptr);
    char time_str[64];
    std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

    log_file << "[" << time_str << "] " << message << ": " << std::strerror(errno) << std::endl;
}

void handle_client(int client_sock) {
    char buffer[1024];
    ssize_t bytes_read = read(client_sock, buffer, sizeof(buffer) - 1);
    if (bytes_read < 0) {
        log_error("read() failed");
    } else {
        buffer[bytes_read] = '\0';
        std::cout << "Received: " << buffer << std::endl;
        if (write(client_sock, "Hello from C++ server", 22) < 0) {
            log_error("write() failed");
        }
    }
    close(client_sock);
}

int main() {
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        log_error("socket() failed");
        return 1;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in server_addr{}, client_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        log_error("bind() failed");
        close(server_sock);
        return 1;
    }

    if (listen(server_sock, 5) < 0) {
        log_error("listen() failed");
        close(server_sock);
        return 1;
    }

    while (true) {
        socklen_t addr_size = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addr_size);
        if (client_sock < 0) {
            log_error("accept() failed");
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            log_error("fork() failed");
            close(client_sock);
            continue;
        }

        if (pid == 0) {
            close(server_sock);
            handle_client(client_sock);
            exit(0);
        }

        close(client_sock);
        while (waitpid(-1, NULL, WNOHANG) > 0);
    }

    close(server_sock);
    return 0;
}
