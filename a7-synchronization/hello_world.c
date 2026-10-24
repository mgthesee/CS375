// hello_world.c - Exercise 1: Hello World
// Compile: gcc -o hello_world hello_world.c -pthread
//
// Fixes:
//  1. #include <stdio.h> was missing (printf undeclared -> compile error/warning).
//  2. The mutex and condition variable are initialized before any thread uses
//     them (pthread_mutex_init / pthread_cond_init before pthread_create).
//  3. main waits with `while (hello < 1)` around pthread_cond_wait, holding the
//     lock, so it can never print before the child has incremented hello:
//       - if the child runs first, hello is already 1 and main never waits
//       - if main runs first, it sleeps until the child signals
//     The while loop (not `if`) also handles spurious wakeups.
#include <stdio.h>
#include <pthread.h>

pthread_mutex_t lock;
pthread_cond_t cv;
int hello = 0;

void* print_hello(void* arg) {
    (void)arg;
    pthread_mutex_lock(&lock);
    hello += 1;
    printf("First line (hello=%d)\n", hello);
    pthread_cond_signal(&cv);          // wake main if it is already waiting
    pthread_mutex_unlock(&lock);
    pthread_exit(0);
}

int main() {
    pthread_t thread;
    pthread_mutex_init(&lock, NULL);   // initialize BEFORE creating the thread
    pthread_cond_init(&cv, NULL);

    pthread_create(&thread, NULL, print_hello, NULL);
    pthread_mutex_lock(&lock);
    while (hello < 1) {                // re-check the condition after every wakeup
        pthread_cond_wait(&cv, &lock); // atomically releases lock and sleeps
    }
    printf("Second line (hello=%d)\n", hello);
    pthread_mutex_unlock(&lock);

    pthread_join(thread, NULL);
    pthread_mutex_destroy(&lock);
    pthread_cond_destroy(&cv);
    return 0;
}
