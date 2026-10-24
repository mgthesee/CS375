// cfs.cpp - Exercise 9: Completely Fair Scheduler (simplified)
//   The runnable process with the SMALLEST virtual runtime runs next (min-heap).
//   After running for `slice` units:  vruntime += slice * (1024 / weight)
//   Enhancement (weights from priority, like Linux nice levels): every step of
//   priority changes weight by ~1.25x, so higher-priority (lower number)
//   processes have bigger weights, their vruntime grows more slowly, and they
//   get more CPU.   weight = 1024 / 1.25^(priority - 1)
//   Time slice = TARGET_LATENCY * weight / (total weight of runnable processes),
//   at least MIN_GRANULARITY. New arrivals start at the current min vruntime so
//   they can't monopolize the CPU.
//   NOTE: the handout's "runtime / nice" would make priority-1 processes age
//   FASTEST (get the least CPU), which is backwards, so weights are used instead.
// Compile: g++ cfs.cpp -o cfs     Run: ./cfs
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
    double vruntime = 0;      // Added for CFS: virtual runtime
    double weight = 1024;     // Added for CFS: weight from priority
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
#include <cmath>

const int TARGET_LATENCY = 4;   // every runnable process should get the CPU within ~4 units
const int MIN_GRANULARITY = 1;

struct CFSCompare {
    bool operator()(const Process* a, const Process* b) {
        if (a->vruntime != b->vruntime) return a->vruntime > b->vruntime;  // Min-heap on vruntime
        if (a->arrival_time != b->arrival_time) return a->arrival_time > b->arrival_time;
        return a->id > b->id;
    }
};

int main() {
    std::vector<Process> processes = {
        {"P1", 0, 8, 2, 8, 0, 0},
        {"P2", 1, 4, 1, 4, 0, 0},
        {"P3", 2, 9, 3, 9, 0, 0},
        {"P4", 3, 5, 4, 5, 0, 0}
    };
    std::stable_sort(processes.begin(), processes.end(),
                     [](const Process& a, const Process& b) { return a.arrival_time < b.arrival_time; });
    for (auto& p : processes) {
        p.weight = 1024.0 / std::pow(1.25, p.priority - 1);
        p.vruntime = 0;
    }

    std::priority_queue<Process*, std::vector<Process*>, CFSCompare> ready_queue;
    std::vector<std::pair<std::string, int>> gantt;
    int t = 0, completed = 0, n = processes.size();
    size_t next = 0;
    double min_vruntime = 0;

    printHeader("Exercise 9: CFS (simplified)");
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Weights: ";
    for (auto& p : processes) std::cout << p.id << "=" << p.weight << "  ";
    std::cout << "\nSlices:\n";

    auto admitArrivals = [&]() {
        while (next < processes.size() && processes[next].arrival_time <= t) {
            Process* p = &processes[next++];
            p->vruntime = min_vruntime;          // start level with everyone else
            ready_queue.push(p);
        }
    };

    while (completed < n) {
        admitArrivals();
        if (ready_queue.empty()) {
            addToGantt(gantt, "IDLE", processes[next].arrival_time - t);
            t = processes[next].arrival_time;
            continue;
        }

        // Slice proportional to this process's share of the total weight
        double total_weight = 0;
        {
            auto copy = ready_queue;
            for (; !copy.empty(); copy.pop()) total_weight += copy.top()->weight;
        }
        Process* p = ready_queue.top();
        ready_queue.pop();
        int slice = std::max(MIN_GRANULARITY, (int)std::lround(TARGET_LATENCY * p->weight / total_weight));
        int run = std::min(slice, p->remaining_time);

        std::cout << "  t=" << std::setw(2) << t << ": " << p->id << " (vruntime " << std::setw(5) << p->vruntime
                  << ") runs " << run;
        addToGantt(gantt, p->id, run);
        t += run;
        p->remaining_time -= run;
        p->vruntime += run * (1024.0 / p->weight);   // vruntime += slice / weight
        std::cout << " -> vruntime " << p->vruntime << "\n";

        if (p->remaining_time > 0) ready_queue.push(p);
        else { finishProcess(*p, t); completed++; }

        // min_vruntime = smallest vruntime still runnable (never moves backwards).
        // Update it BEFORE admitting arrivals so newcomers start level with the
        // processes that have already been running, not at 0.
        if (!ready_queue.empty()) min_vruntime = std::max(min_vruntime, ready_queue.top()->vruntime);
        else min_vruntime = std::max(min_vruntime, p->vruntime);
        admitArrivals();                       // arrivals during the slice
    }

    std::cout << "\n";
    printProcessTable(processes);
    std::cout << "\n";
    printGantt(gantt);
    printTimeline(gantt);
    std::cout << "\n";
    calculateMetrics(processes, t);
    return 0;
}
