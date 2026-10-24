// barrier.c - Exercise 7: Barrier Synchronization
// Compile: gcc -o barrier barrier.c -pthread
//
// A barrier makes every thread wait until all NUM_THREADS have arrived.
// count = how many threads have arrived in the current round. The last thread
// to arrive resets count and broadcasts, which wakes everyone at once.
//
// Problems with the starter's barrier, fixed here:
//  1. #include <stdio.h> (and <unistd.h> for usleep) was missing.
//  2. It used `if (count < NUM_THREADS) pthread_cond_wait(...)`. A condition
//     variable can wake up spuriously, and with `if` a thread would then walk
//     through the barrier early. It must wait in a loop.
//  3. The loop can't simply test `count`, because the last thread resets
//     count to 0 for the next round. A `generation` number fixes this: each
//     thread remembers the generation it arrived in and waits until it
//     changes. That also makes the barrier reusable, so threads can meet at
//     it round after round (a fast thread can't slip into the next round and
//     get counted twice).
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#define NUM_THREADS 4
#define ROUNDS 3

pthread_mutex_t lock;
pthread_cond_t cv;
int count = 0;
int generation = 0;

int finished_work[ROUNDS];   // test: how many threads finished each round's work

void barrier() {
    pthread_mutex_lock(&lock);
    int my_generation = generation;
    count++;
    if (count < NUM_THREADS) {
        while (my_generation == generation)          // loop: spurious wakeups
            pthread_cond_wait(&cv, &lock);
    } else {
        count = 0;                                    // last to arrive: open the barrier
        generation++;
        pthread_cond_broadcast(&cv);
    }
    pthread_mutex_unlock(&lock);
}

void* worker(void* arg) {
    int id = *(int*)arg;
    unsigned seed = id * 7919u;
    for (int round = 0; round < ROUNDS; round++) {
        usleep((rand_r(&seed) % 300 + 50) * 1000);    // work of random length
        pthread_mutex_lock(&lock);
        finished_work[round]++;
        printf("Round %d  Thread %d: Before barrier (finished work)\n", round + 1, id);
        pthread_mutex_unlock(&lock);

        barrier();

        pthread_mutex_lock(&lock);
        // Check: nobody gets past the barrier until all threads finished this round.
        printf("Round %d  Thread %d: After barrier  (%d/%d finished round %d)%s\n", round + 1, id,
               finished_work[round], NUM_THREADS, round + 1, finished_work[round] == NUM_THREADS ? "" : "  ERROR");
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

int main() {
    pthread_t threads[NUM_THREADS];
    int ids[NUM_THREADS];
    pthread_mutex_init(&lock, NULL);
    pthread_cond_init(&cv, NULL);

    for (int i = 0; i < NUM_THREADS; i++) {
        ids[i] = i;
        pthread_create(&threads[i], NULL, worker, &ids[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    printf("All %d threads passed %d barriers together.\n", NUM_THREADS, ROUNDS);

    pthread_mutex_destroy(&lock);
    pthread_cond_destroy(&cv);
    return 0;
}
