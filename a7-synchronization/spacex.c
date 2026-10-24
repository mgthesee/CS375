// spacex.c - Exercise 2: SpaceX countdown
// Compile: gcc -o spacex spacex.c -pthread
//
// Fixes:
//  1. #include <stdio.h> was missing.
//  2. The counter decrements n only while holding the lock and signals the
//     announcer after every change.
//  3. The announcer checks its condition in a `while (n != 0)` loop around
//     pthread_cond_wait. Because of Mesa semantics a wakeup only means "n
//     changed", so the announcer goes back to sleep at n = 2 and n = 1 and
//     prints only when n == 0. If it used `if`, it could print after the
//     first signal (at n = 2), in the middle of the countdown.
//  4. If the announcer starts after the countdown already finished, n is 0,
//     it never waits, and it still prints exactly once.
#include <stdio.h>
#include <pthread.h>

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
int n = 3;

void* counter(void* arg) {
    (void)arg;
    pthread_mutex_lock(&lock);
    while (n > 0) {
        printf("%d\n", n);
        n--;
        pthread_cond_signal(&cv);      // tell the announcer n changed
        pthread_mutex_unlock(&lock);   // give the announcer a chance to run
        pthread_mutex_lock(&lock);
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

void* announcer(void* arg) {
    (void)arg;
    pthread_mutex_lock(&lock);
    while (n != 0) {                   // wait until the countdown reaches 0
        pthread_cond_wait(&cv, &lock);
    }
    printf("FALCON HEAVY TOUCH DOWN!\n");
    pthread_mutex_unlock(&lock);
    return NULL;
}

int main() {
    pthread_t t1, t2;
    pthread_create(&t1, NULL, counter, NULL);
    pthread_create(&t2, NULL, announcer, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    return 0;
}
