// edf.cpp - Exercise 10: Earliest Deadline First (preemptive)
//   deadline = arrival + 2 * burst. At every time unit the ready process with
//   the nearest deadline runs; a newly arrived process with an earlier
//   deadline preempts the running one.
//   Enhancement: schedulability check before running (processor-demand test),
//   plus deadline met/missed report afterwards.
// Compile: g++ edf.cpp -o edf     Run: ./edf
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
    int deadline = 0;         // Added for EDF: absolute deadline
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

const int DEADLINE_FACTOR = 2;

// Processor-demand test for one-shot jobs: for every window [a, d] (a = some
// arrival time, d = some deadline) the work of jobs that both arrive and are
// due inside the window must fit:  demand(a, d) <= d - a.
// EDF is optimal on one CPU, so if this fails NO scheduler can meet every deadline.
bool checkSchedulability(const std::vector<Process>& ps) {
    int worst_over = 0, wa = 0, wd = 0, wdemand = 0;
    for (auto& start : ps)
        for (auto& end : ps) {
            int a = start.arrival_time, d = end.deadline;
            if (d <= a) continue;
            int demand = 0;
            for (auto& p : ps)
                if (p.arrival_time >= a && p.deadline <= d) demand += p.burst_time;
            if (demand - (d - a) > worst_over) {
                worst_over = demand - (d - a);
                wa = a; wd = d; wdemand = demand;
            }
        }
    if (worst_over == 0) {
        std::cout << "Schedulability: FEASIBLE - every window has enough CPU time\n";
        return true;
    }
    std::cout << "Schedulability: NOT FEASIBLE - jobs in window [" << wa << ", " << wd << "] need "
              << wdemand << " units but only " << wd - wa << " are available (overload " << worst_over << ")\n";
    return false;
}

int main() {
    std::vector<Process> processes = {
        {"P1", 0, 8, 2, 8, 0, 0},
        {"P2", 1, 4, 1, 4, 0, 0},
        {"P3", 2, 9, 3, 9, 0, 0},
        {"P4", 3, 5, 4, 5, 0, 0}
    };
    for (auto& p : processes) p.deadline = p.arrival_time + p.burst_time * DEADLINE_FACTOR;

    printHeader("Exercise 10: EDF (deadline = arrival + 2 x burst)");
    std::cout << "Deadlines: ";
    for (auto& p : processes) std::cout << p.id << "=" << p.deadline << "  ";
    std::cout << "\n";
    checkSchedulability(processes);
    std::cout << "Preemption log:\n";

    // Min-heap on deadline is rebuilt each tick because the ready set changes
    auto later = [](Process* a, Process* b) {
        if (a->deadline != b->deadline) return a->deadline > b->deadline;
        return a->arrival_time > b->arrival_time;
    };

    std::vector<std::pair<std::string, int>> gantt;
    int t = 0, completed = 0, n = processes.size(), preemptions = 0;
    Process* current = nullptr;
    while (completed < n) {
        std::priority_queue<Process*, std::vector<Process*>, decltype(later)> ready(later);
        for (auto& p : processes)
            if (p.arrival_time <= t && p.remaining_time > 0) ready.push(&p);
        Process* next = ready.empty() ? nullptr : ready.top();

        if (current && next != current && current->remaining_time > 0) {
            preemptions++;
            std::cout << "  t=" << t << ": " << next->id << " (deadline " << next->deadline << ") preempts "
                      << current->id << " (deadline " << current->deadline << ")\n";
        }
        current = next;

        addToGantt(gantt, current ? current->id : "IDLE", 1);
        t++;
        if (current && --current->remaining_time == 0) {
            finishProcess(*current, t);
            completed++;
            current = nullptr;
        }
    }
    if (preemptions == 0) std::cout << "  (none)\n";

    std::cout << "\n";
    printProcessTable(processes);
    std::cout << "\n";
    printGantt(gantt);
    printTimeline(gantt);
    std::cout << "\n";
    calculateMetrics(processes, t);

    // Deadline report
    std::cout << "\nDeadline report:\n";
    int misses = 0;
    for (auto& p : processes) {
        int lateness = p.completion_time - p.deadline;
        std::cout << "  " << p.id << ": deadline " << std::setw(2) << p.deadline << ", finished " << std::setw(2)
                  << p.completion_time << "  " << (lateness <= 0 ? "MET" : "MISSED by " + std::to_string(lateness))
                  << "\n";
        if (lateness > 0) misses++;
    }
    std::cout << "Deadline misses: " << misses << "/" << n << "\n";
    return 0;
}
