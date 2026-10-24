// lottery.cpp - Exercise 8: Lottery Scheduling
//   Tickets = 10 / priority (P2 with priority 1 gets the most tickets).
//   Every QUANTUM time units a random ticket is drawn; its owner runs.
//   Enhancement (dynamic tickets / compensation): each time a ready process
//   LOSES a draw it gains 1 bonus ticket; the bonus resets when it wins.
//   This keeps low-ticket processes from waiting too long.
//   SEED is fixed so the output is reproducible (needed for screenshots);
//   use std::random_device for a different run each time.
// Compile: g++ lottery.cpp -o lottery     Run: ./lottery
// ======================= Part 1: Common Code =======================
#include <iostream>
#include <iomanip>
#include <vector>
#include <queue>
#include <algorithm>
#include <string>
#include <random>
struct Process {
    std::string id;
    int arrival_time;
    int burst_time;
    int priority;         // Lower number = higher priority
    int remaining_time;   // For preemptive
    int waiting_time;
    int turnaround_time;
    int completion_time = 0;  // Added: time the process finished
    int tickets = 0;          // Added for lottery: base tickets
    int bonus = 0;            // Added for lottery: compensation tickets
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

const int QUANTUM = 2;
const unsigned SEED = 42;

int main() {
    std::vector<Process> processes = {
        {"P1", 0, 8, 2, 8, 0, 0},
        {"P2", 1, 4, 1, 4, 0, 0},
        {"P3", 2, 9, 3, 9, 0, 0},
        {"P4", 3, 5, 4, 5, 0, 0}
    };
    std::mt19937 gen(SEED);   // was: std::random_device rd; std::mt19937 gen(rd());

    for (auto& p : processes) {
        p.tickets = std::max(1, 10 / p.priority);
        p.bonus = 0;
    }

    printHeader("Exercise 8: Lottery (quantum " + std::to_string(QUANTUM) + ", seed " + std::to_string(SEED) + ")");
    std::cout << "Base tickets: ";
    for (auto& p : processes) std::cout << p.id << "=" << p.tickets << "  ";
    std::cout << "\nDraws:\n";

    std::vector<std::pair<std::string, int>> gantt;
    int t = 0, completed = 0, n = processes.size(), draws = 0;
    while (completed < n) {
        std::vector<Process*> ready;
        for (auto& p : processes)
            if (p.arrival_time <= t && p.remaining_time > 0) ready.push_back(&p);
        if (ready.empty()) {
            int next_arrival = 1e9;
            for (auto& p : processes)
                if (p.remaining_time > 0) next_arrival = std::min(next_arrival, p.arrival_time);
            addToGantt(gantt, "IDLE", next_arrival - t);
            t = next_arrival;
            continue;
        }

        // Build the ticket pool and draw a winning ticket number
        int total = 0;
        for (auto* p : ready) total += p->tickets + p->bonus;
        std::uniform_int_distribution<int> dist(1, total);
        int ticket = dist(gen);
        draws++;

        // Walk the pool to find who owns that ticket
        Process* winner = nullptr;
        int counter = 0;
        for (auto* p : ready) {
            counter += p->tickets + p->bonus;
            if (ticket <= counter) { winner = p; break; }
        }

        std::cout << "  t=" << std::setw(2) << t << "  pool[";
        for (size_t i = 0; i < ready.size(); ++i)
            std::cout << (i ? " " : "") << ready[i]->id << ":" << ready[i]->tickets + ready[i]->bonus;
        std::cout << "] total " << std::setw(2) << total << "  ticket " << std::setw(2) << ticket
                  << " -> " << winner->id << "\n";

        // Dynamic tickets: losers gain compensation, winner's bonus resets
        for (auto* p : ready) {
            if (p == winner) p->bonus = 0;
            else p->bonus++;
        }

        int run = std::min(QUANTUM, winner->remaining_time);
        addToGantt(gantt, winner->id, run);
        t += run;
        winner->remaining_time -= run;
        if (winner->remaining_time == 0) {
            finishProcess(*winner, t);
            completed++;
        }
    }

    std::cout << "\n";
    printProcessTable(processes);
    std::cout << "\n";
    printGantt(gantt);
    printTimeline(gantt);
    std::cout << "\n";
    calculateMetrics(processes, t);
    std::cout << "Total draws: " << draws << "\n";
    return 0;
}
