// mlfq.cpp - Exercise 7: Multilevel Feedback Queue
//   3 levels, Round Robin quanta 2 / 4 / 8. Every process starts in level 0.
//   Feedback: a process that uses its FULL quantum is demoted one level
//             (it looks CPU-bound); one that gives up the CPU early stays.
//   Enhancement: a process that waits AGE_LIMIT time units in a lower queue
//             is promoted one level (prevents starvation).
//   A higher-level arrival preempts a lower-level process.
// Compile: g++ mlfq.cpp -o mlfq     Run: ./mlfq
// ======================= Part 1: Common Code =======================
#include <iostream>
#include <iomanip>
#include <vector>
#include <queue>
#include <algorithm>
#include <string>
struct Process {
    std::string id;
    int arrival_time;
    int burst_time;
    int priority;         // Lower number = higher priority
    int remaining_time;   // For preemptive
    int waiting_time;
    int turnaround_time;
    int completion_time = 0;  // Added: time the process finished
    int level = 0;            // Added for MLFQ: current queue level
    int queued_at = 0;        // Added for MLFQ: time it entered its queue
};

// FIX: the starter computed total_time / total_time, which is always 100%.
// CPU utilization = time the CPU was busy running processes / total elapsed time.
void calculateMetrics(std::vector<Process>& processes, int total_time) {
    double avg_wait = 0, avg_turn = 0;
    int busy_time = 0;
    for (auto& p : processes) {
        avg_wait += p.waiting_time;
        avg_turn += p.turnaround_time;
        busy_time += p.burst_time;
    }
    avg_wait /= processes.size();
    avg_turn /= processes.size();
    double cpu_util = (total_time > 0) ? (double)busy_time / total_time * 100 : 0;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Avg Waiting Time: " << avg_wait << "\n";
    std::cout << "Avg Turnaround Time: " << avg_turn << "\n";
    std::cout << "CPU Utilization: " << cpu_util << "%\n";
}

// Function to print text-based Gantt chart (format from the starter code)
void printGantt(const std::vector<std::pair<std::string, int>>& gantt) {
    std::cout << "Gantt Chart: ";
    for (auto& entry : gantt) {
        std::cout << entry.first << "(" << entry.second << ") ";
    }
    std::cout << "\n";
}

// Added: draws the Gantt chart as boxes (2 characters per time unit)
// with the clock time printed under every boundary.
void printTimeline(const std::vector<std::pair<std::string, int>>& gantt) {
    std::string border = "+", middle = "|", times = "0";
    int t = 0;
    for (auto& entry : gantt) {
        const std::string& id = entry.first;
        int width = std::max((int)id.size() + 2, entry.second * 2);
        int left = (width - (int)id.size()) / 2;
        border += std::string(width, '-') + "+";
        middle += std::string(left, ' ') + id + std::string(width - left - id.size(), ' ') + "|";
        t += entry.second;
        int column = (int)border.size() - 1;  // column of the '+' we just drew
        times += std::string(std::max(1, column - (int)times.size()), ' ') + std::to_string(t);
    }
    std::cout << border << "\n" << middle << "\n" << border << "\n" << times << "\n";
}

// Added: per-process results table
void printProcessTable(std::vector<Process> processes) {
    std::sort(processes.begin(), processes.end(),
              [](const Process& a, const Process& b) { return a.id < b.id; });
    std::cout << std::left << std::setw(9) << "Process" << std::right
              << std::setw(9) << "Arrival" << std::setw(7) << "Burst" << std::setw(10) << "Priority"
              << std::setw(10) << "Finish" << std::setw(10) << "Waiting" << std::setw(12) << "Turnaround" << "\n";
    std::cout << std::string(67, '-') << "\n";
    for (auto& p : processes) {
        std::cout << std::left << std::setw(9) << p.id << std::right
                  << std::setw(9) << p.arrival_time << std::setw(7) << p.burst_time
                  << std::setw(10) << p.priority << std::setw(10) << p.completion_time
                  << std::setw(10) << p.waiting_time << std::setw(12) << p.turnaround_time << "\n";
    }
}

void printHeader(const std::string& title) {
    std::cout << "\n==================== " << title << " ====================\n";
}

// Adds a slice to the Gantt chart, merging it with the previous slice if the
// same process (or IDLE) is still on the CPU.
void addToGantt(std::vector<std::pair<std::string, int>>& gantt, const std::string& id, int duration) {
    if (duration <= 0) return;
    if (!gantt.empty() && gantt.back().first == id) gantt.back().second += duration;
    else gantt.push_back({id, duration});
}

// Called when a process finishes at time t.
void finishProcess(Process& p, int t) {
    p.remaining_time = 0;
    p.completion_time = t;
    p.turnaround_time = t - p.arrival_time;
    p.waiting_time = p.turnaround_time - p.burst_time;
}
// ===================================================================
#include <deque>

const int NUM_LEVELS = 3;
const int QUANTA[NUM_LEVELS] = {2, 4, 8};
const int AGE_LIMIT = 10;   // promote after waiting this long (0 = off)

// Runs MLFQ on `processes`. age_limit = 0 disables promotion.
// Returns total time; prints the event log.
int runMLFQ(std::vector<Process>& processes, std::vector<std::pair<std::string, int>>& gantt, int age_limit) {
    std::stable_sort(processes.begin(), processes.end(),
                     [](const Process& a, const Process& b) { return a.arrival_time < b.arrival_time; });
    std::vector<std::deque<Process*>> queues(NUM_LEVELS);
    int t = 0, completed = 0, n = processes.size();
    int demotions = 0, promotions = 0;
    size_t next = 0;
    Process* current = nullptr;
    int slice_used = 0;


    auto enqueue = [&](Process* p, int level) {
        p->level = level;
        p->queued_at = t;
        queues[level].push_back(p);
    };

    while (completed < n) {
        // 1. new arrivals enter the top queue
        while (next < processes.size() && processes[next].arrival_time <= t)
            enqueue(&processes[next++], 0);

        // 2. full quantum used -> demote (feedback)
        if (current && slice_used == QUANTA[current->level]) {
            int new_level = std::min(current->level + 1, NUM_LEVELS - 1);
            if (new_level != current->level) {
                demotions++;
                std::cout << "  t=" << t << ": " << current->id << " used full quantum -> demoted to Q"
                          << new_level << "\n";
            }
            enqueue(current, new_level);
            current = nullptr;
        }

        // 3. Enhancement: promote processes that have waited too long
        for (int l = 1; l < NUM_LEVELS && age_limit > 0; ++l) {
            for (auto it = queues[l].begin(); it != queues[l].end();) {
                Process* p = *it;
                if (t - p->queued_at >= age_limit) {
                    it = queues[l].erase(it);
                    enqueue(p, l - 1);
                    promotions++;
                    std::cout << "  t=" << t << ": " << p->id << " waited " << age_limit
                              << " in Q" << l << " -> promoted to Q" << l - 1 << "\n";
                } else ++it;
            }
        }

        // 4. preempt if a higher queue has work
        if (current) {
            for (int l = 0; l < current->level; ++l)
                if (!queues[l].empty()) {
                    std::cout << "  t=" << t << ": " << queues[l].front()->id << " (Q" << l << ") preempts "
                              << current->id << " (Q" << current->level << ")\n";
                    current->queued_at = t;
                    queues[current->level].push_front(current);
                    current = nullptr;
                    break;
                }
        }

        // 5. dispatch from the highest non-empty queue
        if (!current) {
            for (int l = 0; l < NUM_LEVELS; ++l)
                if (!queues[l].empty()) {
                    current = queues[l].front();
                    queues[l].pop_front();
                    slice_used = 0;
                    break;
                }
        }

        // 6. run one time unit
        addToGantt(gantt, current ? current->id : "IDLE", 1);
        t++;
        if (current) {
            slice_used++;
            if (--current->remaining_time == 0) {
                finishProcess(*current, t);
                std::cout << "  t=" << t << ": " << current->id << " finished in Q" << current->level << "\n";
                completed++;
                current = nullptr;
            }
        }
    }

    std::cout << "  Demotions: " << demotions << "   Promotions (aging): " << promotions << "\n";
    return t;
}

void showResults(std::vector<Process>& processes, std::vector<std::pair<std::string, int>>& gantt, int total) {
    std::cout << "\n";
    printProcessTable(processes);
    std::cout << "\n";
    printGantt(gantt);
    printTimeline(gantt);
    std::cout << "\n";
    calculateMetrics(processes, total);
}

int main() {
    std::vector<Process> processes = {
        {"P1", 0, 8, 2, 8, 0, 0},
        {"P2", 1, 4, 1, 4, 0, 0},
        {"P3", 2, 9, 3, 9, 0, 0},
        {"P4", 3, 5, 4, 5, 0, 0}
    };
    std::vector<std::pair<std::string, int>> gantt;
    printHeader("Exercise 7: MLFQ (quanta 2/4/8, promote after waiting " + std::to_string(AGE_LIMIT) + ")");
    std::cout << "Events:\n";
    int total = runMLFQ(processes, gantt, AGE_LIMIT);
    showResults(processes, gantt, total);

    // ---- Starvation demo: a CPU-bound HOG vs a stream of short interactive jobs ----
    // Each short job finishes inside the top queue's 2-unit quantum, so Q0 is
    // never empty and HOG (demoted to Q1) would wait until the stream ends.
    std::vector<Process> stream = {{"HOG", 0, 10, 3, 10, 0, 0}};
    for (int k = 1; k <= 14; ++k)
        stream.push_back({"I" + std::to_string(k), 2 * k - 1, 2, 1, 2, 0, 0});
    for (int age : {0, AGE_LIMIT}) {
        std::vector<Process> copy = stream;
        std::vector<std::pair<std::string, int>> g;
        printHeader(age ? "Starvation demo: WITH promotion" : "Starvation demo: WITHOUT promotion");
        int tt = runMLFQ(copy, g, age);
        showResults(copy, g, tt);
        // longest stretch HOG sat in a queue without getting the CPU
        int clock = 0, last_end = 0, longest = 0;
        for (auto& seg : g) {
            if (seg.first == "HOG") { longest = std::max(longest, clock - last_end); last_end = clock + seg.second; }
            clock += seg.second;
        }
        std::cout << ">> HOG's longest wait without the CPU: " << longest << " time units\n";
    }
    return 0;
}
