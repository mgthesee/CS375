// priority_transfer.c - Exercise 6: Priority Donation in Transfer
// Compile: gcc -o priority_transfer priority_transfer.c -pthread
//
// Priority inversion: a HIGH-priority thread needs an account lock held by a
// LOW-priority thread. A MEDIUM-priority thread that needs no lock at all keeps
// preempting LOW, so LOW never gets to release the lock and HIGH waits behind
// MEDIUM -- the high-priority thread effectively runs at the lowest priority.
//
// Priority donation: when HIGH blocks on a lock, it donates its priority to the
// lock holder. LOW now runs at HIGH's priority, MEDIUM can no longer preempt
// it, LOW finishes its transfer quickly and releases the lock, drops back to
// its own priority, and HIGH runs next.
//
// Real Linux priority scheduling needs root (SCHED_FIFO), so -- as the handout
// allows -- each thread has a custom priority field and the program simulates
// ONE CPU: a real pthread may only do a unit of work when it is the
// highest-priority ready thread (sched_lock + sched_cv enforce that). Account
// locks record their owner so the holder can be found and given the donated
// priority. (Real POSIX code can get the same effect with the mutex attribute
// PTHREAD_PRIO_INHERIT.) The same scenario runs without and with donation.
#include <stdio.h>
#include <pthread.h>

struct account_t;
typedef struct sim_thread {
    const char* name;
    int base_priority;      // its own priority (higher number = more important)
    int priority;           // effective priority (base or donated)
    int arrival;            // first tick at which it may run
    struct account_t* waiting_on;   // account lock it is blocked on (NULL = none)
    int finished, locks_held;
} sim_thread;

typedef struct account_t {
    sim_thread* owner;      // thread holding this account's lock (NULL = free)
    int balance;
    long uuid;
    int priority;           // priority donated to the holder through this lock
} account_t;

// ---------------- simulated single CPU ----------------
static pthread_mutex_t sched_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sched_cv = PTHREAD_COND_INITIALIZER;
static sim_thread* threads[3];
static int tick = 0;
static int donation_on = 0;

// Highest-priority thread that has arrived and is neither blocked nor done.
static sim_thread* pick(void) {
    sim_thread* best = NULL;
    for (int i = 0; i < 3; i++) {
        sim_thread* t = threads[i];
        int blocked = t->waiting_on && t->waiting_on->owner && t->waiting_on->owner != t;
        if (t->finished || blocked || t->arrival > tick) continue;
        if (!best || t->priority > best->priority) best = t;
    }
    return best;
}

// Wait until this thread owns the (simulated) CPU. Caller holds sched_lock.
static void wait_for_cpu(sim_thread* me) {
    while (pick() != me) pthread_cond_wait(&sched_cv, &sched_lock);
}

// Do one time unit of work.
static void run(sim_thread* me, const char* what) {
    pthread_mutex_lock(&sched_lock);
    wait_for_cpu(me);
    printf("t=%2d  %-6s priority %d%s  %s\n", tick, me->name, me->priority,
           me->priority != me->base_priority ? " (donated)" : "          ", what);
    tick++;
    pthread_cond_broadcast(&sched_cv);   // next tick: let the scheduler re-decide
    pthread_mutex_unlock(&sched_lock);
}

static void account_lock(account_t* a, sim_thread* me) {
    pthread_mutex_lock(&sched_lock);
    for (;;) {
        wait_for_cpu(me);
        if (a->owner == NULL) break;
        sim_thread* holder = a->owner;
        printf("      %s blocks on account %ld (held by %s, priority %d)\n", me->name, a->uuid, holder->name, holder->priority);
        if (donation_on && holder->priority < me->priority) {
            holder->priority = me->priority;      // PRIORITY DONATION
            a->priority = me->priority;
            printf("      %s donates priority %d to %s\n", me->name, me->priority, holder->name);
        }
        me->waiting_on = a;             // blocked until a->owner becomes NULL
        pthread_cond_broadcast(&sched_cv);
    }
    me->waiting_on = NULL;
    a->owner = me;
    me->locks_held++;
    pthread_mutex_unlock(&sched_lock);
}

static void account_unlock(account_t* a, sim_thread* me) {
    pthread_mutex_lock(&sched_lock);
    a->owner = NULL;
    a->priority = 0;
    if (--me->locks_held == 0 && me->priority != me->base_priority) {
        printf("      %s releases its locks and drops back to priority %d\n", me->name, me->base_priority);
        me->priority = me->base_priority;    // give the donation back
    }
    pthread_cond_broadcast(&sched_cv);
    pthread_mutex_unlock(&sched_lock);
}

static void finish(sim_thread* me) {
    pthread_mutex_lock(&sched_lock);
    me->finished = 1;
    pthread_cond_broadcast(&sched_cv);
    pthread_mutex_unlock(&sched_lock);
}

// ---------------- the transfer (same lock order as floopy.c) ----------------
static int high_wait_start, high_got_lock;
static sim_thread LOW, MED, HIGH;

void transfer(account_t *donor, account_t *recipient, float amount, sim_thread* me, int work_units) {
    account_t *first = (donor->uuid < recipient->uuid) ? donor : recipient;
    account_t *second = (donor->uuid < recipient->uuid) ? recipient : donor;

    account_lock(first, me);
    if (me == &HIGH) high_got_lock = tick;
    account_lock(second, me);
    for (int i = 1; i <= work_units; i++) {
        char msg[64];
        snprintf(msg, sizeof msg, "transfer %ld->%ld, step %d/%d", donor->uuid, recipient->uuid, i, work_units);
        run(me, msg);
    }
    if (donor->balance < amount) {
        printf("      Insufficient funds.\n");
    } else {
        donor->balance -= amount;
        recipient->balance += amount;
        printf("      %s transferred %.0f from account %ld to %ld\n", me->name, amount, donor->uuid, recipient->uuid);
    }
    account_unlock(second, me);
    account_unlock(first, me);
}

static account_t acc1, acc2;

void* low_thread(void* arg) {    // priority 1: holds the locks for 3 units
    (void)arg;
    run(&LOW, "starts");
    transfer(&acc2, &acc1, 100, &LOW, 3);
    finish(&LOW);
    return NULL;
}
void* high_thread(void* arg) {   // priority 3: needs the same accounts
    (void)arg;
    run(&HIGH, "arrives, wants accounts 1 and 2");
    high_wait_start = tick;
    transfer(&acc1, &acc2, 200, &HIGH, 1);
    finish(&HIGH);
    return NULL;
}
void* medium_thread(void* arg) { // priority 2: pure CPU work, no locks
    (void)arg;
    for (int i = 1; i <= 5; i++) {
        char msg[32];
        snprintf(msg, sizeof msg, "computing %d/5 (no locks)", i);
        run(&MED, msg);
    }
    finish(&MED);
    return NULL;
}

void scenario(int donation) {
    donation_on = donation;
    tick = 0;
    acc1 = (account_t){NULL, 1000, 1, 0};
    acc2 = (account_t){NULL, 500, 2, 0};
    LOW  = (sim_thread){"LOW",    1, 1, 0, NULL, 0, 0};
    HIGH = (sim_thread){"HIGH",   3, 3, 2, NULL, 0, 0};
    MED  = (sim_thread){"MEDIUM", 2, 2, 3, NULL, 0, 0};
    threads[0] = &LOW; threads[1] = &HIGH; threads[2] = &MED;

    printf("\n===== %s priority donation =====\n", donation ? "WITH" : "WITHOUT");
    pthread_t tl, th, tm;
    pthread_create(&tl, NULL, low_thread, NULL);
    pthread_create(&th, NULL, high_thread, NULL);
    pthread_create(&tm, NULL, medium_thread, NULL);
    pthread_join(tl, NULL);
    pthread_join(th, NULL);
    pthread_join(tm, NULL);
    printf("Result: HIGH waited %d ticks for the lock; balances acc1=%d acc2=%d\n",
           high_got_lock - high_wait_start, acc1.balance, acc2.balance);
}

int main() {
    printf("LOW (priority 1) holds accounts 1+2, HIGH (3) needs them, MEDIUM (2) only uses the CPU.\n");
    scenario(0);
    scenario(1);
    return 0;
}
