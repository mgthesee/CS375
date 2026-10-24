// floopy.c - Exercise 4: Locking Up the Floopies
// Compile: gcc -o floopy floopy.c -pthread
//
// The deadlock: the original transfer() locked donor first, then recipient.
//   Thread 1: transfer(acc1 -> acc2)  locks acc1, then wants acc2
//   Thread 2: transfer(acc2 -> acc1)  locks acc2, then wants acc1
// Each thread holds the lock the other needs, so both wait forever (circular
// wait). The fix: always lock the account with the smaller uuid first. Both
// threads then try acc1 first; whoever gets it also gets acc2, and the other
// waits for acc1 while holding nothing, so no cycle can form.
//
// Other bugs fixed in the starter:
//  - #include <stdio.h> was missing.
//  - pthread_create was given transfer() cast to void*(*)(void*). A thread
//    function receives ONE pointer, so donor became the struct pointer and
//    recipient/amount were garbage (undefined behavior, usually a crash).
//    A wrapper, transfer_thread(), now unpacks a params struct.
//
// This program first demonstrates the original lock order (using a timed lock
// so the deadlock is detected instead of hanging), then runs the fixed version
// under load and checks that no money was created or lost.
#define _DEFAULT_SOURCE   // usleep, pthread_mutex_timedlock
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

typedef struct account_t {
    pthread_mutex_t lock;
    int balance;
    long uuid;
} account_t;

typedef struct {
    account_t* donor;
    account_t* recipient;
    float amount;
    int repeat;      // how many transfers this thread performs
    int verbose;     // print each transfer
} transfer_args;

// ---------------- fixed version: consistent lock order ----------------
void transfer(account_t *donor, account_t *recipient, float amount, int verbose) {
    // Lock accounts in order based on uuid to avoid deadlock
    account_t *first = (donor->uuid < recipient->uuid) ? donor : recipient;
    account_t *second = (donor->uuid < recipient->uuid) ? recipient : donor;

    pthread_mutex_lock(&first->lock);
    pthread_mutex_lock(&second->lock);

    if (donor->balance < amount) {
        if (verbose) printf("Insufficient funds.\n");
    } else {
        donor->balance -= amount;
        recipient->balance += amount;
        if (verbose)
            printf("Transferred %.2f from account %ld to %ld\n", amount, donor->uuid, recipient->uuid);
    }

    pthread_mutex_unlock(&second->lock);
    pthread_mutex_unlock(&first->lock);
}

void* transfer_thread(void* arg) {
    transfer_args* a = arg;
    for (int i = 0; i < a->repeat; i++) transfer(a->donor, a->recipient, a->amount, a->verbose);
    return NULL;
}

// ---------------- original lock order (for the demonstration) ----------------
// Locks donor first, then recipient. Uses a 2-second timed lock on the second
// account so that, instead of hanging forever, the program reports the deadlock.
void* unsafe_transfer_thread(void* arg) {
    transfer_args* a = arg;
    pthread_mutex_lock(&a->donor->lock);
    printf("  [original] thread moving %ld->%ld locked account %ld\n", a->donor->uuid, a->recipient->uuid, a->donor->uuid);
    usleep(100000);   // widen the window so both threads hold one lock

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 2;
    if (pthread_mutex_timedlock(&a->recipient->lock, &deadline) != 0) {
        printf("  [original] DEADLOCK: holds account %ld, waited 2s for account %ld (held by the other thread)\n"
               "             -> without the timeout both threads would wait forever; backing off\n",
               a->donor->uuid, a->recipient->uuid);
        pthread_mutex_unlock(&a->donor->lock);
        return NULL;
    }
    a->donor->balance -= a->amount;
    a->recipient->balance += a->amount;
    printf("  [original] transferred %.2f from %ld to %ld\n", a->amount, a->donor->uuid, a->recipient->uuid);
    pthread_mutex_unlock(&a->recipient->lock);
    pthread_mutex_unlock(&a->donor->lock);
    return NULL;
}

int main() {
    account_t acc1 = {PTHREAD_MUTEX_INITIALIZER, 1000, 1};
    account_t acc2 = {PTHREAD_MUTEX_INITIALIZER, 500, 2};
    pthread_t t1, t2;

    printf("== Original lock order (donor first) ==\n");
    transfer_args u1 = {&acc1, &acc2, 200, 1, 1}, u2 = {&acc2, &acc1, 100, 1, 1};
    pthread_create(&t1, NULL, unsafe_transfer_thread, &u1);
    pthread_create(&t2, NULL, unsafe_transfer_thread, &u2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    acc1.balance = 1000;   // reset after the demo
    acc2.balance = 500;
    printf("\n== Fixed: lock lower uuid first ==\n");
    transfer_args a1 = {&acc1, &acc2, 200, 1, 1}, a2 = {&acc2, &acc1, 100, 1, 1};
    pthread_create(&t1, NULL, transfer_thread, &a1);
    pthread_create(&t2, NULL, transfer_thread, &a2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    printf("Balances: account 1 = %d, account 2 = %d\n", acc1.balance, acc2.balance);

    printf("\n== Stress test: 100000 transfers each way at the same time ==\n");
    int total_before = acc1.balance + acc2.balance;
    transfer_args s1 = {&acc1, &acc2, 1, 100000, 0}, s2 = {&acc2, &acc1, 1, 100000, 0};
    pthread_create(&t1, NULL, transfer_thread, &s1);
    pthread_create(&t2, NULL, transfer_thread, &s2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    printf("Finished without deadlock. Total before = %d, after = %d (%s)\n", total_before,
           acc1.balance + acc2.balance, total_before == acc1.balance + acc2.balance ? "no money lost" : "MISMATCH");
    return 0;
}
