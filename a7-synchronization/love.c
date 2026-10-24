// love.c - Exercise 3: I Love You, Unconditionally!
// Compile: gcc -o love love.c -pthread
//
// Without synchronization, main can read subaru before the helper thread
// increments it and print "I love Rem!". The fix:
//  1. #include <stdio.h> was missing.
//  2. Both threads touch subaru only while holding `lock`.
//  3. main waits in `while (subaru != 1)`. Under Mesa semantics a signal is
//     only a hint, so main re-checks subaru after every wakeup; it leaves the
//     loop only when subaru really is 1. It assumes nothing about which
//     thread the scheduler runs first:
//       - helper first: subaru is already 1, main never waits
//       - main first:   main sleeps (releasing the lock) until helper signals
//     Either way the if-test sees subaru == 1 and prints "I love Emilia!".
#include <stdio.h>
#include <pthread.h>

pthread_mutex_t lock;
pthread_cond_t cv;
int subaru = 0;

void* helper(void* arg) {
    (void)arg;
    pthread_mutex_lock(&lock);
    subaru += 1;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&lock);
    pthread_exit(0);
}

int main() {
    pthread_t thread;
    pthread_mutex_init(&lock, NULL);
    pthread_cond_init(&cv, NULL);

    pthread_create(&thread, NULL, helper, NULL);
    pthread_mutex_lock(&lock);
    while (subaru != 1) {              // Mesa semantics: always re-check
        pthread_cond_wait(&cv, &lock);
    }
    if (subaru == 1)
        printf("I love Emilia!\n");
    else
        printf("I love Rem!\n");
    pthread_mutex_unlock(&lock);

    pthread_join(thread, NULL);
    pthread_mutex_destroy(&lock);
    pthread_cond_destroy(&cv);
    return 0;
}
