// baking.c - Exercise 5: Baking with Condition Variables
// Compile: gcc -o baking baking.c -pthread
//
// One shared bowl, five threads: 1 batter adder, 2 egg breakers, 1 heater,
// 1 eater. A cake needs exactly 1 batter + 2 eggs. One mutex protects the
// bowl; three condition variables say WHY a thread is waiting:
//   needIngredients - adders wait here while the bowl has enough of their
//                     ingredient or a cake is waiting to be eaten
//   readyToBake     - heater waits here until 1 batter + 2 eggs are in
//   startEating     - eater waits here until a cake is baked
// Every wait is a `while` loop (Mesa semantics), so a thread that wakes up
// re-checks the bowl before acting. Nothing is added while a cake is in the
// bowl (readyToEat blocks the adders) and the heater never bakes an
// incomplete bowl. The assert() calls check those rules on every step.
//
// Fixes to the starter: added #include <stdio.h>, <stdbool.h> (bool),
// <unistd.h>; added output; a `done` flag stops all threads after NUM_CAKES
// cakes instead of running forever, so the run fits in a screenshot.
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>

#define NUM_CAKES 5

int numBatterInBowl = 0;
int numEggInBowl = 0;
bool readyToEat = false;
int cakesEaten = 0;
bool done = false;          // added: set after NUM_CAKES so threads can exit
pthread_mutex_t lock;
pthread_cond_t needIngredients, readyToBake, startEating;

void addBatter() {
    assert(!readyToEat && numBatterInBowl == 0);   // never add to a baked cake
    numBatterInBowl += 1;
    printf("  batter added            bowl: batter=%d eggs=%d\n", numBatterInBowl, numEggInBowl);
}
void addEgg(long who) {
    assert(!readyToEat && numEggInBowl < 2);
    numEggInBowl += 1;
    printf("  egg added (breaker %ld)   bowl: batter=%d eggs=%d\n", who, numBatterInBowl, numEggInBowl);
}
void heatBowl() {
    assert(numBatterInBowl == 1 && numEggInBowl == 2 && !readyToEat);  // only a complete bowl
    readyToEat = true; numBatterInBowl = 0; numEggInBowl = 0;
    printf("  bowl heated -> cake ready\n");
}
void eatCake() {
    assert(readyToEat);
    readyToEat = false;
    cakesEaten++;
    printf("Cake %d eaten!\n", cakesEaten);
}

void* batterAdder(void* arg) {
    (void)arg;
    pthread_mutex_lock(&lock);
    while (1) {
        while ((numBatterInBowl || readyToEat) && !done) {
            pthread_cond_wait(&needIngredients, &lock);
        }
        if (done) break;
        addBatter();
        pthread_cond_signal(&readyToBake);
        pthread_mutex_unlock(&lock);
        pthread_mutex_lock(&lock);
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

void* eggBreaker(void* arg) {
    long who = (long)arg;
    pthread_mutex_lock(&lock);
    while (1) {
        while ((numEggInBowl >= 2 || readyToEat) && !done) {
            pthread_cond_wait(&needIngredients, &lock);
        }
        if (done) break;
        addEgg(who);
        pthread_cond_signal(&readyToBake);
        pthread_mutex_unlock(&lock);
        pthread_mutex_lock(&lock);
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

void* bowlHeater(void* arg) {
    (void)arg;
    pthread_mutex_lock(&lock);
    while (1) {
        while ((numBatterInBowl < 1 || numEggInBowl < 2 || readyToEat) && !done) {
            pthread_cond_wait(&readyToBake, &lock);
        }
        if (done) break;
        heatBowl();
        pthread_cond_signal(&startEating);
        pthread_mutex_unlock(&lock);
        pthread_mutex_lock(&lock);
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

void* cakeEater(void* arg) {
    (void)arg;
    pthread_mutex_lock(&lock);
    while (1) {
        while (!readyToEat && !done) {
            pthread_cond_wait(&startEating, &lock);
        }
        if (done) break;
        eatCake();
        if (cakesEaten == NUM_CAKES) {    // stop: wake everyone so they see done
            done = true;
            pthread_cond_broadcast(&readyToBake);
            pthread_cond_broadcast(&startEating);
        }
        pthread_cond_broadcast(&needIngredients);   // wake ALL adders: bowl is empty
        pthread_mutex_unlock(&lock);
        pthread_mutex_lock(&lock);
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    pthread_mutex_init(&lock, NULL);
    pthread_cond_init(&needIngredients, NULL);
    pthread_cond_init(&readyToBake, NULL);
    pthread_cond_init(&startEating, NULL);

    pthread_t batter, egg1, egg2, heater, eater;
    pthread_create(&batter, NULL, batterAdder, NULL);
    pthread_create(&egg1, NULL, eggBreaker, (void*)1L);
    pthread_create(&egg2, NULL, eggBreaker, (void*)2L);
    pthread_create(&heater, NULL, bowlHeater, NULL);
    pthread_create(&eater, NULL, cakeEater, NULL);

    // The starter slept forever here; instead the threads stop after NUM_CAKES.
    pthread_join(batter, NULL);
    pthread_join(egg1, NULL);
    pthread_join(egg2, NULL);
    pthread_join(heater, NULL);
    pthread_join(eater, NULL);
    printf("Baked and ate %d cakes. Every bowl had exactly 1 batter + 2 eggs.\n", cakesEaten);
    return 0;
}
