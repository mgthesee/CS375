// thread_pool.c - Exercise 9: Thread Pool
// Compile: gcc -o thread_pool thread_pool.c -pthread
//
// NUM_THREADS worker threads are created once and reused. Tasks go into a
// bounded ThreadSafeQueue (a circular buffer protected by a mutex):
//   queue_push waits on not_full  while the queue is full, then signals not_empty
//   queue_pop  waits on not_empty while the queue is empty, then signals not_full
//
// Fixes to the starter:
//  - #include <stdio.h>/<unistd.h> were missing.
//  - queue_pop returned immediately when the queue was empty, so workers
//    busy-polled with sleep(1): every task waited up to 1 s for nothing.
//    Workers now sleep on not_empty and wake as soon as a task is pushed.
//  - There was no way to stop the workers, and main just slept 5 s and hoped
//    all tasks were done. Now pool_shutdown() sets a flag, broadcasts, and
//    joins the workers after they drain the queue.
//
// The second half of the program compares the pool with creating a new
// thread for every task.
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#define NUM_THREADS 4

typedef struct {
    void (*task)(int);
    int arg;
} Task;

typedef struct {
    Task* queue;
    int head, tail, count, size;
    int shutdown;                       // added: tells workers to exit
    pthread_mutex_t lock;
    pthread_cond_t not_empty, not_full;
} ThreadSafeQueue;

void queue_init(ThreadSafeQueue* q, int size) {
    q->queue = malloc(sizeof(Task) * size);
    q->head = q->tail = q->count = 0;
    q->size = size;
    q->shutdown = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

void queue_push(ThreadSafeQueue* q, Task task) {
    pthread_mutex_lock(&q->lock);
    while (q->count == q->size) {
        pthread_cond_wait(&q->not_full, &q->lock);
    }
    q->queue[q->tail] = task;
    q->tail = (q->tail + 1) % q->size;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
}

// Blocks until a task is available. Returns 0 only when shutting down and empty.
int queue_pop(ThreadSafeQueue* q, Task* task) {
    pthread_mutex_lock(&q->lock);
    while (q->count == 0 && !q->shutdown) {
        pthread_cond_wait(&q->not_empty, &q->lock);
    }
    if (q->count == 0) {                // shutdown and nothing left
        pthread_mutex_unlock(&q->lock);
        return 0;
    }
    *task = q->queue[q->head];
    q->head = (q->head + 1) % q->size;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return 1;
}

typedef struct { ThreadSafeQueue* q; int id; } WorkerArg;
static __thread int worker_id = -1;     // which worker runs the current task
static int verbose = 1;

void* worker(void* arg) {
    WorkerArg* w = arg;
    worker_id = w->id;
    Task task;
    while (queue_pop(w->q, &task)) {
        task.task(task.arg);
    }
    return NULL;
}

void pool_start(pthread_t* threads, WorkerArg* args, ThreadSafeQueue* q) {
    for (int i = 0; i < NUM_THREADS; i++) {
        args[i] = (WorkerArg){q, i};
        pthread_create(&threads[i], NULL, worker, &args[i]);
    }
}

void pool_shutdown(pthread_t* threads, ThreadSafeQueue* q) {
    pthread_mutex_lock(&q->lock);
    q->shutdown = 1;
    pthread_cond_broadcast(&q->not_empty);   // wake every idle worker
    pthread_mutex_unlock(&q->lock);
    for (int i = 0; i < NUM_THREADS; i++) pthread_join(threads[i], NULL);
}

void sample_task(int arg) {
    usleep(100000);                          // pretend to work for 0.1 s
    if (verbose) printf("Task %d executed by worker %d\n", arg, worker_id);
}

// ---- tiny task for the benchmark ----
static long sink;
void tiny_task(int arg) { __atomic_fetch_add(&sink, arg, __ATOMIC_RELAXED); }
void* tiny_thread(void* arg) { tiny_task((int)(long)arg); return NULL; }

static double seconds(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
}

int main() {
    ThreadSafeQueue q;
    queue_init(&q, 10);
    pthread_t threads[NUM_THREADS];
    WorkerArg args[NUM_THREADS];
    struct timespec t0, t1;

    printf("== 10 tasks on a pool of %d workers ==\n", NUM_THREADS);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pool_start(threads, args, &q);
    for (int i = 0; i < 10; i++) {
        queue_push(&q, (Task){sample_task, i});
    }
    pool_shutdown(threads, &q);                // waits for all tasks to finish
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("10 tasks x 0.1 s finished in %.2f s (4 run at a time)\n", seconds(t0, t1));

    // ---- efficiency: pool vs. one new thread per task ----
    const int N = 20000;
    verbose = 0;
    printf("\n== Benchmark: %d tiny tasks ==\n", N);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < N; i++) {
        pthread_t t;
        pthread_create(&t, NULL, tiny_thread, (void*)(long)i);
        pthread_join(t, NULL);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double per_task = seconds(t0, t1);
    printf("New thread per task: %.3f s (%d threads created)\n", per_task, N);

    ThreadSafeQueue q2;
    queue_init(&q2, 64);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pool_start(threads, args, &q2);
    for (int i = 0; i < N; i++) queue_push(&q2, (Task){tiny_task, i});
    pool_shutdown(threads, &q2);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double pooled = seconds(t0, t1);
    printf("Thread pool:         %.3f s (%d threads created)\n", pooled, NUM_THREADS);
    printf("Pool was %.1fx faster\n", per_task / pooled);

    free(q.queue);
    free(q2.queue);
    return 0;
}
