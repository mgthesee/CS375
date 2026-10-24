// mlq.cpp - Exercise 6: Multilevel Queue Scheduling
//   High queue: priority < 3, Round Robin (q = 4)
//   Low queue : priority >= 3, FCFS
// The high queue always wins: a low-queue process is preempted as soon as a
// high-queue process becomes ready. Processes never move between queues.
// Compile: g++ mlq.cpp -o mlq     Run: ./mlq
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
#include <climits>

// One entry per level. Adding a level = adding a line to `levels` in main().
struct Level {
    std::string name;
    int max_priority;          // processes with priority <= max_priority go here
    bool round_robin;          // true = RR with `quantum`, false = FCFS
    int quantum;
    std::deque<Process*> queue;
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

    std::vector<Level> levels = {
        {"High (RR q=4)", 2,       true,  4, {}},   // priority < 3
        {"Low  (FCFS)",   INT_MAX, false, 0, {}},   // everything else
    };
    auto levelOf = [&](const Process& p) {
        for (size_t i = 0; i < levels.size(); ++i)
            if (p.priority <= levels[i].max_priority) return (int)i;
        return (int)levels.size() - 1;
    };

    printHeader("Exercise 6: Multilevel Queue");
    std::cout << "Queue assignment:\n";
    for (auto& p : processes)
        std::cout << "  " << p.id << " (priority " << p.priority << ") -> " << levels[levelOf(p)].name << "\n";
    std::cout << "Events:\n";

    std::vector<std::pair<std::string, int>> gantt;
    int t = 0, completed = 0, n = processes.size();
    size_t next = 0;
    Process* current = nullptr;
    int cur_level = -1, slice_used = 0;

    while (completed < n) {
        // 1. admit arrivals into their fixed queue
        while (next < processes.size() && processes[next].arrival_time <= t) {
            Process* p = &processes[next++];
            levels[levelOf(*p)].queue.push_back(p);
        }
        // 2. RR quantum expired -> back of its queue
        if (current && levels[cur_level].round_robin && slice_used == levels[cur_level].quantum) {
            levels[cur_level].queue.push_back(current);
            current = nullptr;
        }
        // 3. a higher queue has work -> preempt (keeps its place at the FRONT of its queue)
        if (current) {
            for (int l = 0; l < cur_level; ++l)
                if (!levels[l].queue.empty()) {
                    std::cout << "  t=" << t << ": " << levels[l].queue.front()->id << " (" << levels[l].name
                              << ") preempts " << current->id << "\n";
                    levels[cur_level].queue.push_front(current);
                    current = nullptr;
                    break;
                }
        }
        // 4. dispatch from the highest non-empty queue
        if (!current) {
            for (int l = 0; l < (int)levels.size(); ++l)
                if (!levels[l].queue.empty()) {
                    current = levels[l].queue.front();
                    levels[l].queue.pop_front();
                    cur_level = l;
                    slice_used = 0;
                    break;
                }
        }
        // 5. run one time unit
        addToGantt(gantt, current ? current->id : "IDLE", 1);
        t++;
        if (current) {
            slice_used++;
            if (--current->remaining_time == 0) {
                finishProcess(*current, t);
                std::cout << "  t=" << t << ": " << current->id << " finished\n";
                completed++;
                current = nullptr;
            }
        }
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
