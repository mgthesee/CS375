# CS375 Lab 8: CPU Scheduling Lab and Task Scheduling Simulator

This lab implements ten CPU scheduling algorithms in C++. Each one appears twice: as a standalone lab
program (Part 2) and as a class in a modular command-line simulator (Part 3).

```
lab8-scheduling/
├── part2-lab/            10 standalone programs, one per exercise
│   ├── fcfs.cpp  sjf.cpp  srtf.cpp  priority.cpp  rr.cpp
│   └── mlq.cpp   mlfq.cpp lottery.cpp cfs.cpp     edf.cpp
├── part3-simulator/
│   ├── simulator.cpp     all 10 schedulers behind one Scheduler base class
│   ├── Makefile
│   ├── tasks.txt             12 general tasks
│   ├── tasks_deadlines.txt   8 jobs with explicit deadlines
│   └── tasks_io.txt          CPU-bound and I/O-bound mix
└── screenshots/          output of every run (used in the report)
```

## Part 2: Lab exercises

Every file contains the Part 1 common code, so each file compiles on its own:

```bash
cd part2-lab
g++ fcfs.cpp -o fcfs && ./fcfs
```

Each program prints the following for the assignment's process table:
- a results table (finish, waiting and turnaround time per process)
- the starter-style Gantt line `P1(8) P2(4) ...`
- a boxed timeline
- the averages and CPU utilization

**Fixes to the starter code**
- `calculateMetrics` computed `total_time / total_time`, which always gives 100%. It now uses busy time / total time.
- `simulator.cpp` used `std::accumulate` without `#include <numeric>`.
- The simulator read command-line arguments in pairs, so `--random --num 20` shifted every argument after `--random`.
- The CFS starter had no `vruntime` field.

### Results on the assignment table (P1–P4)

| # | Scheduler | Gantt chart | Avg wait | Avg TAT | Enhancement |
|---|---|---|---|---|---|
| 1 | FCFS | P1(8) P2(4) P3(9) P4(5) | 8.75 | 15.25 | — |
| 2 | SJF | P1(8) P2(4) P4(5) P3(9) | 7.75 | 14.25 | — |
| 3 | SRTF | P1(1) P2(4) P4(5) P1(7) P3(9) | **6.50** | **13.00** | Preemption log |
| 4 | Priority | P1(8) P2(4) P3(9) P4(5) | 8.75 | 15.25 | Aging + starvation demo |
| 5 | RR q=4 | P1 P2 P3 P4 P1 P3 P4(1) P3(1) | 11.75 | 18.25 | Queue trace |
| 6 | MLQ | P1(4) P2(4) P1(4) P3(9) P4(5) | 8.75 | 15.25 | Table-driven levels (one line adds a level) |
| 7 | MLFQ 2/4/8 | 10 slices | 13.00 | 19.50 | Promotion after waiting 10 + starvation demo |
| 8 | Lottery q=2 | 12 slices (seed 42) | 13.25 | 19.75 | Compensation tickets for losers |
| 9 | CFS | 17 slices | 12.50 | 19.00 | Weights from priority (1.25× per level) |
| 10 | EDF | P1(1) P2(4) P4(5) P1(7) P3(9) | 6.50 | 13.00 | Schedulability test + miss report |

**Design notes**
- **Priority aging:** a task's effective priority is `max(0, priority − wait/3)`. On P1–P4 aging doesn't change
  the order. The program therefore also runs a starvation demo: one priority-5 task competes with a stream of
  priority-1 tasks. Its wait drops from 23 to 15 time units once aging is on.
- **MLFQ promotion:** the program includes a demo with one CPU hog and 14 short interactive jobs. Without
  promotion, the hog's longest stretch without the CPU is 28 units. With promotion it is 14 units.
- **CFS:** the handout's formula `vruntime = runtime / nice` would give priority-1 tasks the *least* CPU time.
  I used a Linux-style weight instead: `weight = 1024 / 1.25^(priority−1)`, `vruntime += run × 1024/weight`.
  Each time slice is a share of a 4-unit target latency, proportional to the task's weight. New arrivals start at
  `min_vruntime`.
- **EDF:** with `deadline = arrival + 2×burst`, the jobs in the window [0, 20] need 26 units of CPU, which is
  more than the window holds. No scheduler can meet every deadline here, and EDF misses two (P1 by 1, P3 by 6).
  The 6 is the smallest maximum lateness any schedule can achieve.

## Part 3: Simulator

```bash
cd part3-simulator
g++ -std=c++17 simulator.cpp -o simulator      # or: make
./simulator --help
```

| Option | Meaning |
|---|---|
| `--scheduler NAME` | `fcfs sjf srtf priority rr mlq mlfq lottery cfs edf`. Accepts a comma list (`rr,srtf`) or `all` |
| `--quantum N` | quantum for RR, MLQ and Lottery (default 4) |
| `--input FILE` | `ID arrival burst priority [deadline [io_every io_duration]]`. `#` starts a comment |
| `--random --num N` | N random tasks (default 10) |
| `--workload W` | `mixed` (default), `cpu`, `io`, `web` |
| `--io` | gives about half of the mixed random tasks I/O bursts |
| `--seed N` | makes a random run repeatable (the seed is always printed) |
| `--aging N`, `--age-limit N` | aging interval for Priority (default 3) and promotion limit for MLFQ (default 10). 0 turns either off |
| `--deadline-factor F` | automatic deadline = arrival + F × burst (default 2) |
| `--output FILE` | log file (default `simulation_log.txt`) |
| `--verbose` | when running 3+ schedulers, also print each full result |

Examples from the handout:
```bash
./simulator --scheduler rr --quantum 4
./simulator --scheduler rr --random
./simulator --scheduler srtf --random --num 20 --output log.txt
./simulator --scheduler rr,srtf --input tasks.txt       # side-by-side comparison
./simulator --scheduler all --input tasks_io.txt        # all 10 on the same input
```

### Design
- `Scheduler` is an abstract base class with a pure virtual `schedule(processes, gantt, total_time)`. It also
  provides `name()` and `notes()`, which reports enhancement details. Ten derived classes implement it.
  `makeScheduler()` is the only place that maps a name to a class. Adding an 11th scheduler means writing one
  class and adding one line there.
- Every scheduler gets a fresh copy of the same task list, so comparisons are fair.
- Every scheduler is metered the same way. The metrics are:
  - waiting, turnaround and response time
  - CPU utilization
  - throughput
  - context switches
  - deadline misses
  - average turnaround for I/O-bound vs CPU-bound tasks
- **I/O simulation (enhancement):**
  - A task with `io_every = k` blocks after every k units of CPU, for `io_duration` units.
  - While a task is blocked, it is not ready and the CPU runs something else (or sits IDLE).
  - Waiting time excludes I/O time.
  - All ten schedulers support this through the shared helpers `isReady`, `afterRun`, `collectWakeups` and `idleUntilNext`.
  - In MLFQ, a task that blocks before using its full quantum keeps its level. This is why interactive tasks stay at the top.
- **Real-world scenario (`--workload web`):** generates web-server requests with these mixes:

  | Request type | Share | Priority | Burst | Deadline (SLA) | I/O |
  |---|---|---|---|---|---|
  | API calls (`A..`) | 70% | 1 | 1–3 | arrival + 6 | none |
  | Page renders (`W..`) | 22% | 2 | 3–6 | arrival + 15 | database I/O |
  | Batch reports (`B..`) | 8% | 4 | 10–20 | arrival + 80 | database I/O |

  Requests arrive at random intervals of about 3 time units on average.
- **Input validation:** the simulator reports the file name and line number for these errors:
  - bad numbers
  - duplicate IDs
  - a burst of 0 or less
  - extra columns

  It also rejects unknown options, missing values and quanta below 1.

### Comparison results (all runs are reproducible with the commands shown)

**RR vs SRTF on interactive tasks** (`./simulator --scheduler rr,srtf --input tasks.txt`, 12 tasks):

| | Avg wait | Avg TAT | Avg response | Context switches |
|---|---|---|---|---|
| RR q=4 | 20.67 | 25.33 | 12.83 | 18 |
| SRTF | **11.08** | **15.75** | **10.58** | 12 |

SRTF is provably optimal for average waiting time, and it wins here. It has a cost, though:
- It needs to know each burst length in advance. Real systems have to estimate it.
- Long jobs can starve. T03 (burst 8) arrives at t=2 but doesn't start until t=39, after every shorter job.

RR needs no burst estimate and bounds each task's wait to (n−1)·q per round. That makes it predictable for
interactive users. The price is more context switches and a worse average.

**CPU-bound vs I/O-bound** (`--input tasks_io.txt`: 3 CPU hogs and 4 jobs that compute for 1 unit, then wait 4 for I/O):

| | CPU util | I/O-bound avg TAT | CPU-bound avg TAT | Avg response |
|---|---|---|---|---|
| FCFS | 87.1% | 60.75 | 27.00 | 22.71 |
| RR q=4 | 91.0% | 57.75 | 47.33 | 7.00 |
| MLFQ | **100%** | 31.75 | 57.33 | 3.00 |
| CFS | **100%** | 30.25 | 57.33 | **2.14** |
| SRTF | **100%** | **21.50** | 45.00 | 11.14 |

- **FCFS** has a convoy effect. Each I/O job waits behind a 12–15 unit CPU hog. When all the I/O jobs are
  blocked at once, the CPU sits idle (87% utilization).
- **MLFQ and CFS** keep I/O-bound tasks at the front. MLFQ does it because those tasks never use a full
  quantum. CFS does it because they build up little vruntime. As soon as an I/O completes, the task runs again
  and starts its next I/O, so CPU work and I/O overlap and the CPU reaches 100%.

Conclusions:
- **For I/O-bound or interactive work, use MLFQ, CFS or SRTF.** RR helps less than expected, because an
  I/O job still waits behind up to three 4-unit quanta for every 1 unit of CPU it uses.
- **For purely CPU-bound batch work** (`--workload cpu --num 15 --seed 4`), use SRTF, SJF or FCFS. These had
  the lowest average turnaround (81–99). Time-slicing schedulers (RR, MLFQ, CFS) raise the average turnaround
  to 145–153 because every job finishes late, although their response time is better.
- **For deadlines** (`tasks_deadlines.txt`), use EDF. It is the only scheduler that met all 8 deadlines
  (FCFS missed 4, RR 5, CFS 6), and the feasibility check confirms that a schedule meeting all of them exists.

## References
- Silberschatz, Galvin & Gagne, *Operating System Concepts*, Ch. 5 (CPU Scheduling).
- Arpaci-Dusseau, *Operating Systems: Three Easy Pieces*, chapters on MLFQ, Lottery and CFS.
- Linux kernel `sched/fair.c`, as a reference for CFS weights (`sched_prio_to_weight`, about 1.25× per nice level).
