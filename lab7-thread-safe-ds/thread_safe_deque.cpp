// thread_safe_deque.cpp - Exercise 5: Thread-Safe Deque
// Compile: g++ -o thread_safe_deque thread_safe_deque.cpp -pthread -std=c++11
//
// One mutex protects the whole deque, so a push or pop at the front and a push
// or pop at the back can never run at the same time. Two separate locks (one
// per end) would allow more parallelism, but when the deque holds 0 or 1
// elements both ends are the SAME element: a pop_front and a pop_back could
// both take it, or one could read it while the other deletes it. std::deque
// can also reallocate its internal block map on a push at either end, which
// affects both ends. A single lock avoids all of that.
//
// Changes to the starter:
//  - Added missing <iostream>.
//  - The poppers tried 10 times each and skipped a try when the deque was
//    empty, so items could be left behind. They now pop until all 10 pushed
//    items are taken (a shared atomic counter), and the program checks that
//    every pushed value was popped exactly once.
//  - Added a stress test: 4 threads doing 20,000 mixed operations at both ends.
#include <iostream>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>
#include <algorithm>

std::mutex print_mtx;
#define PRINT(x) do { std::lock_guard<std::mutex> pl(print_mtx); std::cout << x << "\n"; } while (0)

template <typename T>
class ThreadSafeDeque {
private:
    std::deque<T> deque;
    std::mutex mtx;

public:
    void push_front(T value) {
        std::lock_guard<std::mutex> lock(mtx);
        deque.push_front(value);
    }

    void push_back(T value) {
        std::lock_guard<std::mutex> lock(mtx);
        deque.push_back(value);
    }

    bool pop_front(T& value) {
        std::lock_guard<std::mutex> lock(mtx);
        if (deque.empty()) return false;
        value = deque.front();
        deque.pop_front();
        return true;
    }

    bool pop_back(T& value) {
        std::lock_guard<std::mutex> lock(mtx);
        if (deque.empty()) return false;
        value = deque.back();
        deque.pop_back();
        return true;
    }

    bool empty() {
        std::lock_guard<std::mutex> lock(mtx);
        return deque.empty();
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(mtx);
        return deque.size();
    }
};

void dequeTest() {
    ThreadSafeDeque<int> dq;
    std::vector<std::thread> threads;
    const int TOTAL = 10;
    std::atomic<int> popped(0);
    std::vector<int> taken;            // every value popped, to check for duplicates/losses
    std::mutex taken_mtx;

    auto push_front = [&dq](int id) {
        for (int i = 0; i < 5; ++i) {
            dq.push_front(id * 100 + i);
            PRINT("Thread " << id << " pushed front: " << (id * 100 + i));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    };

    auto push_back = [&dq](int id) {
        for (int i = 0; i < 5; ++i) {
            dq.push_back(id * 100 + i);
            PRINT("Thread " << id << " pushed back:  " << (id * 100 + i));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    };

    auto pop_end = [&](bool front) {
        while (popped < TOTAL) {
            int value;
            if (front ? dq.pop_front(value) : dq.pop_back(value)) {
                popped++;
                { std::lock_guard<std::mutex> l(taken_mtx); taken.push_back(value); }
                PRINT("    Popped " << (front ? "front: " : "back:  ") << value);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    };

    threads.emplace_back(push_front, 1);
    threads.emplace_back(push_back, 2);
    threads.emplace_back(pop_end, true);
    threads.emplace_back(pop_end, false);

    for (auto& t : threads) t.join();

    std::sort(taken.begin(), taken.end());
    bool unique = std::adjacent_find(taken.begin(), taken.end()) == taken.end();
    std::cout << "Pushed " << TOTAL << ", popped " << taken.size() << ", left " << dq.size()
              << (unique ? " - every value popped exactly once" : " - DUPLICATE POP") << "\n";

    // Stress test: many operations at both ends at once
    ThreadSafeDeque<int> big;
    std::atomic<long> pushes(0), pops(0);
    std::vector<std::thread> workers;
    for (int w = 0; w < 4; ++w)
        workers.emplace_back([&, w] {
            for (int i = 0; i < 5000; ++i) {
                int v;
                switch ((i + w) % 4) {
                    case 0: big.push_front(i); pushes++; break;
                    case 1: big.push_back(i); pushes++; break;
                    case 2: if (big.pop_front(v)) pops++; break;
                    default: if (big.pop_back(v)) pops++; break;
                }
            }
        });
    for (auto& t : workers) t.join();
    std::cout << "Stress test (4 threads, 20000 ops at both ends): " << pushes << " pushes - " << pops
              << " pops = " << pushes - pops << ", deque size = " << big.size()
              << (pushes - pops == (long)big.size() ? " (consistent)" : " (INCONSISTENT)") << "\n";
}

int main() {
    dequeTest();
    return 0;
}
