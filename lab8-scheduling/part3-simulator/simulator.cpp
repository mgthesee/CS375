// simulator.cpp - Modular Task Scheduling Simulator (CS375 Lab 8, Part 3)
//
// Build:  g++ -std=c++17 simulator.cpp -o simulator
// Usage:  ./simulator --scheduler <name>[,<name>...] [options]     (./simulator --help)
//
// Every scheduler derives from the abstract Scheduler class and implements
// schedule(). main() picks one at runtime through a factory (polymorphism), so
// adding an 11th scheduler means writing one class and one line in makeScheduler().
//
// Enhancements beyond the starter code:
//   * Priority: aging (effective priority improves the longer a task waits)
//   * EDF: deadlines from the input file (or arrival + factor * burst),
//          schedulability check, and a deadline met/missed report
//   * MLFQ: promotion of tasks that wait too long (starvation prevention)
//   * Lottery: compensation tickets for tasks that keep losing
//   * CFS: priority -> weight mapping (Linux nice-style, 1.25x per level)
//   * I/O simulation (all schedulers): a task can block for I/O after every
//     `io_every` units of CPU for `io_duration` units; the CPU runs someone else
//   * Workload generators: mixed / cpu / io / web (web-server requests)
//   * Response time, context switches, deadline misses, multi-scheduler comparison
//   * --output log file, input validation with line numbers

#include <iostream>
#include <vector>
#include <queue>
#include <deque>
#include <algorithm>
#include <string>
#include <fstream>
#include <sstream>
#include <random>
#include <chrono>
#include <map>
#include <memory>   // For smart pointers
#include <numeric>  // FIX: std::accumulate needs this header (missing in the starter)
#include <iomanip>
#include <climits>
#include <cmath>

// ============================================================================
// Process
// ============================================================================
struct Process {
    std::string id;
    int arrival_time;
    int burst_time;
    int priority;  // Lower = higher priority
    int remaining_time;
    int waiting_time = 0;
    int turnaround_time = 0;
    int deadline = 0;         // For EDF (0 in the file = auto: arrival + factor * burst)
    // ---- added fields ----
    int completion_time = 0;
    int start_time = -1;      // first time on the CPU (for response time)
    int io_every = 0;         // I/O: block after this many CPU units (0 = CPU-bound)
    int io_duration = 0;      // I/O: length of each block
    int io_time = 0;          // total time spent blocked on I/O
    int io_counter = 0;       // CPU units since the last I/O
    bool in_io = false;       // currently blocked on I/O
    int blocked_until = 0;    // when the current I/O finishes
    int ready_since = 0;      // last time it became ready (arrival or I/O done) - for aging
    // scheduler-specific bookkeeping
    int level = 0, queued_at = 0;          // MLFQ
    int tickets = 0, bonus = 0;            // Lottery
    double vruntime = 0, weight = 1024;    // CFS
};

using Gantt = std::vector<std::pair<std::string, int>>;

// ============================================================================
// Shared helpers
// ============================================================================
// Append a slice, merging with the previous one if the same task is still running.
void addToGantt(Gantt& gantt, const std::string& id, int duration) {
    if (duration <= 0) return;
    if (!gantt.empty() && gantt.back().first == id) gantt.back().second += duration;
    else gantt.emplace_back(id, duration);
}

// Record completion. Waiting time excludes time the task spent blocked on I/O.
void finishProcess(Process& p, int t) {
    p.remaining_time = 0;
    p.completion_time = t;
    p.turnaround_time = t - p.arrival_time;
    p.waiting_time = p.turnaround_time - p.burst_time - p.io_time;
}

// ---- I/O simulation (works with every scheduler) ----
// A task with io_every = k blocks for io_duration units after every k units of
// CPU. While blocked it is not ready, so the scheduler must pick someone else.

// Ready = arrived, unfinished, and not blocked on I/O.
bool isReady(const Process& p, int t) {
    return p.arrival_time <= t && p.remaining_time > 0 && !p.in_io;
}

// How long the task can run before its next I/O request.
int cpuUntilIO(const Process& p) {
    return p.io_every > 0 ? p.io_every - p.io_counter : INT_MAX;
}

// Earliest future time a task arrives or returns from I/O (for idle jumps).
int nextReadyTime(const std::vector<Process>& ps, int t) {
    int best = INT_MAX;
    for (auto& p : ps) {
        if (p.remaining_time == 0) continue;
        int ready_at = p.in_io ? p.blocked_until : p.arrival_time;
        if (ready_at > t) best = std::min(best, ready_at);
    }
    return best;
}

// Tasks whose I/O finished by time t (ordered by completion time). Marks them ready.
std::vector<Process*> collectWakeups(std::vector<Process>& ps, int t) {
    std::vector<Process*> woken;
    for (auto& p : ps)
        if (p.in_io && p.blocked_until <= t) { p.in_io = false; p.ready_since = p.blocked_until; woken.push_back(&p); }
    std::stable_sort(woken.begin(), woken.end(), [](Process* a, Process* b) { return a->blocked_until < b->blocked_until; });
    return woken;
}

enum RunResult { RUNNABLE, FINISHED, BLOCKED };

// Bookkeeping after `p` ran `run` units ending at time t.
RunResult afterRun(Process& p, int run, int t, int& io_requests) {
    p.remaining_time -= run;
    p.io_counter += run;
    if (p.remaining_time == 0) { finishProcess(p, t); return FINISHED; }
    if (p.io_every > 0 && p.io_counter >= p.io_every) {
        p.io_counter = 0;
        p.io_time += p.io_duration;
        p.in_io = true;
        p.blocked_until = t + p.io_duration;
        io_requests++;
        return BLOCKED;
    }
    return RUNNABLE;
}

// Idle the CPU until the next task arrives or finishes I/O.
void idleUntilNext(std::vector<Process>& ps, Gantt& gantt, int& t) {
    int nt = nextReadyTime(ps, t);
    addToGantt(gantt, "IDLE", nt - t);
    t = nt;
}

// ============================================================================
// Abstract base class for schedulers
// ============================================================================
class Scheduler {
public:
    virtual ~Scheduler() = default;
    virtual void schedule(std::vector<Process>& processes, Gantt& gantt, int& total_time) = 0;
    virtual std::string name() const = 0;
    // Extra output from the scheduler's enhancement (aging decisions, deadline misses, ...)
    std::string notes() const { return notes_.str() + ioNote(); }

protected:
    std::ostringstream notes_;
    int io_requests = 0;
    std::string ioNote() const {
        return io_requests ? "I/O requests: " + std::to_string(io_requests) + " (task blocked, CPU given to another task)\n" : "";
    }
};

// ----------------------------------------------------------------------------
// Ready-queue engine shared by FCFS (quantum = infinity) and Round Robin.
// ----------------------------------------------------------------------------
int runReadyQueue(std::vector<Process>& ps, Gantt& gantt, int quantum, int& io_requests) {
    std::deque<Process*> ready;
    size_t next = 0;
    int t = 0, done = 0, n = ps.size(), slice = 0;
    Process* cur = nullptr;

    while (done < n) {
        // arrivals first, then I/O completions, then the preempted task (RR convention)
        while (next < ps.size() && ps[next].arrival_time <= t) ready.push_back(&ps[next++]);
        for (Process* p : collectWakeups(ps, t)) ready.push_back(p);
        if (cur && slice >= quantum) { ready.push_back(cur); cur = nullptr; }
        if (!cur && !ready.empty()) { cur = ready.front(); ready.pop_front(); slice = 0; }
        if (!cur) { idleUntilNext(ps, gantt, t); continue; }

        addToGantt(gantt, cur->id, 1);
        t++; slice++;
        RunResult r = afterRun(*cur, 1, t, io_requests);
        if (r == FINISHED) done++;
        if (r != RUNNABLE) cur = nullptr;
    }
    return t;
}

// ============================================================================
// 1. FCFS
// ============================================================================
class FCFSScheduler : public Scheduler {
public:
    std::string name() const override { return "FCFS"; }
    void schedule(std::vector<Process>& processes, Gantt& gantt, int& total_time) override {
        total_time = runReadyQueue(processes, gantt, INT_MAX, io_requests);
    }
};

// ============================================================================
// 2. SJF (non-preemptive). Picks the shortest NEXT CPU burst (= whole burst
//    when the task does no I/O).
// ============================================================================
class SJFScheduler : public Scheduler {
public:
    std::string name() const override { return "SJF"; }
    void schedule(std::vector<Process>& processes, Gantt& gantt, int& total_time) override {
        int t = 0, done = 0, n = processes.size();
        auto nextBurst = [](const Process& p) { return std::min(p.remaining_time, cpuUntilIO(p)); };
        while (done < n) {
            collectWakeups(processes, t);
            Process* best = nullptr;
            for (auto& p : processes) {
                if (!isReady(p, t)) continue;
                if (!best || nextBurst(p) < nextBurst(*best) ||
                    (nextBurst(p) == nextBurst(*best) && p.arrival_time < best->arrival_time))
                    best = &p;
            }
            if (!best) { idleUntilNext(processes, gantt, t); continue; }
            int run = nextBurst(*best);                  // runs without preemption
            addToGantt(gantt, best->id, run);
            t += run;
            if (afterRun(*best, run, t, io_requests) == FINISHED) done++;
        }
        total_time = t;
    }
};

// ============================================================================
// 3. SRTF (preemptive SJF)
// ============================================================================
class SRTFScheduler : public Scheduler {
public:
    std::string name() const override { return "SRTF"; }
    void schedule(std::vector<Process>& processes, Gantt& gantt, int& total_time) override {
        int t = 0, done = 0, n = processes.size(), preemptions = 0;
        Process* cur = nullptr;
        while (done < n) {
            collectWakeups(processes, t);
            Process* best = nullptr;
            for (auto& p : processes) {
                if (!isReady(p, t)) continue;
                if (!best || p.remaining_time < best->remaining_time ||
                    (p.remaining_time == best->remaining_time && p.arrival_time < best->arrival_time))
                    best = &p;
            }
            if (!best) { idleUntilNext(processes, gantt, t); continue; }
            if (cur && cur != best && isReady(*cur, t)) preemptions++;
            cur = best;
            addToGantt(gantt, cur->id, 1);
            t++;
            RunResult r = afterRun(*cur, 1, t, io_requests);
            if (r == FINISHED) done++;
            if (r != RUNNABLE) cur = nullptr;
        }
        notes_ << "Preemptions: " << preemptions << "\n";
        total_time = t;
    }
};

// ============================================================================
// 4. Priority (non-preemptive) + aging
// ============================================================================
class PriorityScheduler : public Scheduler {
    int aging_interval;   // 0 = aging off
    int effective(const Process& p, int t) const {
        if (aging_interval <= 0) return p.priority;
        return std::max(0, p.priority - (t - p.ready_since) / aging_interval);
    }
public:
    explicit PriorityScheduler(int aging) : aging_interval(aging) {}
    std::string name() const override { return "Priority"; }
    void schedule(std::vector<Process>& processes, Gantt& gantt, int& total_time) override {
        int t = 0, done = 0, n = processes.size(), changed = 0;
        std::ostringstream log;
        while (done < n) {
            collectWakeups(processes, t);
            Process* best = nullptr;      // with aging
            Process* no_aging = nullptr;  // what plain priority would have picked
            for (auto& p : processes) {
                if (!isReady(p, t)) continue;
                int e = effective(p, t);
                if (!best || e < effective(*best, t) || (e == effective(*best, t) && p.ready_since < best->ready_since))
                    best = &p;
                if (!no_aging || p.priority < no_aging->priority ||
                    (p.priority == no_aging->priority && p.ready_since < no_aging->ready_since))
                    no_aging = &p;
            }
            if (!best) { idleUntilNext(processes, gantt, t); continue; }
            if (best != no_aging) {
                changed++;
                log << "  t=" << t << ": aging picked " << best->id << " (priority " << best->priority
                    << " aged to " << effective(*best, t) << ") over " << no_aging->id << " (priority "
                    << no_aging->priority << ")\n";
            }
            int run = std::min(best->remaining_time, cpuUntilIO(*best));
            addToGantt(gantt, best->id, run);
            t += run;
            if (afterRun(*best, run, t, io_requests) == FINISHED) done++;
        }
        if (aging_interval > 0)
            notes_ << "Aging: +1 priority level per " << aging_interval << " time units waited; changed "
                   << changed << " decision(s)\n" << log.str();
        else notes_ << "Aging: off\n";
        total_time = t;
    }
};

// ============================================================================
// 5. Round Robin
// ============================================================================
class RoundRobinScheduler : public Scheduler {
private:
    int quantum;
public:
    RoundRobinScheduler(int q) : quantum(q) {}
    std::string name() const override { return "RR(q=" + std::to_string(quantum) + ")"; }
    void schedule(std::vector<Process>& processes, Gantt& gantt, int& total_time) override {
        total_time = runReadyQueue(processes, gantt, quantum, io_requests);
    }
};

// ============================================================================
// 6. Multilevel Queue: priority < 3 -> high queue (RR), else low queue (FCFS)
// ============================================================================
class MLQScheduler : public Scheduler {
    int quantum;
public:
    explicit MLQScheduler(int q) : quantum(q) {}
    std::string name() const override { return "MLQ"; }
    void schedule(std::vector<Process>& processes, Gantt& gantt, int& total_time) override {
        std::deque<Process*> high, low;
        auto enqueue = [&](Process* p) { (p->priority < 3 ? high : low).push_back(p); };
        size_t next = 0;
        int t = 0, done = 0, n = processes.size(), slice = 0, preempted = 0;
        Process* cur = nullptr;
        bool cur_high = false;
        while (done < n) {
            while (next < processes.size() && processes[next].arrival_time <= t) enqueue(&processes[next++]);
            for (Process* p : collectWakeups(processes, t)) enqueue(p);
            if (cur && cur_high && slice >= quantum) { high.push_back(cur); cur = nullptr; }
            if (cur && !cur_high && !high.empty()) {     // high queue preempts low queue
                low.push_front(cur);
                cur = nullptr;
                preempted++;
            }
            if (!cur) {
                if (!high.empty()) { cur = high.front(); high.pop_front(); cur_high = true; }
                else if (!low.empty()) { cur = low.front(); low.pop_front(); cur_high = false; }
                slice = 0;
            }
            if (!cur) { idleUntilNext(processes, gantt, t); continue; }
            addToGantt(gantt, cur->id, 1);
            t++; slice++;
            RunResult r = afterRun(*cur, 1, t, io_requests);
            if (r == FINISHED) done++;
            if (r != RUNNABLE) cur = nullptr;
        }
        notes_ << "Queues: high = priority 1-2 (RR q=" << quantum << "), low = priority 3+ (FCFS)\n";
        notes_ << "Low-queue tasks preempted by high-queue arrivals: " << preempted << "\n";
        total_time = t;
    }
};

// ============================================================================
// 7. Multilevel Feedback Queue (3 levels, quanta 2/4/8) + promotion
//    A task that blocks for I/O before using its quantum keeps its level, so
//    I/O-bound tasks stay in the high-priority queue.
// ============================================================================
class MLFQScheduler : public Scheduler {
    static constexpr int LEVELS = 3;
    const int quanta[LEVELS] = {2, 4, 8};
    int age_limit;
public:
    explicit MLFQScheduler(int age) : age_limit(age) {}
    std::string name() const override { return "MLFQ"; }
    void schedule(std::vector<Process>& processes, Gantt& gantt, int& total_time) override {
        std::vector<std::deque<Process*>> q(LEVELS);
        size_t next = 0;
        int t = 0, done = 0, n = processes.size(), slice = 0;
        int demotions = 0, promotions = 0;
        Process* cur = nullptr;
        auto enqueue = [&](Process* p, int level) { p->level = level; p->queued_at = t; q[level].push_back(p); };

        while (done < n) {
            while (next < processes.size() && processes[next].arrival_time <= t) enqueue(&processes[next++], 0);
            for (Process* p : collectWakeups(processes, t)) enqueue(p, p->level);   // back from I/O: same level
            if (cur && slice >= quanta[cur->level]) {                    // used full quantum -> demote
                int nl = std::min(cur->level + 1, LEVELS - 1);
                if (nl != cur->level) demotions++;
                enqueue(cur, nl);
                cur = nullptr;
            }
            for (int l = 1; l < LEVELS && age_limit > 0; ++l)            // promote long waiters
                for (auto it = q[l].begin(); it != q[l].end();) {
                    if (t - (*it)->queued_at >= age_limit) {
                        Process* p = *it;
                        it = q[l].erase(it);
                        enqueue(p, l - 1);
                        promotions++;
                    } else ++it;
                }
            if (cur)                                                     // higher level waiting -> preempt
                for (int l = 0; l < cur->level; ++l)
                    if (!q[l].empty()) { cur->queued_at = t; q[cur->level].push_front(cur); cur = nullptr; break; }
            if (!cur)
                for (int l = 0; l < LEVELS; ++l)
                    if (!q[l].empty()) { cur = q[l].front(); q[l].pop_front(); slice = 0; break; }
            if (!cur) { idleUntilNext(processes, gantt, t); continue; }
            addToGantt(gantt, cur->id, 1);
            t++; slice++;
            RunResult r = afterRun(*cur, 1, t, io_requests);
            if (r == FINISHED) done++;
            if (r != RUNNABLE) cur = nullptr;                            // blocked early: no demotion
        }
        notes_ << "Quanta: Q0=2, Q1=4, Q2=8 | demotions: " << demotions << ", promotions (waited >= "
               << age_limit << "): " << promotions << "\nFinal level: ";
        for (auto& p : processes) notes_ << p.id << "=Q" << p.level << " ";
        notes_ << "\n";
        total_time = t;
    }
};

// ============================================================================
// 8. Lottery (tickets = 10 / priority) + compensation tickets
// ============================================================================
class LotteryScheduler : public Scheduler {
    int quantum;
    unsigned seed;
public:
    LotteryScheduler(int q, unsigned s) : quantum(q), seed(s) {}
    std::string name() const override { return "Lottery"; }
    void schedule(std::vector<Process>& processes, Gantt& gantt, int& total_time) override {
        std::mt19937 gen(seed);
        for (auto& p : processes) { p.tickets = std::max(1, 10 / std::max(1, p.priority)); p.bonus = 0; }
        int t = 0, done = 0, n = processes.size(), draws = 0;
        while (done < n) {
            collectWakeups(processes, t);
            std::vector<Process*> ready;
            int total = 0;
            for (auto& p : processes)
                if (isReady(p, t)) { ready.push_back(&p); total += p.tickets + p.bonus; }
            if (ready.empty()) { idleUntilNext(processes, gantt, t); continue; }

            int ticket = std::uniform_int_distribution<int>(1, total)(gen);
            draws++;
            Process* winner = ready.back();
            int sum = 0;
            for (Process* p : ready) {
                sum += p->tickets + p->bonus;
                if (ticket <= sum) { winner = p; break; }
            }
            for (Process* p : ready) p->bonus = (p == winner) ? 0 : p->bonus + 1;   // compensation

            int run = std::min({quantum, winner->remaining_time, cpuUntilIO(*winner)});
            addToGantt(gantt, winner->id, run);
            t += run;
            if (afterRun(*winner, run, t, io_requests) == FINISHED) done++;
        }
        notes_ << "Seed " << seed << ", quantum " << quantum << ", " << draws << " draws. Base tickets: ";
        for (auto& p : processes) notes_ << p.id << "=" << p.tickets << " ";
        notes_ << "\n";
        total_time = t;
    }
};

// ============================================================================
// 9. CFS (simplified): min-heap on vruntime, weights from priority
// ============================================================================
struct CFSCompare {
    bool operator()(const Process* a, const Process* b) const {
        if (a->vruntime != b->vruntime) return a->vruntime > b->vruntime;  // Min-heap on vruntime
        if (a->arrival_time != b->arrival_time) return a->arrival_time > b->arrival_time;
        return a->id > b->id;
    }
};

class CFSScheduler : public Scheduler {
    int target_latency;
public:
    explicit CFSScheduler(int latency) : target_latency(latency) {}
    std::string name() const override { return "CFS"; }
    void schedule(std::vector<Process>& processes, Gantt& gantt, int& total_time) override {
        for (auto& p : processes) { p.weight = 1024.0 / std::pow(1.25, p.priority - 1); p.vruntime = 0; }
        std::priority_queue<Process*, std::vector<Process*>, CFSCompare> rq;
        size_t next = 0;
        int t = 0, done = 0, n = processes.size();
        double min_vruntime = 0, total_weight = 0;
        auto admit = [&]() {
            while (next < processes.size() && processes[next].arrival_time <= t) {
                Process* p = &processes[next++];
                p->vruntime = min_vruntime;                 // newcomers start level with everyone
                total_weight += p->weight;
                rq.push(p);
            }
            for (Process* p : collectWakeups(processes, t)) {
                p->vruntime = std::max(p->vruntime, min_vruntime);   // sleepers can't bank credit
                total_weight += p->weight;
                rq.push(p);
            }
        };
        while (done < n) {
            admit();
            if (rq.empty()) { idleUntilNext(processes, gantt, t); continue; }
            Process* p = rq.top();
            rq.pop();
            int slice = std::max(1, (int)std::lround(target_latency * p->weight / total_weight));
            int run = std::min({slice, p->remaining_time, cpuUntilIO(*p)});
            addToGantt(gantt, p->id, run);
            t += run;
            p->vruntime += run * (1024.0 / p->weight);     // vruntime += slice / weight
            RunResult r = afterRun(*p, run, t, io_requests);
            if (r == RUNNABLE) rq.push(p);
            else { total_weight -= p->weight; if (r == FINISHED) done++; }
            min_vruntime = std::max(min_vruntime, rq.empty() ? p->vruntime : rq.top()->vruntime);
        }
        notes_ << std::fixed << std::setprecision(1) << "Target latency " << target_latency
               << ". Weight / final vruntime: ";
        for (auto& p : processes) notes_ << p.id << "=" << p.weight << "/" << p.vruntime << " ";
        notes_ << "\n";
        total_time = t;
    }
};

// ============================================================================
// 10. EDF (preemptive) + schedulability check + miss report
// ============================================================================
class EDFScheduler : public Scheduler {
public:
    std::string name() const override { return "EDF"; }
    void schedule(std::vector<Process>& processes, Gantt& gantt, int& total_time) override {
        // Processor-demand test: in every window [a, d], work that arrives and is
        // due inside the window must fit in d - a units. (I/O time is ignored.)
        int worst = 0, wa = 0, wd = 0, wdem = 0;
        for (auto& s : processes)
            for (auto& e : processes) {
                int a = s.arrival_time, d = e.deadline, demand = 0;
                if (d <= a) continue;
                for (auto& p : processes)
                    if (p.arrival_time >= a && p.deadline <= d) demand += p.burst_time;
                if (demand - (d - a) > worst) { worst = demand - (d - a); wa = a; wd = d; wdem = demand; }
            }
        if (worst == 0) notes_ << "Schedulability: FEASIBLE (every window has enough CPU time)\n";
        else notes_ << "Schedulability: NOT FEASIBLE - window [" << wa << "," << wd << "] needs " << wdem
                    << " units but has " << wd - wa << " -> some deadline must be missed\n";

        int t = 0, done = 0, n = processes.size(), preemptions = 0;
        Process* cur = nullptr;
        while (done < n) {
            collectWakeups(processes, t);
            Process* best = nullptr;
            for (auto& p : processes) {
                if (!isReady(p, t)) continue;
                if (!best || p.deadline < best->deadline ||
                    (p.deadline == best->deadline && p.arrival_time < best->arrival_time))
                    best = &p;
            }
            if (!best) { idleUntilNext(processes, gantt, t); continue; }
            if (cur && cur != best && isReady(*cur, t)) preemptions++;
            cur = best;
            addToGantt(gantt, cur->id, 1);
            t++;
            RunResult r = afterRun(*cur, 1, t, io_requests);
            if (r == FINISHED) done++;
            if (r != RUNNABLE) cur = nullptr;
        }
        int misses = 0;
        std::ostringstream list;
        for (auto& p : processes)
            if (p.completion_time > p.deadline) {
                misses++;
                list << "  " << p.id << " due " << p.deadline << ", finished " << p.completion_time
                     << " (late by " << p.completion_time - p.deadline << ")\n";
            }
        notes_ << "Preemptions: " << preemptions << " | Deadline misses: " << misses << "/" << n << "\n" << list.str();
        total_time = t;
    }
};

// ============================================================================
// Metrics and output
// ============================================================================
struct Metrics {
    double avg_wait = 0, avg_turn = 0, avg_resp = 0, cpu_util = 0, throughput = 0;
    int context_switches = 0, deadline_misses = 0, total_time = 0;
    double io_bound_tat = -1, cpu_bound_tat = -1;   // only when the input mixes both kinds
};

void calculateMetrics(const std::vector<Process>& processes, int total_time, double& avg_wait, double& avg_turn,
                      double& cpu_util, double& throughput) {
    avg_wait = 0;
    avg_turn = 0;
    for (const auto& p : processes) {
        avg_wait += p.waiting_time;
        avg_turn += p.turnaround_time;
    }
    int n = processes.size();
    avg_wait /= n;
    avg_turn /= n;
    cpu_util = (total_time > 0) ? (double)std::accumulate(processes.begin(), processes.end(), 0, [](int sum, const Process& p){ return sum + p.burst_time; }) / total_time * 100 : 0;
    throughput = (total_time > 0) ? (double)n / total_time : 0;
}

// Fill start_time from the Gantt chart (first slice of each task).
void computeStartTimes(std::vector<Process>& ps, const Gantt& gantt) {
    std::map<std::string, int> first;
    int t = 0;
    for (auto& [id, dur] : gantt) {
        if (id != "IDLE" && !first.count(id)) first[id] = t;
        t += dur;
    }
    for (auto& p : ps) p.start_time = first.count(p.id) ? first[p.id] : -1;
}

Metrics computeMetrics(std::vector<Process>& ps, const Gantt& gantt, int total_time) {
    Metrics m;
    m.total_time = total_time;
    calculateMetrics(ps, total_time, m.avg_wait, m.avg_turn, m.cpu_util, m.throughput);
    computeStartTimes(ps, gantt);
    for (auto& p : ps) {
        m.avg_resp += p.start_time - p.arrival_time;
        if (p.completion_time > p.deadline) m.deadline_misses++;
    }
    m.avg_resp /= ps.size();
    double io_sum = 0, cpu_sum = 0;
    int io_n = 0, cpu_n = 0;
    for (auto& p : ps) {
        if (p.io_every > 0) { io_sum += p.turnaround_time; io_n++; }
        else { cpu_sum += p.turnaround_time; cpu_n++; }
    }
    if (io_n && cpu_n) { m.io_bound_tat = io_sum / io_n; m.cpu_bound_tat = cpu_sum / cpu_n; }
    int busy_segments = 0;
    for (auto& g : gantt) if (g.first != "IDLE") busy_segments++;
    m.context_switches = std::max(0, busy_segments - 1);
    return m;
}

void printGantt(std::ostream& out, const Gantt& gantt) {
    std::string line = "Gantt Chart: ";
    for (const auto& entry : gantt) {
        std::string item = entry.first + "(" + std::to_string(entry.second) + ") ";
        if (line.size() + item.size() > 100) { out << line << "\n"; line = "             "; }
        line += item;
    }
    out << line << "\n";
}

// Box-style timeline, wrapped into rows of at most ~100 characters.
void printTimeline(std::ostream& out, const Gantt& gantt, int total_time) {
    int scale = total_time <= 45 ? 2 : 1;
    std::string border = "+", middle = "|", times;
    int t = 0;
    times = "0";
    auto flush = [&]() {
        out << border << "\n" << middle << "\n" << border << "\n" << times << "\n";
    };
    for (auto& [id, dur] : gantt) {
        std::string label = id == "IDLE" ? "--" : id;
        int width = std::max((int)label.size() + 2, dur * scale);
        if (border.size() + width > 100) {              // start a new row
            flush();
            border = "+"; middle = "|"; times = std::to_string(t);
        }
        int left = (width - (int)label.size()) / 2;
        border += std::string(width, '-') + "+";
        middle += std::string(left, ' ') + label + std::string(width - left - label.size(), ' ') + "|";
        t += dur;
        int col = (int)border.size() - 1;
        times += std::string(std::max(1, col - (int)times.size()), ' ') + std::to_string(t);
    }
    flush();
}

void printResults(std::ostream& out, const Scheduler& s, std::vector<Process> processes, int total_time,
                  const Gantt& gantt, bool show_deadlines, Metrics& m) {
    m = computeMetrics(processes, gantt, total_time);
    bool has_io = std::any_of(processes.begin(), processes.end(), [](const Process& p) { return p.io_every > 0; });

    out << "\n=============== " << s.name() << " ===============\n";
    std::sort(processes.begin(), processes.end(), [](const Process& a, const Process& b) {
        return a.arrival_time != b.arrival_time ? a.arrival_time < b.arrival_time : a.id < b.id;
    });
    out << std::left << std::setw(7) << "ID" << std::right << std::setw(7) << "Arr" << std::setw(7) << "Burst"
        << std::setw(5) << "Pri";
    if (show_deadlines) out << std::setw(6) << "Due";
    if (has_io) out << std::setw(8) << "I/O";
    out << std::setw(7) << "Start" << std::setw(8) << "Finish" << std::setw(7) << "Wait" << std::setw(6) << "TAT"
        << std::setw(6) << "Resp" << "\n";
    out << std::string(60 + (show_deadlines ? 6 : 0) + (has_io ? 8 : 0), '-') << "\n";
    for (auto& p : processes) {
        out << std::left << std::setw(7) << p.id << std::right << std::setw(7) << p.arrival_time << std::setw(7)
            << p.burst_time << std::setw(5) << p.priority;
        if (show_deadlines) out << std::setw(6) << p.deadline;
        if (has_io) {
            std::string io = p.io_every ? std::to_string(p.io_every) + "/" + std::to_string(p.io_duration) : "-";
            out << std::setw(8) << io;
        }
        out << std::setw(7) << p.start_time << std::setw(8) << p.completion_time << std::setw(7) << p.waiting_time
            << std::setw(6) << p.turnaround_time << std::setw(6) << p.start_time - p.arrival_time;
        if (show_deadlines && p.completion_time > p.deadline) out << "  MISSED";
        out << "\n";
    }
    out << "\n";
    printGantt(out, gantt);
    printTimeline(out, gantt, total_time);
    out << "\n" << std::fixed << std::setprecision(2);
    out << "Average Waiting Time: " << m.avg_wait << "\n";
    out << "Average Turnaround Time: " << m.avg_turn << "\n";
    out << "Average Response Time: " << m.avg_resp << "\n";
    out << "CPU Utilization: " << m.cpu_util << "%\n";
    out << std::setprecision(4) << "Throughput: " << m.throughput << " processes/unit time\n";
    out << "Context switches: " << m.context_switches << "\n";
    if (m.io_bound_tat >= 0)
        out << std::setprecision(2) << "Avg turnaround: I/O-bound tasks " << m.io_bound_tat << ", CPU-bound tasks "
            << m.cpu_bound_tat << "\n";
    std::string notes = s.notes();
    if (!notes.empty()) out << "-- " << s.name() << " details --\n" << notes;
}

// ============================================================================
// Input
// ============================================================================
// Format (one task per line, '#' starts a comment):
//   ID arrival burst priority [deadline [io_every io_duration]]
// deadline 0 = auto (arrival + factor * burst).
std::vector<Process> loadProcesses(const std::string& filename) {
    std::vector<Process> processes;
    std::ifstream file(filename);
    if (!file) {
        std::cerr << "Error opening file: " << filename << "\n";
        return processes;
    }
    std::string line;
    int line_no = 0;
    std::map<std::string, int> seen;
    while (std::getline(file, line)) {
        line_no++;
        line = line.substr(0, line.find('#'));
        std::istringstream in(line);
        std::string id;
        if (!(in >> id)) continue;                         // blank / comment line
        int at, bt, pri, dl = 0, io_every = 0, io_dur = 0;
        auto fail = [&](const std::string& why) {
            std::cerr << filename << ":" << line_no << ": " << why << "\n  > " << line << "\n";
            processes.clear();
        };
        if (!(in >> at >> bt >> pri)) { fail("expected: ID arrival burst priority [deadline [io_every io_duration]]"); return processes; }
        if (in >> dl) {
            if (in >> io_every) {
                if (!(in >> io_dur)) { fail("io_every given without io_duration"); return processes; }
            }
        }
        std::string extra;
        if (in >> extra) { fail("unexpected extra value '" + extra + "'"); return processes; }
        if (at < 0) { fail("arrival time must be >= 0"); return processes; }
        if (bt <= 0) { fail("burst time must be > 0"); return processes; }
        if (pri < 1) { fail("priority must be >= 1"); return processes; }
        if (dl < 0 || io_every < 0 || io_dur < 0) { fail("deadline / I/O values must be >= 0"); return processes; }
        if (seen.count(id)) { fail("duplicate task id '" + id + "' (first on line " + std::to_string(seen[id]) + ")"); return processes; }
        seen[id] = line_no;
        Process p{id, at, bt, pri, bt};
        p.deadline = dl;
        p.io_every = io_every;
        p.io_duration = io_dur;
        processes.push_back(p);
    }
    // Sort by arrival if needed
    std::stable_sort(processes.begin(), processes.end(), [](const Process& a, const Process& b) {
        return a.arrival_time < b.arrival_time;
    });
    return processes;
}

// workload: mixed (assignment ranges), cpu, io, web
std::vector<Process> generateRandomProcesses(int num, unsigned seed, const std::string& workload, bool add_io) {
    std::vector<Process> processes;
    std::mt19937 gen(seed);
    auto rnd = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(gen); };
    double clock = 0;
    std::exponential_distribution<double> gap(0.3);   // web: ~0.3 requests per time unit (random gaps)
    for (int i = 0; i < num; ++i) {
        std::string num_str = (i + 1 < 10 ? "0" : "") + std::to_string(i + 1);
        Process p{"P" + num_str, 0, 1, 1, 1};
        if (workload == "web") {
            // Web server: mostly short API calls, some page renders, rare heavy reports.
            int kind = rnd(1, 100);
            p.arrival_time = (int)clock;
            clock += gap(gen);
            if (kind <= 70)      { p.id = "A" + num_str; p.burst_time = rnd(1, 3);   p.priority = 1; p.deadline = p.arrival_time + 6; }
            else if (kind <= 92) { p.id = "W" + num_str; p.burst_time = rnd(3, 6);   p.priority = 2; p.deadline = p.arrival_time + 15; }
            else                 { p.id = "B" + num_str; p.burst_time = rnd(10, 20); p.priority = 4; p.deadline = p.arrival_time + 80; }
            if (p.id[0] != 'A') { p.io_every = rnd(2, 3); p.io_duration = rnd(2, 4); }  // database / disk I/O
        } else if (workload == "cpu") {
            p.arrival_time = rnd(0, 19); p.burst_time = rnd(8, 20); p.priority = rnd(1, 5);
        } else if (workload == "io") {
            p.arrival_time = rnd(0, 19); p.burst_time = rnd(4, 10); p.priority = rnd(1, 5);
            p.io_every = rnd(1, 2); p.io_duration = rnd(3, 8);
        } else {   // mixed: same ranges as the starter code
            p.arrival_time = rnd(0, 19); p.burst_time = rnd(1, 10); p.priority = rnd(1, 5);
            if (add_io && rnd(0, 1)) { p.io_every = rnd(1, 3); p.io_duration = rnd(2, 6); }
        }
        p.remaining_time = p.burst_time;
        processes.push_back(p);
    }
    std::stable_sort(processes.begin(), processes.end(), [](const Process& a, const Process& b) {
        return a.arrival_time < b.arrival_time;
    });
    return processes;
}

// ============================================================================
// Command line
// ============================================================================
void printUsage() {
    std::cout <<
R"(Usage: ./simulator --scheduler NAME[,NAME...] [options]

Schedulers: fcfs sjf srtf priority rr mlq mlfq lottery cfs edf   (or "all")

Options:
  --quantum N          time quantum for rr / mlq / lottery (default 4)
  --input FILE         read tasks from FILE: ID arrival burst priority [deadline [io_every io_duration]]
  --random             generate random tasks instead of reading a file
  --num N              number of random tasks (default 10)
  --workload W         random workload: mixed (default), cpu, io, web
  --io                 give about half of the mixed random tasks I/O bursts
  --seed N             random seed (default: time-based; printed so runs can be repeated)
  --aging N            priority aging interval (default 3, 0 = off)
  --age-limit N        MLFQ promotion after waiting N units (default 10, 0 = off)
  --deadline-factor F  EDF auto deadline = arrival + F * burst (default 2)
  --output FILE        log file (default simulation_log.txt)
  --verbose            with several schedulers, print every full result, not just the table
  --help               show this message

Examples:
  ./simulator --scheduler rr --quantum 4
  ./simulator --scheduler rr --random
  ./simulator --scheduler srtf --random --num 20 --seed 7 --output srtf_log.txt
  ./simulator --scheduler rr,srtf --input tasks.txt
  ./simulator --scheduler all --random --workload web --num 30 --seed 3
)";
}

std::unique_ptr<Scheduler> makeScheduler(const std::string& type, int quantum, int aging, int age_limit, unsigned seed) {
    if (type == "fcfs") return std::make_unique<FCFSScheduler>();
    if (type == "sjf") return std::make_unique<SJFScheduler>();
    if (type == "srtf") return std::make_unique<SRTFScheduler>();
    if (type == "priority") return std::make_unique<PriorityScheduler>(aging);
    if (type == "rr") return std::make_unique<RoundRobinScheduler>(quantum);
    if (type == "mlq") return std::make_unique<MLQScheduler>(quantum);
    if (type == "mlfq") return std::make_unique<MLFQScheduler>(age_limit);
    if (type == "lottery") return std::make_unique<LotteryScheduler>(quantum, seed);
    if (type == "cfs") return std::make_unique<CFSScheduler>(4);
    if (type == "edf") return std::make_unique<EDFScheduler>();
    return nullptr;
}

int main(int argc, char* argv[]) {
    // FIX: the starter read arguments in pairs, so a flag without a value
    // ("--random --num 20") shifted every argument after it.
    const std::vector<std::string> value_flags = {"--scheduler", "--quantum", "--input", "--num", "--seed", "--output",
                                                  "--workload", "--aging", "--age-limit", "--deadline-factor"};
    const std::vector<std::string> bool_flags = {"--random", "--io", "--verbose", "--help"};
    std::map<std::string, std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (std::find(bool_flags.begin(), bool_flags.end(), a) != bool_flags.end()) args[a] = "1";
        else if (std::find(value_flags.begin(), value_flags.end(), a) != value_flags.end()) {
            if (i + 1 >= argc) { std::cerr << "Missing value after " << a << "\n"; return 1; }
            args[a] = argv[++i];
        } else { std::cerr << "Unknown option: " << a << "\n\n"; printUsage(); return 1; }
    }
    if (args.count("--help")) { printUsage(); return 0; }
    if (!args.count("--scheduler")) { std::cerr << "Missing --scheduler\n\n"; printUsage(); return 1; }

    auto toInt = [&](const std::string& flag, int def, int min) {
        if (!args.count(flag)) return def;
        try {
            size_t pos;
            int v = std::stoi(args[flag], &pos);
            if (pos != args[flag].size() || v < min) throw std::invalid_argument("range");
            return v;
        } catch (...) {
            std::cerr << "Invalid value for " << flag << ": '" << args[flag] << "' (need an integer >= " << min << ")\n";
            std::exit(1);
        }
    };
    int quantum = toInt("--quantum", 4, 1);
    int num = toInt("--num", 10, 1);
    int aging = toInt("--aging", 3, 0);
    int age_limit = toInt("--age-limit", 10, 0);
    int factor = toInt("--deadline-factor", 2, 1);
    // Random tasks without --seed get a time-based seed (printed, so the run can be repeated).
    // Otherwise default to 42 so lottery results are reproducible.
    bool want_random = args.count("--random") || args.count("--workload");
    unsigned seed = args.count("--seed") ? (unsigned)toInt("--seed", 0, 0)
                  : want_random ? (unsigned)(std::chrono::system_clock::now().time_since_epoch().count() % 100000)
                                : 42u;
    std::string workload = args.count("--workload") ? args["--workload"] : "mixed";
    if (workload != "mixed" && workload != "cpu" && workload != "io" && workload != "web") {
        std::cerr << "Unknown workload: " << workload << " (mixed, cpu, io, web)\n";
        return 1;
    }
    bool random = want_random;
    std::string input_file = args.count("--input") ? args["--input"] : "";
    std::string output_file = args.count("--output") ? args["--output"] : "simulation_log.txt";

    // Which schedulers?
    std::vector<std::string> types;
    {
        std::string list = args["--scheduler"];
        if (list == "all") list = "fcfs,sjf,srtf,priority,rr,mlq,mlfq,lottery,cfs,edf";
        std::stringstream ss(list);
        for (std::string s; std::getline(ss, s, ',');) {
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            if (!makeScheduler(s, 1, 0, 0, 0)) { std::cerr << "Unknown scheduler: " << s << "\n"; return 1; }
            types.push_back(s);
        }
    }

    // Load tasks
    std::vector<Process> processes;
    std::string source;
    if (random) {
        processes = generateRandomProcesses(num, seed, workload, args.count("--io") > 0);
        source = "random (" + std::to_string(num) + " tasks, workload " + workload + (args.count("--io") ? " + I/O" : "") +
                 ", seed " + std::to_string(seed) + ")";
    } else if (!input_file.empty()) {
        processes = loadProcesses(input_file);
        source = input_file;
    } else {
        // Default from assignment
        processes = {
            {"P1", 0, 8, 2, 8},
            {"P2", 1, 4, 1, 4},
            {"P3", 2, 9, 3, 9},
            {"P4", 3, 5, 4, 5}
        };
        source = "default 4-process table from the assignment";
    }
    if (processes.empty()) {
        std::cerr << "No processes loaded.\n";
        return 1;
    }
    bool explicit_deadlines = false;
    for (auto& p : processes) {
        if (p.deadline > 0) explicit_deadlines = true;
        else p.deadline = p.arrival_time + factor * p.burst_time;
        p.ready_since = p.arrival_time;
    }

    std::ostringstream out;
    out << "Task source: " << source << "\n";
    out << "Tasks: " << processes.size() << " | Schedulers: ";
    for (size_t i = 0; i < types.size(); ++i) out << (i ? ", " : "") << types[i];
    out << "\n";

    bool full = types.size() <= 2 || args.count("--verbose");
    std::vector<std::pair<std::string, Metrics>> results;
    for (auto& type : types) {
        auto scheduler = makeScheduler(type, quantum, aging, age_limit, seed);
        std::vector<Process> copy = processes;    // every scheduler gets the same, fresh input
        Gantt gantt;
        int total_time = 0;
        scheduler->schedule(copy, gantt, total_time);
        Metrics m;
        std::ostringstream detail;
        printResults(detail, *scheduler, copy, total_time, gantt, type == "edf" || explicit_deadlines, m);
        if (full) out << detail.str();
        results.emplace_back(scheduler->name(), m);
    }

    if (results.size() > 1) {
        out << "\n=============== Comparison (same " << processes.size() << " tasks) ===============\n";
        out << std::left << std::setw(10) << "Scheduler" << std::right << std::setw(9) << "AvgWait" << std::setw(9)
            << "AvgTAT" << std::setw(9) << "AvgResp" << std::setw(8) << "CPU%" << std::setw(11) << "Throughput"
            << std::setw(8) << "Switch" << std::setw(8) << "Missed" << std::setw(7) << "Time";
        bool mixed = results[0].second.io_bound_tat >= 0;
        if (mixed) out << std::setw(8) << "IO-TAT" << std::setw(9) << "CPU-TAT";
        out << "\n" << std::string(mixed ? 96 : 79, '-') << "\n" << std::fixed;
        for (auto& [name, m] : results) {
            out << std::left << std::setw(10) << name << std::right << std::setprecision(2) << std::setw(9) << m.avg_wait
                << std::setw(9) << m.avg_turn << std::setw(9) << m.avg_resp << std::setprecision(1) << std::setw(8)
                << m.cpu_util << std::setprecision(4) << std::setw(11) << m.throughput << std::setw(8)
                << m.context_switches << std::setw(8) << m.deadline_misses << std::setw(7) << m.total_time;
            if (mixed) out << std::setprecision(2) << std::setw(8) << m.io_bound_tat << std::setw(9) << m.cpu_bound_tat;
            out << "\n";
        }
        auto best = [&](auto key) {     // name(s) of the scheduler(s) with the lowest value
            auto lowest = key(std::min_element(results.begin(), results.end(), [&](auto& a, auto& b) {
                              return key(a.second) < key(b.second);
                          })->second);
            std::string names;
            for (auto& r : results)
                if (std::abs(key(r.second) - lowest) < 1e-9) names += (names.empty() ? "" : "/") + r.first;
            return names.find('/') != std::string::npos && (size_t)std::count(names.begin(), names.end(), '/') + 1 == results.size()
                       ? std::string("all tied") : names;
        };
        out << "Lowest avg wait: " << best([](const Metrics& m) { return m.avg_wait; })
            << " | lowest avg response: " << best([](const Metrics& m) { return m.avg_resp; })
            << " | fewest deadline misses: " << best([](const Metrics& m) { return m.deadline_misses; }) << "\n";
        out << "(Missed = finished after deadline; deadlines default to arrival + " << factor << " x burst)\n";
    }

    std::cout << out.str();

    // Log to file
    std::ofstream log(output_file);
    if (log) {
        log << "Scheduler: " << args["--scheduler"] << " | quantum " << quantum << "\n";
        log << out.str();
        std::cout << "\n[results written to " << output_file << "]\n";
    } else {
        std::cerr << "Warning: could not write " << output_file << "\n";
    }
    return 0;
}
