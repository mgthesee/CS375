#include <iostream>
#include <unistd.h>
#include <netinet/in.h>
#include <cstring>
#include <sys/wait.h>
#include <sys/mman.h>
#include <semaphore.h>

#define PORT 8080

struct SharedData {
    int count;
    sem_t mutex;
};

void handle_client(int client_sock, SharedData* shared) {
    char buffer[1024];
    read(client_sock, buffer, sizeof(buffer) - 1);
    write(client_sock, "Hello from C++ server", 22);
    close(client_sock);

    sem_wait(&shared->mutex);
    shared->count--;
    std::cout << "Client disconnected. Active clients: " << shared->count << std::endl;
    sem_post(&shared->mutex);
}

int main() {
    int server_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_size;
    int opt = 1;

    SharedData* shared = (SharedData*)mmap(NULL, sizeof(SharedData),
                                           PROT_READ | PROT_WRITE,
                                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    shared->count = 0;
    sem_init(&shared->mutex, 1, 1);

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
    listen(server_sock, 5);

    while (true) {
        addr_size = sizeof(client_addr);
        client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addr_size);
        if (client_sock < 0) continue;

        sem_wait(&shared->mutex);
        shared->count++;
        std::cout << "Client connected. Active clients: " << shared->count << std::endl;
        sem_post(&shared->mutex);

        if (fork() == 0) {
            close(server_sock);
            handle_client(client_sock, shared);
            exit(0);
        }
        close(client_sock);
        while (waitpid(-1, NULL, WNOHANG) > 0);
    }

    close(server_sock);
    sem_destroy(&shared->mutex);
    munmap(shared, sizeof(SharedData));
    return 0;
}
