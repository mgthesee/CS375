// priority_queue.cpp - Exercise 3: Thread-Safe Priority Queue
// Compile: g++ -o priority_queue priority_queue.cpp -pthread -std=c++11
//
// std::priority_queue keeps its largest element on top (a binary max-heap), so
// pop() always returns the highest priority currently in the queue. One mutex
// protects every operation, so the heap is never modified by two threads at once.
//
// Changes to the starter:
//  - Added missing headers <iostream> and <random>.
//  - rand() is not thread-safe (shared hidden state); each pusher now has its
//    own std::mt19937 generator with a fixed seed so runs are repeatable.
//  - The popper tried exactly 20 times and skipped a try when the queue was
//    empty, so some values could be left unpopped. It now keeps going until it
//    has popped all 20 values.
//  - Added a check at the end: the values popped after all pushers finished
//    must come out in descending order (highest priority first).
#include <iostream>
#include <queue>
#include <mutex>
#include <thread>
#include <vector>
#include <chrono>
#include <random>
#include <atomic>

std::mutex print_mtx;
#define PRINT(x) do { std::lock_guard<std::mutex> pl(print_mtx); std::cout << x << "\n"; } while (0)

template <typename T>
class ThreadSafePriorityQueue {
private:
    std::priority_queue<T> pq;
    std::mutex mtx;

public:
    void push(T value) {
        std::lock_guard<std::mutex> lock(mtx);
        pq.push(value);            // heap re-orders itself: O(log n)
    }

    bool pop(T& value) {
        std::lock_guard<std::mutex> lock(mtx);
        if (pq.empty()) return false;
        value = pq.top();          // highest priority
        pq.pop();
        return true;
    }

    bool empty() {
        std::lock_guard<std::mutex> lock(mtx);
        return pq.empty();
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(mtx);
        return pq.size();
    }
};

void priorityQueueTest() {
    ThreadSafePriorityQueue<int> pq;
    std::vector<std::thread> threads;
    const int NUM_THREADS = 4;
    const int PER_THREAD = 5;
    std::atomic<int> pushers_done(0);
    std::vector<int> after_pushers;   // values popped once all pushes are done

    auto pusher = [&](int id) {
        std::mt19937 gen(42 + id);    // per-thread generator (rand() is not thread-safe)
        for (int i = 0; i < PER_THREAD; ++i) {
            int priority = gen() % 100;
            pq.push(priority);
            PRINT("Thread " << id << " pushed: " << priority);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        pushers_done++;
    };

    auto popper = [&]() {
        int popped = 0;
        while (popped < NUM_THREADS * PER_THREAD) {
            int value;
            bool all_pushed = (pushers_done == NUM_THREADS);
            if (pq.pop(value)) {
                popped++;
                if (all_pushed) after_pushers.push_back(value);
                PRINT("    Popped: " << value << (all_pushed ? "  (all pushes done)" : ""));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    };

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(pusher, i);
    }
    threads.emplace_back(popper);

    for (auto& t : threads) t.join();

    bool sorted = true;
    for (size_t i = 1; i < after_pushers.size(); ++i)
        if (after_pushers[i] > after_pushers[i - 1]) sorted = false;
    std::cout << "Pushed " << NUM_THREADS * PER_THREAD << ", popped " << NUM_THREADS * PER_THREAD
              << ", left: " << pq.size() << ". After all pushes, " << after_pushers.size()
              << " values came out " << (sorted ? "in descending order (highest priority first)" : "OUT OF ORDER")
              << "\n";
}

int main() {
    priorityQueueTest();
    return 0;
}
