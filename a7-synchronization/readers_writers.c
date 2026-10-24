// readers_writers.c - Exercise 8: Readers-Writers with Writer Priority
// Compile: gcc -o readers_writers readers_writers.c -pthread
//
// Rules:
//  - Any number of readers may read at the same time.
//  - A writer needs the data alone (no readers, no other writer).
//  - Writer priority: once a writer is WAITING, new readers must wait too,
//    even if other readers are currently reading. Readers already inside
//    finish; then the writer goes before every waiting reader.
//
// State (protected by `lock`): reader_count (readers reading now),
// writer_active (a writer is writing now), writer_waiting (writers in line).
//   reader waits while  writer_active || writer_waiting > 0
//   writer waits while  writer_active || reader_count > 0
// When a writer finishes it hands off to the next WRITER first (writer_cv);
// only if no writer is waiting does it wake all readers (reader_cv broadcast).
//
// Changes from the starter: #include <stdio.h>/<unistd.h> were missing; the
// starter wrote while holding the mutex (so readers and writers were all
// serialized, and a finished writer always woke readers even if another
// writer was waiting). Here reading and writing happen OUTSIDE the mutex, and
// the scenario below uses timed arrivals so writer priority is visible.
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>

pthread_mutex_t lock;
pthread_cond_t reader_cv, writer_cv;
int reader_count = 0, writer_waiting = 0, writer_active = 0;
int shared_data = 0;

static struct timeval start;
static long ms(void) {                // milliseconds since start, rounded to 50
    struct timeval now;
    gettimeofday(&now, NULL);
    long t = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000;
    return (t + 25) / 50 * 50;
}

typedef struct { const char* name; int arrive_ms; int duration_ms; } job;

void* reader(void* arg) {
    job* j = arg;
    usleep(j->arrive_ms * 1000);
    pthread_mutex_lock(&lock);
    printf("%4ld ms  %s arrives (readers=%d, writer active=%d, writers waiting=%d)\n", ms(), j->name, reader_count, writer_active, writer_waiting);
    while (writer_active || writer_waiting > 0) {   // writers go first
        pthread_cond_wait(&reader_cv, &lock);
    }
    reader_count++;
    pthread_mutex_unlock(&lock);

    printf("%4ld ms  %s reads: %d\n", ms(), j->name, shared_data);   // many readers at once
    usleep(j->duration_ms * 1000);

    pthread_mutex_lock(&lock);
    reader_count--;
    printf("%4ld ms  %s done reading\n", ms(), j->name);
    if (reader_count == 0) {
        pthread_cond_signal(&writer_cv);             // last reader lets a writer in
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

void* writer(void* arg) {
    job* j = arg;
    usleep(j->arrive_ms * 1000);
    pthread_mutex_lock(&lock);
    printf("%4ld ms  %s arrives (readers=%d, writer active=%d)\n", ms(), j->name, reader_count, writer_active);
    writer_waiting++;
    while (writer_active || reader_count > 0) {
        pthread_cond_wait(&writer_cv, &lock);
    }
    writer_waiting--;
    writer_active = 1;
    pthread_mutex_unlock(&lock);

    shared_data++;                                   // exclusive access
    printf("%4ld ms  %s writes: %d\n", ms(), j->name, shared_data);
    usleep(j->duration_ms * 1000);

    pthread_mutex_lock(&lock);
    writer_active = 0;
    printf("%4ld ms  %s done writing\n", ms(), j->name);
    if (writer_waiting > 0) pthread_cond_signal(&writer_cv);   // next writer first
    else pthread_cond_broadcast(&reader_cv);                   // then all readers
    pthread_mutex_unlock(&lock);
    return NULL;
}

int main() {
    // R1, R2 read together; W1 arrives while they read; R3 arrives after W1 is
    // waiting, so it must wait even though readers are still active; W2 arrives
    // later and still goes before R3.
    job readers[3] = {{"R1", 0, 400}, {"R2", 50, 400}, {"R3", 250, 200}};
    job writers[2] = {{"W1", 150, 200}, {"W2", 300, 200}};
    pthread_t rt[3], wt[2];
    pthread_mutex_init(&lock, NULL);
    pthread_cond_init(&reader_cv, NULL);
    pthread_cond_init(&writer_cv, NULL);
    gettimeofday(&start, NULL);

    for (int i = 0; i < 3; i++) pthread_create(&rt[i], NULL, reader, &readers[i]);
    for (int i = 0; i < 2; i++) pthread_create(&wt[i], NULL, writer, &writers[i]);

    for (int i = 0; i < 3; i++) pthread_join(rt[i], NULL);
    for (int i = 0; i < 2; i++) pthread_join(wt[i], NULL);
    printf("Final shared_data = %d\n", shared_data);

    pthread_mutex_destroy(&lock);
    pthread_cond_destroy(&reader_cv);
    pthread_cond_destroy(&writer_cv);
    return 0;
}
